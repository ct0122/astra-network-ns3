#include "switch-node.h"
#include "memop-header.h"
#include "ppp-header.h"
#include "ns3/flow-id-tag.h"
#include "ns3/simulator.h"
#include "ns3/udp-header.h"

namespace ns3 {

bool SwitchNode::SetReductionCapacity(uint32_t active, uint32_t records) {
    if (!m_reductions.empty() || active == 0 || records < active) return false;
    m_reductionActiveLimit = active; m_reductionRecordLimit = records; return true;
}

SwitchNode::ReductionStatus SwitchNode::GetReductionStatus(uint64_t id) {
    auto it = m_reductions.find(id);
    if (it == m_reductions.end()) return ReductionStatus::UNKNOWN;
    auto& record = it->second;
    if (record.status != ReductionStatus::COMPLETE && Simulator::Now() >= record.descriptor.deadline) {
        if (record.status != ReductionStatus::TIMED_OUT) {
            record.status = ReductionStatus::TIMED_OUT; std::vector<uint64_t>().swap(record.sum);
            reductionEvents(id, uint32_t(record.status));
        }
    }
    return record.status;
}

const SwitchNode::ReductionRecord* SwitchNode::GetReductionRecord(uint64_t id) const {
    auto it = m_reductions.find(id);
    return it == m_reductions.end() ? nullptr : &it->second;
}

bool SwitchNode::RegisterReduction(const ReductionDescriptor& d) {
    if (m_reductions.count(d.transaction) || m_reductions.size() >= m_reductionRecordLimit ||
        d.contributors.empty() || d.size == 0 || d.size > 4096 || d.size % 8 ||
        d.deadline <= Simulator::Now() || GetGpuPort(d.resultGpu) < 0 ||
        !ValidMemOpPort(GetGpuPort(d.resultGpu))) return false;
    uint32_t active = 0;
    for (auto& entry : m_reductions) {
        auto status = GetReductionStatus(entry.first);
        if (status != ReductionStatus::COMPLETE && status != ReductionStatus::TIMED_OUT) ++active;
    }
    if (active >= m_reductionActiveLimit) return false;
    ReductionRecord r; r.descriptor = d; r.outputPort = GetGpuPort(d.resultGpu);
    std::set<uint32_t> usedPorts;
    for (auto gpu : d.contributors) {
        int port = GetGpuPort(gpu);
        if (port < 0 || !ValidMemOpPort(port) || !usedPorts.insert(port).second) return false;
        r.ingressPorts[gpu] = port;
    }
    r.sum.resize(d.size / 8, 0);
    m_reductions.emplace(d.transaction, std::move(r));
    Simulator::Schedule(d.deadline - Simulator::Now(), &SwitchNode::ExpireReduction, this, d.transaction);
    return true;
}

void SwitchNode::ExpireReduction(uint64_t id) { GetReductionStatus(id); }

bool SwitchNode::CompleteReduction(uint64_t id, uint32_t gpu) {
    if (GetReductionStatus(id) != ReductionStatus::RESULT_QUEUED) return false;
    auto& r = m_reductions.at(id);
    if (gpu != r.descriptor.resultGpu) return false;
    r.status = ReductionStatus::COMPLETE; r.completed = Simulator::Now();
    reductionEvents(id, uint32_t(r.status)); return true;
}

void SwitchNode::HandleReduction(Ptr<NetDevice> device, Ptr<Packet> packet,
                                 CustomHeader& ch, const MemOpHeader& op) {
    reductionIngressBytes += packet->GetSize();
    auto status = GetReductionStatus(op.transaction);
    if (status == ReductionStatus::UNKNOWN || status == ReductionStatus::TIMED_OUT) {
        ++reductionRejected; return;
    }
    auto& r = m_reductions.at(op.transaction);
    auto member = r.ingressPorts.find(op.src);
    if (op.address != r.descriptor.address || op.size != r.descriptor.size ||
        op.dst != r.descriptor.resultGpu || member == r.ingressPorts.end() ||
        member->second != device->GetIfIndex()) { ++reductionRejected; return; }
    if (r.received.count(op.src)) { ++reductionDuplicates; return; }
    if (status == ReductionStatus::COMPLETE || status == ReductionStatus::RESULT_QUEUED) {
        ++reductionRejected; return;
    }
    auto payload = packet->Copy(); PppHeader ppp; Ipv4Header ip;
    payload->RemoveHeader(ppp); payload->RemoveHeader(ip);
    auto transport = payload->CreateFragment(0, CustomHeader::GetUdpHeaderSize());
    payload->RemoveAtStart(CustomHeader::GetUdpHeaderSize());
    MemOpHeader ignored; payload->RemoveHeader(ignored);
    std::vector<uint8_t> data(op.size); payload->CopyData(data.data(), data.size());
    for (uint32_t i = 0; i < r.sum.size(); ++i) {
        uint64_t value = 0;
        for (uint32_t byte = 0; byte < 8; ++byte) value = (value << 8) | data[i * 8 + byte];
        r.sum[i] += value; // Unsigned overflow is modulo 2^64.
    }
    if (r.received.empty()) r.first = Simulator::Now();
    r.last = Simulator::Now(); r.received.insert(op.src); r.status = ReductionStatus::COLLECTING;
    if (r.received.size() != r.descriptor.contributors.size()) return;
    for (uint32_t i = 0; i < r.sum.size(); ++i)
        for (uint32_t byte = 0; byte < 8; ++byte)
            data[i * 8 + byte] = uint8_t(r.sum[i] >> (56 - 8 * byte));
    auto result = Create<Packet>(data.data(), data.size());
    auto outOp = op; outOp.opcode = MemOpHeader::REDUCE_RESULT; outOp.src = GetId();
    result->AddHeader(outOp); transport->AddAtEnd(result);
    ip.SetSource(Ipv4Address(0x0a000001 + GetId()));
    ip.SetPayloadSize(transport->GetSize()); ip.SetTtl(64);
    transport->AddHeader(ip); transport->AddHeader(ppp);
    FlowIdTag tag; packet->PeekPacketTag(tag); transport->ReplacePacketTag(tag);
    r.status = ReductionStatus::RESULT_QUEUED; r.resultQueued = Simulator::Now(); std::vector<uint64_t>().swap(r.sum);
    if (!ValidMemOpPort(r.outputPort)) return; // Missing delivery expires at deadline.
    CustomHeader output(CustomHeader::L2_Header | CustomHeader::L3_Header | CustomHeader::L4_Header);
    transport->PeekHeader(output);
    SendToDev(transport, output, r.outputPort);
}
} // namespace ns3
