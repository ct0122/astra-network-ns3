// Standalone fabric acceptance test: real QBB links and the existing SwitchNode.
// GPU endpoints are deterministic memory stubs; no GPU execution or RDMA is modeled.
#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/qbb-helper.h"
#include "ns3/switch-node.h"
#include "ns3/seq-ts-header.h"
#include "ns3/ppp-header.h"
#include "ns3/memop-header.h"

#include <array>
#include <fstream>
#include <iostream>
#include <set>
#include <vector>
#include <map>
#include <algorithm>

using namespace ns3;


namespace {
std::array<Ptr<QbbNetDevice>, 8> nics;
std::array<uint32_t, 8> ports;
uint32_t requests = 0, responses = 0, forwards = 0;
Time completed;
int64_t minRttNs = INT64_MAX, maxRttNs = 0, totalRttNs = 0;
bool dropResponse = false, m2 = false, m3 = false;
Ptr<SwitchNode> fabric;
struct Transaction {
    MemOpHeader request;
    Time issued;
    bool received = false, complete = false;
    uint32_t requestForwards = 0, responseForwards = 0;
};
std::map<uint64_t, Transaction> transactions;
std::array<uint32_t, 8> outstanding{}, peakOutstanding{};
std::map<std::pair<uint64_t, uint8_t>, Time> enqueued;
uint32_t peakQueueBytes = 0, waitedPackets = 0, reorderedCompletions = 0;
int64_t maxQueueWaitNs = 0;

uint32_t ServiceGpu(const MemOpHeader& request)
{
    // Transaction 3000 explicitly redirects logical GPU3 to physical GPU5.
    return m3 && request.transaction == 3000 ? 5 : request.dst;
}

uint8_t DataByte(const MemOpHeader& op, uint32_t offset)
{
    return m2 ? uint8_t((op.transaction * 17 + op.address + offset) & 255) : 0xa5;
}

Ipv4Address Ip(uint32_t gpu) { return Ipv4Address(0x0a000001 + gpu); }

MemOpHeader Decode(Ptr<const Packet> wire, Ptr<Packet>& payload)
{
    payload = wire->Copy();
    PppHeader ppp; Ipv4Header ip; UdpHeader udp; SeqTsHeader seq;
    payload->RemoveHeader(ppp); payload->RemoveHeader(ip);
    payload->RemoveHeader(udp); payload->RemoveHeader(seq);
    MemOpHeader op;
    NS_ABORT_MSG_IF(payload->GetSize() < op.GetSerializedSize(), "Truncated MemOp");
    payload->RemoveHeader(op);
    NS_ABORT_MSG_IF(op.src >= 8 || op.dst >= 8 || ip.GetSource() != Ip(op.src)
                    || ip.GetDestination() != Ip(m3 ? (op.dst + 1) % 8 : op.dst), "MemOp/IP endpoint mismatch");
    auto found = transactions.find(op.transaction);
    NS_ABORT_MSG_IF(found == transactions.end(), "Unknown transaction ID");
    const auto& expected = found->second.request;
    NS_ABORT_MSG_IF(op.opcode != MemOpHeader::READ && op.opcode != MemOpHeader::RESPONSE,
                    "Unknown opcode");
    bool response = op.opcode == MemOpHeader::RESPONSE;
    NS_ABORT_MSG_IF(op.address != expected.address || op.size != expected.size
                    || op.src != (response ? ServiceGpu(expected) : expected.src)
                    || op.dst != (response ? expected.src : expected.dst),
                    "Transaction metadata changed in flight");
    return op;
}

void Send(MemOpHeader op)
{
    std::vector<uint8_t> bytes(op.opcode == MemOpHeader::RESPONSE ? op.size : 0, 0xa5);
    for (uint32_t i = 0; i < bytes.size(); ++i) bytes[i] = DataByte(op, i);
    Ptr<Packet> p = bytes.empty() ? Create<Packet>() : Create<Packet>(bytes.data(), bytes.size());
    p->AddHeader(op);
    SeqTsHeader seq; seq.SetSeq(0); seq.SetPG(1); p->AddHeader(seq);
    UdpHeader udp; udp.SetSourcePort(9000); udp.SetDestinationPort(9000); p->AddHeader(udp);
    Ipv4Header ip; ip.SetSource(Ip(op.src)); ip.SetDestination(Ip(m3 ? (op.dst + 1) % 8 : op.dst));
    ip.SetProtocol(0x11); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    std::cout << Simulator::Now().GetNanoSeconds() << " ns SEND " << op << '\n';
    // Use the NIC's existing control-packet injection queue, without an RDMA QP.
    // The switch still classifies the UDP packet into normal priority group 1.
    nics[op.src]->RdmaEnqueueHighPrioQ(p);
    nics[op.src]->TriggerTransmit();
}

int Receive(uint32_t gpu, Ptr<Packet> wire, CustomHeader&)
{
    Ptr<Packet> payload;
    auto op = Decode(wire, payload);
    NS_ABORT_MSG_IF(gpu != (op.opcode == MemOpHeader::READ ? ServiceGpu(op) : op.dst),
                    "Delivered to wrong GPU");
    std::cout << Simulator::Now().GetNanoSeconds() << " ns RECEIVE " << op << '\n';
    auto& tx = transactions.at(op.transaction);
    if (op.opcode == MemOpHeader::READ)
    {
        NS_ABORT_MSG_IF(tx.received || payload->GetSize() != 0, "Invalid/duplicate READ");
        tx.received = true; ++requests;
        if (dropResponse) return 0;
        std::swap(op.src, op.dst); op.src = gpu; op.opcode = MemOpHeader::RESPONSE;
        // Deliberately vary service delay so completion cannot rely on FIFO order.
        Simulator::Schedule(NanoSeconds(m2 ? 100 + (7 - op.transaction % 8) * 200 : 100),
                            &Send, op);
    }
    else
    {
        NS_ABORT_MSG_IF(!tx.received || tx.complete || payload->GetSize() != op.size,
                        "Invalid/duplicate response");
        std::vector<uint8_t> bytes(op.size); payload->CopyData(bytes.data(), bytes.size());
        for (uint32_t i = 0; i < bytes.size(); ++i)
            NS_ABORT_MSG_IF(bytes[i] != DataByte(op, i), "READ data corrupted/mismatched");
        for (const auto& [id, earlier] : transactions)
            if (id < op.transaction && earlier.request.src == gpu && !earlier.complete)
            { ++reorderedCompletions; break; }
        tx.complete = true; ++responses; --outstanding[gpu];
        completed = Simulator::Now();
        auto rtt = (completed - tx.issued).GetNanoSeconds();
        minRttNs = std::min(minRttNs, rtt); maxRttNs = std::max(maxRttNs, rtt);
        totalRttNs += rtt;
    }
    return 0;
}

void Forward(uint32_t gpu, Ptr<const Packet> wire, uint32_t queue)
{
    Ptr<Packet> payload;
    auto op = Decode(wire, payload);
    NS_ABORT_MSG_IF((op.opcode == MemOpHeader::READ ? ServiceGpu(op) : op.dst) != gpu || queue != 1, "Wrong switch output port/queue");
    auto& tx = transactions.at(op.transaction);
    auto& count = op.opcode == MemOpHeader::READ ? tx.requestForwards : tx.responseForwards;
    NS_ABORT_MSG_IF(++count != 1, "Duplicate switch forwarding");
    auto key = std::make_pair(op.transaction, op.opcode);
    auto queued = enqueued.find(key);
    NS_ABORT_MSG_IF(queued == enqueued.end(), "Missing switch enqueue event");
    auto wait = (Simulator::Now() - queued->second).GetNanoSeconds();
    if (wait > 0) ++waitedPackets;
    maxQueueWaitNs = std::max(maxQueueWaitNs, wait);
    enqueued.erase(queued);
    ++forwards;
    std::cout << Simulator::Now().GetNanoSeconds() << " ns SWITCH_FORWARD port="
              << ports[gpu] << " " << op << '\n';
}

void Enqueue(Ptr<QbbNetDevice> dev, Ptr<const Packet> wire, uint32_t queue)
{
    Ptr<Packet> payload; auto op = Decode(wire, payload);
    NS_ABORT_MSG_IF(queue != 1, "Unexpected queue");
    NS_ABORT_MSG_IF(!enqueued.emplace(std::make_pair(op.transaction, op.opcode),
                                     Simulator::Now()).second, "Duplicate enqueue");
    // QbbEnqueue fires immediately before the actual queue insertion.
    peakQueueBytes = std::max(peakQueueBytes, dev->GetQueue()->GetNBytesTotal() + wire->GetSize());
}

void Issue(MemOpHeader op)
{
    NS_ABORT_MSG_IF(!transactions.emplace(op.transaction,
                        Transaction{op, Simulator::Now()}).second, "Duplicate issued ID");
    peakOutstanding[op.src] = std::max(peakOutstanding[op.src], ++outstanding[op.src]);
    Send(op);
}

void Start()
{
    if (m3)
    {
        for (uint32_t gpu = 0; gpu < 8; ++gpu)
        {
            MemOpHeader op; op.transaction = 3000 + gpu;
            op.src = gpu; op.dst = (gpu + 3) % 8;
            op.size = 128 * (gpu + 1); op.address = 0x300000 + gpu * 4096;
            if (gpu == 0)
                NS_ABORT_MSG_IF(!fabric->BindMemOpTransaction(op.transaction, MemOpHeader::READ,
                                                             ports[5]), "Rule bind failed");
            Issue(op);
        }
        return;
    }
    if (!m2) {
        MemOpHeader op; op.transaction = 729; op.address = 0x1000;
        op.src = 3; op.dst = 7; op.size = 64;
        Issue(op); return;
    }
    const std::array<uint32_t, 8> sizes = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    // All eight GPUs issue eight requests at the same simulation timestamp.
    // Seven sources converge on GPU7; GPU7 sends to GPU0 (no local reads).
    for (uint32_t src = 0; src < 8; ++src)
        for (uint32_t i = 0; i < sizes.size(); ++i)
        {
            MemOpHeader op;
            op.transaction = 1000 + src * 8 + i;
            op.src = src; op.dst = src == 7 ? 0 : 7;
            op.size = sizes[i]; op.address = 0x100000 + op.transaction * 8192;
            Issue(op);
        }
}
} // namespace

