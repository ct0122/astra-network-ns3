// M4 endpoint stubs and counters. Included within the smoke test's namespace.
std::set<uint32_t> m4Requests, m4Responses, m4RequestForwards, m4ResponseForwards;
uint64_t m4SourceBytes = 0, m4SourcePackets = 0, m4NicBytes = 0, m4SwitchBytes = 0;
uint64_t m4NicPackets = 0, m4SwitchPackets = 0;
Time m4LastDelivery, m4FirstCompletion, m4LastCompletion;

MemOpHeader M4Decode(Ptr<const Packet> wire, Ptr<Packet>& payload)
{
    payload = wire->Copy();
    PppHeader ppp; Ipv4Header ip; UdpHeader udp; SeqTsHeader seq;
    payload->RemoveHeader(ppp); payload->RemoveHeader(ip);
    payload->RemoveHeader(udp); payload->RemoveHeader(seq);
    MemOpHeader op;
    NS_ABORT_MSG_IF(payload->GetSize() < op.GetSerializedSize(), "Truncated M4 header");
    payload->RemoveHeader(op);
    NS_ABORT_MSG_IF(op.transaction != 4000 || op.address != 0x400000 || op.size != readSize
                    || ip.GetSource() != Ip(op.src) || ip.GetDestination() != Ip(op.dst),
                    "M4 metadata mismatch");
    bool response = op.opcode == MemOpHeader::RESPONSE;
    if (response) {
        NS_ABORT_MSG_IF(op.src < 1 || op.src > fanout || op.dst != 0 ||
                        payload->GetSize() != readSize, "Invalid M4 response");
    } else {
        NS_ABORT_MSG_IF(op.src != 0 || payload->GetSize() != 0 ||
            (m4 == "multicast" ? op.opcode != MemOpHeader::MULTICAST_READ || op.dst != 100
                               : op.opcode != MemOpHeader::READ || op.dst < 1 || op.dst > fanout),
            "Invalid M4 request");
    }
    return op;
}

uint8_t M4Byte(const MemOpHeader& op, uint32_t offset)
{
    return uint8_t((op.transaction + op.address + op.src * 17 + offset) & 255);
}

void M4Send(MemOpHeader op)
{
    std::vector<uint8_t> bytes(op.opcode == MemOpHeader::RESPONSE ? op.size : 0);
    for (uint32_t i = 0; i < bytes.size(); ++i) bytes[i] = M4Byte(op, i);
    auto p = bytes.empty() ? Create<Packet>() : Create<Packet>(bytes.data(), bytes.size());
    p->AddHeader(op);
    SeqTsHeader seq; seq.SetSeq(0); seq.SetPG(1); p->AddHeader(seq);
    UdpHeader udp; udp.SetSourcePort(9000); udp.SetDestinationPort(9000); p->AddHeader(udp);
    Ipv4Header ip; ip.SetSource(Ip(op.src)); ip.SetDestination(Ip(op.dst));
    ip.SetProtocol(0x11); ip.SetTtl(64); ip.SetPayloadSize(p->GetSize()); p->AddHeader(ip);
    PppHeader ppp; ppp.SetProtocol(0x0021); p->AddHeader(ppp);
    nics[op.src]->RdmaEnqueueHighPrioQ(p); nics[op.src]->TriggerTransmit();
}

void M4Start()
{
    MemOpHeader op; op.transaction = 4000; op.address = 0x400000; op.size = readSize;
    op.src = 0;
    if (m4 == "multicast") {
        op.opcode = MemOpHeader::MULTICAST_READ; op.dst = 100; M4Send(op);
    } else {
        op.opcode = MemOpHeader::READ;
        for (uint32_t gpu = 1; gpu <= fanout; ++gpu) { op.dst = gpu; M4Send(op); }
    }
}

