// Synthetic System adapter: wire injection, result validation, and completion acknowledgement.
std::string m5;
struct M5Expected { SwitchNode::ReductionDescriptor descriptor; std::vector<uint64_t> result; };
std::map<uint64_t, M5Expected> m5Expected;
std::set<uint64_t> m5Completed, m5Forwarded;
std::set<uint64_t> m5SuccessSignals, m5TimeoutSignals;
void M5Status(uint64_t id, uint32_t state) {
    auto& signals = state == uint32_t(SwitchNode::ReductionStatus::COMPLETE)
                  ? m5SuccessSignals : m5TimeoutSignals;
    NS_ABORT_MSG_IF(!signals.insert(id).second, "Duplicate reduction terminal event");
}

uint64_t m5WireBytes = 0, m5OutputBytes = 0;

void M5Send(uint64_t id, uint32_t src, uint32_t nic, std::vector<uint64_t> values,
            bool wrongAddress = false)
{
    const auto& d = m5Expected.at(id).descriptor;
    std::vector<uint8_t> data(values.size() * 8);
    for (uint32_t i = 0; i < values.size(); ++i)
        for (uint32_t b = 0; b < 8; ++b) data[8*i+b] = uint8_t(values[i] >> (56-8*b));
    auto p = Create<Packet>(data.data(), data.size());
    MemOpHeader op; op.transaction = id; op.src = src; op.dst = d.resultGpu;
    op.address = d.address + (wrongAddress ? 1 : 0); op.size = d.size;
    op.opcode = MemOpHeader::REDUCE_SUM_U64; p->AddHeader(op);
    SeqTsHeader seq; seq.SetSeq(0); seq.SetPG(1); p->AddHeader(seq);
    UdpHeader udp; udp.SetSourcePort(9000); udp.SetDestinationPort(9000); p->AddHeader(udp);
    Ipv4Header ip; ip.SetSource(Ip(src)); ip.SetDestination(Ip(d.resultGpu));
    ip.SetProtocol(0x11); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    nics[nic]->RdmaEnqueueHighPrioQ(p); nics[nic]->TriggerTransmit();
}

MemOpHeader M5Result(Ptr<const Packet> wire, Ptr<Packet>& payload)
{
    payload = wire->Copy(); PppHeader ppp; Ipv4Header ip; UdpHeader udp; SeqTsHeader seq;
    payload->RemoveHeader(ppp); payload->RemoveHeader(ip); payload->RemoveHeader(udp);
    payload->RemoveHeader(seq); MemOpHeader op; payload->RemoveHeader(op);
    NS_ABORT_MSG_IF(op.opcode != MemOpHeader::REDUCE_RESULT || op.src != 8 ||
                    !m5Expected.count(op.transaction), "Invalid reduction result identity");
    const auto& e = m5Expected.at(op.transaction);
    NS_ABORT_MSG_IF(op.dst != e.descriptor.resultGpu || op.address != e.descriptor.address ||
                    op.size != e.descriptor.size || payload->GetSize() != op.size ||
                    ip.GetSource() != Ip(8) || ip.GetDestination() != Ip(op.dst), "Result metadata mismatch");
    std::vector<uint8_t> data(op.size); payload->CopyData(data.data(), data.size());
    for (uint32_t i = 0; i < e.result.size(); ++i) {
        uint64_t v = 0; for (uint32_t b = 0; b < 8; ++b) v = (v << 8) | data[i*8+b];
        NS_ABORT_MSG_IF(v != e.result[i], "Incorrect reduction sum");
    }
    return op;
}

int M5Receive(uint32_t gpu, Ptr<Packet> wire)
{
    Ptr<Packet> data; auto op = M5Result(wire, data);
    NS_ABORT_MSG_IF(gpu != op.dst, "Reduction reached wrong GPU");
    if (Simulator::Now() >= m5Expected.at(op.transaction).descriptor.deadline) {
        NS_ABORT_MSG_IF(fabric->CompleteReduction(op.transaction, gpu), "Late result completed");
        return 0;
    }
    NS_ABORT_MSG_IF(!m5Completed.insert(op.transaction).second ||
                    !fabric->CompleteReduction(op.transaction, gpu), "Duplicate/invalid completion");
    std::cout << "M5_RESULT tx=" << op.transaction << " elements=" << op.size / 8
              << " first=" << m5Expected.at(op.transaction).result[0]
              << " receipt_ns=" << Simulator::Now().GetNanoSeconds() << '\n';
    return 0;
}

void M5Forward(uint32_t gpu, Ptr<const Packet> wire, uint32_t queue)
{
    Ptr<Packet> data; auto op = M5Result(wire, data);
    NS_ABORT_MSG_IF(gpu != op.dst || queue != 1 || !m5Forwarded.insert(op.transaction).second,
                    "Duplicate/wrong reduction output");
    m5OutputBytes += wire->GetSize();
}

void M5NicTx(Ptr<const Packet> wire, uint32_t) { m5WireBytes += wire->GetSize(); }

bool M5Register(uint64_t id, std::set<uint32_t> members, std::vector<uint64_t> expected,
                 Time deadline)
{
    SwitchNode::ReductionDescriptor d{id, 0x500000 + id * 8192, members, 0,
                                      uint32_t(expected.size() * 8), deadline};
    m5Expected[id] = {d, expected};
    return fabric->RegisterReduction(d);
}