int main(int argc, char** argv)
{
    std::string topology = "scratch/topology/8_nodes_1_switch_topology.txt";
    CommandLine cmd; cmd.AddValue("topology", "Existing 8-GPU star topology file", topology);
    cmd.AddValue("dropResponse", "Negative control: suppress GPU7 response (expect FAIL)", dropResponse);
    cmd.AddValue("m2", "Run 64 concurrent variable-size transactions with hotspot contention", m2);
    cmd.AddValue("m3", "Test metadata forwarding and programmable bindings", m3);
    cmd.Parse(argc, argv);
    NS_ABORT_MSG_IF(m2 && m3, "Select one milestone");
    std::ifstream input(topology);
    uint32_t nodes = 0, switches = 0, links = 0, switchId = 0;
    NS_ABORT_MSG_IF(!(input >> nodes >> switches >> links >> switchId)
                    || nodes != 9 || switches != 1 || links != 8 || switchId != 8,
                    "Expected 9 nodes, one switch (8), eight links: " << topology);
    NodeContainer gpus; gpus.Create(8);
    Ptr<SwitchNode> sw = CreateObject<SwitchNode>();
    fabric = sw;
    sw->EnableMemOpForwarding(m3);
    NodeContainer all(gpus); all.Add(sw);
    InternetStackHelper internet; internet.Install(all);
    sw->m_mmu->node_id = sw->GetId();
    sw->m_mmu->ConfigBufferSize(32 * 1024 * 1024);
    std::set<uint32_t> connected;
    for (uint32_t link = 0; link < links; ++link)
    {
        uint32_t a, b; std::string rate, delay; double error;
        NS_ABORT_MSG_IF(!(input >> a >> b >> rate >> delay >> error), "Invalid link record");
        if (b == switchId) std::swap(a, b);
        NS_ABORT_MSG_IF(a != switchId || b >= 8 || !connected.insert(b).second || error != 0,
                        "Expected eight unique GPU links with zero error rate");
        QbbHelper qbb;
        qbb.SetDeviceAttribute("DataRate", StringValue(rate));
        qbb.SetChannelAttribute("Delay", StringValue(delay));
        auto devices = qbb.Install(sw, gpus.Get(b));
        auto out = DynamicCast<QbbNetDevice>(devices.Get(0));
        nics[b] = DynamicCast<QbbNetDevice>(devices.Get(1));
        ports[b] = out->GetIfIndex();
        NS_ABORT_MSG_IF(!sw->BindGpuPort(b, ports[b]), "GPU bind failed");
        auto ip = Ip(b); sw->AddTableEntry(ip, ports[b]);
        sw->m_mmu->ConfigHdrm(ports[b], 65536);
        sw->m_mmu->pfc_a_shift[ports[b]] = 3;
        sw->m_mmu->ConfigEcn(ports[b], 800, 3200, 0.2);
        nics[b]->GetRdmaQueue()->m_qpGrp = CreateObject<RdmaQueuePairGroup>();
        nics[b]->m_rdmaReceiveCb = MakeBoundCallback(&Receive, b);
        NS_ABORT_MSG_IF(!out->TraceConnectWithoutContext("QbbDequeue", MakeBoundCallback(&Forward, b)),
                        "Cannot attach forwarding observer");
        NS_ABORT_MSG_IF(!out->TraceConnectWithoutContext("QbbEnqueue", MakeBoundCallback(&Enqueue, out)),
                        "Cannot attach queue observer");
        std::cout << "BIND GPU" << b << " -> switch " << sw->GetId()
                  << " interface " << ports[b] << " " << rate << " " << delay << '\n';
    }
    sw->m_mmu->ConfigNPort(8);
    if (m3)
    {
        NS_ABORT_MSG_IF(sw->BindGpuPort(0, 0) || sw->BindGpuPort(0, 9999)
                        || sw->BindMemOpTransaction(1, MemOpHeader::READ, 9999),
                        "Invalid port was accepted");
        NS_ABORT_MSG_IF(!sw->BindGpuPort(0, ports[1]) || sw->GetGpuPort(0) != int(ports[1])
                        || !sw->BindGpuPort(0, ports[0]), "Rebinding failed");
        NS_ABORT_MSG_IF(!sw->BindMemOpTransaction(3000, MemOpHeader::READ, ports[6]),
                        "Initial transaction bind failed");
        // Start() replaces this rule with GPU5; check replacement semantics in flight.
        for (uint32_t gpu = 0; gpu < 8; ++gpu)
            NS_ABORT_MSG_IF(sw->GetGpuPort(gpu) != int(ports[gpu]), "GPU map changed");
    }
    Simulator::Schedule(MicroSeconds(1), &Start);
    Simulator::Stop(MilliSeconds(1));
    Simulator::Run();
    uint32_t expected = m2 ? 64 : (m3 ? 8 : 1);
    bool passed = requests == expected && responses == expected && forwards == 2 * expected
                  && enqueued.empty() && transactions.size() == expected;
    for (const auto& [id, tx] : transactions)
        passed &= tx.received && tx.complete && tx.requestForwards == 1 && tx.responseForwards == 1;
    if (m3)
    {
        passed &= sw->GetMemOpRuleHits() == 1 && sw->GetMemOpMapHits() == 15;
        for (auto count : outstanding) passed &= count == 0;
        std::cout << (passed ? "PASS" : "FAIL") << " M3 transactions=8 requests=" << requests
                  << " responses=" << responses << " switch_forwards=" << forwards
                  << " rule_hits=" << sw->GetMemOpRuleHits()
                  << " mapping_hits=" << sw->GetMemOpMapHits() << '\n';
        Simulator::Destroy(); return passed ? 0 : 1;
    }
    if (m2)
    {
        for (uint32_t gpu = 0; gpu < 8; ++gpu)
        {
            passed &= outstanding[gpu] == 0 && peakOutstanding[gpu] == 8;
            std::cout << "GPU" << gpu << " peak_outstanding=" << peakOutstanding[gpu]
                      << " remaining=" << outstanding[gpu] << '\n';
        }
        passed &= waitedPackets > 0 && maxQueueWaitNs > 0 && peakQueueBytes > 0
                  && reorderedCompletions > 0;
        std::cout << "queue_peak_bytes=" << peakQueueBytes << " waited_packets=" << waitedPackets
                  << " max_queue_wait_ns=" << maxQueueWaitNs
                  << " reordered_completions=" << reorderedCompletions << '\n';
        if (responses) std::cout << "rtt_ns min=" << minRttNs << " mean="
                                 << double(totalRttNs) / responses << " max=" << maxRttNs << '\n';
        std::cout << (passed ? "PASS" : "FAIL") << " M2 transactions=" << expected
                  << " requests=" << requests << " responses=" << responses
                  << " switch_forwards=" << forwards << '\n';
        Simulator::Destroy(); return passed ? 0 : 1;
    }
    std::cout << (passed ? "PASS" : "FAIL") << " GPU3 READ GPU7 tx=729 requests=" << requests
              << " responses=" << responses << " switch_forwards=" << forwards << '\n';
    if (passed) std::cout << "READ round-trip=" << (completed - MicroSeconds(1)).GetNanoSeconds()
                          << " ns (includes 100 ns memory stub delay)\n";
    Simulator::Destroy();
    return passed ? 0 : 1;
}