int M4Receive(uint32_t gpu, Ptr<Packet> wire)
{
    Ptr<Packet> payload; auto op = M4Decode(wire, payload);
    if (op.opcode != MemOpHeader::RESPONSE) {
        NS_ABORT_MSG_IF(gpu < 1 || gpu > fanout ||
                        (m4 == "unicast" && op.dst != gpu) ||
                        !m4Requests.insert(gpu).second, "Wrong/duplicate M4 delivery");
        m4LastDelivery = Simulator::Now();
        if (!dropResponse && gpu != dropResponder) {
            op.opcode = MemOpHeader::RESPONSE; op.src = gpu; op.dst = 0;
            Simulator::Schedule(NanoSeconds(100), &M4Send, op);
        }
    } else {
        NS_ABORT_MSG_IF(gpu != 0 || !m4Requests.count(op.src) ||
                        !m4Responses.insert(op.src).second, "Wrong/duplicate M4 completion");
        std::vector<uint8_t> bytes(op.size); payload->CopyData(bytes.data(), bytes.size());
        for (uint32_t i = 0; i < bytes.size(); ++i)
            NS_ABORT_MSG_IF(bytes[i] != M4Byte(op, i), "M4 data corruption");
        if (m4Responses.size() == 1) m4FirstCompletion = Simulator::Now();
        m4LastCompletion = Simulator::Now();
    }
    return 0;
}

void M4Forward(uint32_t gpu, Ptr<const Packet> wire, uint32_t queue)
{
    Ptr<Packet> payload; auto op = M4Decode(wire, payload);
    NS_ABORT_MSG_IF(queue != 1, "Wrong M4 queue");
    if (op.opcode == MemOpHeader::RESPONSE) {
        NS_ABORT_MSG_IF(gpu != 0 || !m4ResponseForwards.insert(op.src).second,
                        "Wrong/duplicate response forwarding");
    } else {
        NS_ABORT_MSG_IF(gpu < 1 || gpu > fanout || (m4 == "unicast" && op.dst != gpu) ||
                        !m4RequestForwards.insert(gpu).second, "Wrong/duplicate replication");
    }
    ++m4SwitchPackets; m4SwitchBytes += wire->GetSize();
}

void M4NicTx(uint32_t gpu, Ptr<const Packet> wire, uint32_t)
{
    Ptr<Packet> payload; auto op = M4Decode(wire, payload);
    NS_ABORT_MSG_IF(op.src != gpu, "Wrong sending NIC");
    ++m4NicPackets; m4NicBytes += wire->GetSize();
    if (gpu == 0) { ++m4SourcePackets; m4SourceBytes += wire->GetSize(); }
}

bool M4Finish()
{
    uint32_t sourceCount = m4 == "multicast" ? 1 : fanout;
    bool passed = m4Requests.size() == fanout && m4Responses.size() == fanout &&
        m4RequestForwards.size() == fanout && m4ResponseForwards.size() == fanout &&
        m4SourcePackets == sourceCount && m4NicPackets == sourceCount + fanout &&
        m4SwitchPackets == 2 * fanout;
    auto ns = [](Time t) { return (t - MicroSeconds(1)).GetNanoSeconds(); };
    std::cout << "M4_METRICS {\"mode\":\"" << m4 << "\",\"fanout\":" << fanout
              << ",\"read_size\":" << readSize << ",\"source_packets\":" << m4SourcePackets
              << ",\"source_bytes\":" << m4SourceBytes << ",\"nic_bytes\":" << m4NicBytes
              << ",\"switch_bytes\":" << m4SwitchBytes
              << ",\"total_link_bytes\":" << m4NicBytes + m4SwitchBytes
              << ",\"last_delivery_ns\":" << ns(m4LastDelivery)
              << ",\"first_completion_ns\":" << (m4Responses.empty() ? -1 : ns(m4FirstCompletion))
              << ",\"all_completion_ns\":" << (m4Responses.size() != fanout ? -1 : ns(m4LastCompletion))
              << "}\n";
    std::cout << (passed ? "PASS" : "FAIL") << " M4 " << m4 << " recipients=" << fanout
              << " deliveries=" << m4Requests.size() << " completions=" << m4Responses.size()
              << " switch_forwards=" << m4SwitchPackets << '\n';
    return passed;
}