void M5Later(uint64_t id, uint32_t src, uint32_t nic, std::vector<uint64_t> data,
             uint32_t delay = 0, bool wrong = false)
{
    Simulator::Schedule(NanoSeconds(delay), &M5Send, id, src, nic, data, wrong);
}

void M5Second()
{
    NS_ABORT_MSG_IF(!M5Register(5001, {1}, {9}, MicroSeconds(30)), "Capacity not reclaimed");
    M5Send(5001, 1, 1, {9});
}

void M5Start()
{
    fabric->reductionEvents.ConnectWithoutContext(MakeCallback(&M5Status));
    if (m5 == "capacity") {
        NS_ABORT_MSG_IF(!fabric->SetReductionCapacity(1, 2) ||
                        !M5Register(5000, {1}, {7}, MicroSeconds(8)), "Capacity setup failed");
        NS_ABORT_MSG_IF(M5Register(5001, {1}, {9}, MicroSeconds(30)), "Active limit ignored");
        M5Send(5000, 1, 1, {7});
        Simulator::Schedule(MicroSeconds(10), &M5Second); return;
    }
    if (m5 != "normal") {
        Time deadline = m5 == "late-result" ? NanoSeconds(1700) : MicroSeconds(8);
        NS_ABORT_MSG_IF(!M5Register(5000, {1,2}, {3}, deadline), "Registration failed");
        M5Send(5000, 1, 1, {1});
        if (m5 == "missing") M5Later(5000, 2, 2, {2}, 10000);
        if (m5 == "result-boundary") M5Later(5000, 2, 2, {2}, 5998);
        if (m5 == "late-result") M5Send(5000, 2, 2, {2});
        if (m5 == "boundary") {
            // A 73-byte packet serializes in 1 ns plus 500 ns propagation.
            M5Later(5000, 2, 2, {2}, 6499);
        }
        return;
    }
    NS_ABORT_MSG_IF(!M5Register(5000, {1,2,3,4}, {10,100}, MicroSeconds(20)) ||
                    !M5Register(5001, {1,2}, {0}, MicroSeconds(20)) ||
                    !M5Register(5002, {1,2}, std::vector<uint64_t>(512, 3), MicroSeconds(20)) ||
                    !M5Register(5003, {1,2,3,4}, {10,100}, MicroSeconds(20)), "Registration failed");
    auto invalid = m5Expected.at(5000).descriptor;
    NS_ABORT_MSG_IF(fabric->RegisterReduction(invalid), "Reused ID accepted");
    invalid.transaction = 5999; invalid.size = 7;
    NS_ABORT_MSG_IF(fabric->RegisterReduction(invalid), "Invalid size accepted");
    for (uint32_t gpu : {4,2,3,1}) {
        M5Later(5000, gpu, gpu, {gpu, 10*gpu}, gpu * 100);
        M5Later(5003, gpu, gpu, {gpu, 10*gpu}, 1000 + gpu * 150);
    }
    M5Later(5001, 2, 2, {1}, 100);
    M5Later(5001, 1, 1, {UINT64_MAX}, 250);
    M5Later(5002, 2, 2, std::vector<uint64_t>(512, 2), 400);
    M5Later(5002, 1, 1, std::vector<uint64_t>(512, 1), 500);
    M5Later(5000, 1, 1, {999,999}, 250); // Duplicate before completion: first wins.
    M5Later(5000, 1, 1, {999,999}, 9000); // Duplicate after completion.
    M5Later(5003, 5, 5, {5,50}, 0); // Nonmember.
    M5Later(5003, 1, 2, {1,10}, 0); // Spoofed ingress.
    M5Later(5003, 1, 1, {1,10}, 0, true); // Wrong address.
    M5Later(5003, 1, 1, {1}, 100); // Truncated payload.
    auto unknown = m5Expected.at(5003); unknown.descriptor.transaction = 5998;
    m5Expected[5998] = unknown; M5Later(5998, 1, 1, {1,10}, 200);
}

bool M5Finish()
{
    using Status = SwitchNode::ReductionStatus;
    bool passed = true;
    if (m5 == "normal") {
        passed = m5Completed.size() == 4 && m5Forwarded.size() == 4 &&
                 fabric->reductionDuplicates >= 2 && fabric->reductionRejected >= 5;
        for (uint64_t id : {5000,5001,5002,5003})
            passed &= fabric->GetReductionStatus(id) == Status::COMPLETE;
    } else if (m5 == "capacity") {
        passed = m5Completed.size() == 2 && m5Forwarded.size() == 2 &&
                 !M5Register(5002, {1}, {1}, MicroSeconds(2000));
    } else {
        passed = m5Completed.empty() && fabric->GetReductionStatus(5000) == Status::TIMED_OUT &&
                 m5Forwarded.size() == ((m5 == "late-result" || m5 == "result-boundary") ? 1 : 0);
        if (m5 == "boundary") passed &= fabric->reductionRejected >= 1;
    }
    passed &= m5SuccessSignals == m5Completed;
    passed &= m5TimeoutSignals.size() == ((m5 == "normal" || m5 == "capacity") ? 0 : 1);
    passed &= fabric->reductionResultBytes == m5OutputBytes;
    std::cout << (passed ? "PASS" : "FAIL") << " M5 " << m5 << " completions=" << m5Completed.size()
              << " result_packets=" << m5Forwarded.size() << " duplicates=" << fabric->reductionDuplicates
              << " rejected=" << fabric->reductionRejected << " nic_bytes=" << m5WireBytes
              << " result_link_bytes=" << m5OutputBytes << '\n';
    return passed;
}
