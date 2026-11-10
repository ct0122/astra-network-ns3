// M6 network-only driver. System calls the backend adapter; GPU memory is synthetic.
bool m6 = false;
uint32_t m6Count = 512, m6Window = 64, m6Size = 128;
uint64_t m6Period = 0, m6Timeout = 100000;
std::string m6Output = "build/memop-evaluation/single";
std::shared_ptr<MemOpNetworkAdapter> m6Adapter;
std::map<uint64_t, MemOpNetworkAdapter::Request> m6Submitted;
std::set<uint64_t> m6Callbacks;
std::array<Ptr<QbbNetDevice>, 8> m6Devices;
std::array<uint64_t, 8> m6QueueBytes{}, m6QueuePeaks{};
std::array<long double, 8> m6QueueIntegral{};
std::array<int64_t, 8> m6QueueTime{};
std::array<uint64_t, 8> m6NicQueueBytes{}, m6NicQueuePeaks{};
std::array<long double, 8> m6NicQueueIntegral{};
std::array<int64_t, 8> m6NicQueueTime{};
std::ofstream m6LatencyFile, m6QueueFile, m6TrafficFile, m6NicQueueFile;
std::vector<int64_t> m6Latencies;
uint32_t m6Accepted = 0, m6Success = 0, m6TimedOut = 0, m6Busy = 0, m6PeakActive = 0;
uint64_t m6NicBytes = 0, m6SwitchBytes = 0, m6NicPackets = 0, m6SwitchPackets = 0;
Time m6LastCallback, m6LastActivity, m6NextSubmit;
bool m6PumpScheduled = false;

MemOpHeader M6Decode(Ptr<const Packet> wire, Ptr<Packet>& data)
{
    data = wire->Copy(); PppHeader ppp; Ipv4Header ip; UdpHeader udp; SeqTsHeader seq;
    data->RemoveHeader(ppp); data->RemoveHeader(ip); data->RemoveHeader(udp); data->RemoveHeader(seq);
    MemOpHeader op; data->RemoveHeader(op);
    auto it = m6Submitted.find(op.transaction);
    NS_ABORT_MSG_IF(it == m6Submitted.end(), "Unknown evaluation transaction");
    const auto& r = it->second;
    bool reply = op.opcode == MemOpHeader::RESPONSE;
    NS_ABORT_MSG_IF((!reply && op.opcode != MemOpHeader::READ) || op.address != r.address ||
        op.size != r.size || op.src != (reply ? r.dst : r.src) || op.dst != (reply ? r.src : r.dst) ||
        ip.GetSource() != Ip(op.src) || ip.GetDestination() != Ip(op.dst) ||
        data->GetSize() != (reply ? op.size : 0), "Evaluation metadata mismatch");
    return op;
}

void M6Pump();
void M6SchedulePump(Time delay)
{
    if (m6PumpScheduled || m6Accepted == m6Count) return;
    m6PumpScheduled = true;
    Simulator::Schedule(delay, [] { m6PumpScheduled = false; M6Pump(); });
}

void M6Completion(const MemOpNetworkAdapter::Completion& result)
{
    NS_ABORT_MSG_IF(!m6Callbacks.insert(result.request.transaction).second, "Repeated completion callback");
    bool success = result.status == MemOpNetworkAdapter::Status::SUCCESS;
    if (success) {
        NS_ABORT_MSG_IF(result.data.size() != m6Size, "Incomplete callback payload");
        for (auto b : result.data) NS_ABORT_MSG_IF(b != 0xa5, "Corrupt callback payload");
        ++m6Success; m6Latencies.push_back((result.finished - result.accepted).GetNanoSeconds());
    } else { ++m6TimedOut; NS_ABORT_MSG_IF(!result.data.empty(), "Timeout returned partial data"); }
    m6LastCallback = result.finished;
    m6LatencyFile << result.request.transaction << ',' << result.request.src << ',' << result.request.dst
        << ',' << result.request.size << ',' << result.accepted.GetNanoSeconds() << ','
        << result.finished.GetNanoSeconds() << ',' << (success ? "success" : "timeout") << ','
        << (result.finished - result.accepted).GetNanoSeconds() << '\n';
    // A completed ID cannot be completed again, even from within a reentrant callback.
    MemOpHeader duplicate; duplicate.transaction = result.request.transaction;
    NS_ABORT_MSG_IF(m6Adapter->Receive(duplicate, {}), "Duplicate callback response accepted");
    M6SchedulePump(m6NextSubmit > Simulator::Now() ? m6NextSubmit - Simulator::Now() : NanoSeconds(0));
}

void M6Pump()
{
    while (m6Accepted < m6Count) {
        if (m6NextSubmit > Simulator::Now()) { M6SchedulePump(m6NextSubmit - Simulator::Now()); return; }
        uint32_t src = m6Accepted % 8;
        MemOpNetworkAdapter::Request r{6000 + m6Accepted, 0x600000 + uint64_t(m6Accepted)*8192,
                                      src, src == 7 ? 0u : 7u, m6Size,
                                      Simulator::Now() + NanoSeconds(m6Timeout)};
        auto status = m6Adapter->Submit(r, &M6Completion);
        if (status == MemOpNetworkAdapter::Admission::BUSY) { ++m6Busy; return; }
        NS_ABORT_MSG_IF(status != MemOpNetworkAdapter::Admission::ACCEPTED, "Unexpected submission rejection");
        m6Submitted.emplace(r.transaction, r); ++m6Accepted;
        m6PeakActive = std::max(m6PeakActive, m6Adapter->Outstanding());
        m6NextSubmit = Simulator::Now() + NanoSeconds(m6Period);
    }
}

int M6Receive(uint32_t gpu, Ptr<Packet> wire)
{
    m6LastActivity = Simulator::Now();
    Ptr<Packet> data; auto op = M6Decode(wire, data);
    NS_ABORT_MSG_IF(gpu != op.dst, "Wrong evaluation recipient");
    if (op.opcode == MemOpHeader::READ) {
        if (!dropResponse) {
            std::swap(op.src, op.dst); op.opcode = MemOpHeader::RESPONSE;
            Simulator::Schedule(NanoSeconds(100), &Send, op);
        }
    } else {
        std::vector<uint8_t> bytes(op.size); data->CopyData(bytes.data(), bytes.size());
        m6Adapter->Receive(op, bytes); // May reject a legitimately late response after timeout.
    }
    return 0;
}

void M6Queue(uint32_t gpu, uint64_t bytes, const char* event)
{
    auto now = Simulator::Now().GetNanoSeconds();
    m6QueueIntegral[gpu] += static_cast<long double>(m6QueueBytes[gpu]) * (now - m6QueueTime[gpu]);
    m6QueueBytes[gpu] = bytes; m6QueueTime[gpu] = now;
    m6QueuePeaks[gpu] = std::max(m6QueuePeaks[gpu], bytes);
    m6QueueFile << now << ',' << gpu << ',' << ports[gpu] << ',' << event << ',' << bytes << '\n';
    m6LastActivity = Simulator::Now();
}
void M6Enqueue(Ptr<QbbNetDevice> dev, Ptr<const Packet> wire, uint32_t)
{
    uint32_t gpu = 0; while (gpu < 8 && m6Devices[gpu] != dev) ++gpu;
    NS_ABORT_MSG_IF(gpu == 8, "Unknown evaluation port");
    M6Queue(gpu, dev->GetQueue()->GetNBytesTotal() + wire->GetSize(), "enqueue");
}
void M6Traffic(const char* kind, uint32_t gpu, Ptr<const Packet> wire)
{
    Ptr<Packet> data; auto op = M6Decode(wire, data);
    m6TrafficFile << Simulator::Now().GetNanoSeconds() << ',' << kind << ',' << gpu << ','
                 << op.transaction << ',' << unsigned(op.opcode) << ',' << wire->GetSize() << '\n';
    m6LastActivity = Simulator::Now();
}
void M6Dequeue(uint32_t gpu, Ptr<const Packet> wire, uint32_t queue)
{
    NS_ABORT_MSG_IF(queue != 1, "Wrong evaluation priority");
    M6Queue(gpu, m6Devices[gpu]->GetQueue()->GetNBytesTotal(), "dequeue");
    M6Traffic("switch", gpu, wire); ++m6SwitchPackets; m6SwitchBytes += wire->GetSize();
}
void M6NicQueue(uint32_t gpu, uint64_t bytes, const char* event)
{
    auto now = Simulator::Now().GetNanoSeconds();
    m6NicQueueIntegral[gpu] += static_cast<long double>(m6NicQueueBytes[gpu]) * (now - m6NicQueueTime[gpu]);
    m6NicQueueTime[gpu] = now; m6NicQueueBytes[gpu] = bytes;
    m6NicQueuePeaks[gpu] = std::max(m6NicQueuePeaks[gpu], bytes);
    m6NicQueueFile << now << ',' << gpu << ',' << event << ',' << bytes << '\n';
}
void M6NicEnqueue(uint32_t gpu, Ptr<const Packet> wire, uint32_t)
{
    M6NicQueue(gpu, nics[gpu]->GetRdmaQueue()->m_ackQ->GetNBytes() + wire->GetSize(), "enqueue");
}
void M6NicTx(uint32_t gpu, Ptr<const Packet> wire, uint32_t)
{
    M6NicQueue(gpu, nics[gpu]->GetRdmaQueue()->m_ackQ->GetNBytes(), "dequeue");
    M6Traffic("nic", gpu, wire); ++m6NicPackets; m6NicBytes += wire->GetSize();
}

void M6AdapterChecks()
{
    using Adapter = MemOpNetworkAdapter;
    uint32_t callbacks = 0;
    auto probe = std::make_shared<Adapter>([](const MemOpHeader&) {}, 1, 1);
    Adapter::Request r{123, 4096, 3, 7, 1, Simulator::Now() + MicroSeconds(1)};
    auto callback = [&callbacks](const Adapter::Completion& done) {
        NS_ABORT_MSG_IF(done.status != Adapter::Status::SUCCESS || done.data != std::vector<uint8_t>{42},
                        "Adapter guard test completion mismatch");
        ++callbacks;
    };
    NS_ABORT_MSG_IF(probe->Submit(r, callback) != Adapter::Admission::ACCEPTED || callbacks,
                    "Submit called completion synchronously");
    NS_ABORT_MSG_IF(probe->Submit(r, callback) != Adapter::Admission::DUPLICATE, "ID reuse accepted");
    auto other = r; ++other.transaction;
    NS_ABORT_MSG_IF(probe->Submit(other, callback) != Adapter::Admission::BUSY, "Capacity ignored");
    other.size = 0;
    NS_ABORT_MSG_IF(probe->Submit(other, callback) != Adapter::Admission::INVALID, "Invalid READ accepted");
    MemOpHeader reply; reply.transaction = r.transaction; reply.address = r.address;
    reply.src = r.dst; reply.dst = r.src; reply.size = r.size; reply.opcode = MemOpHeader::RESPONSE;
    auto wrong = reply; ++wrong.address;
    NS_ABORT_MSG_IF(probe->Receive(wrong, {42}) || probe->Receive(reply, {}) || callbacks,
                    "Invalid response completed a transaction");
    NS_ABORT_MSG_IF(!probe->Receive(reply, {42}) || callbacks != 1 || probe->Outstanding() != 0 ||
                    probe->Receive(reply, {42}), "Exactly-once completion failed");
    other.size = 1;
    NS_ABORT_MSG_IF(probe->Submit(other, callback) != Adapter::Admission::RECORD_LIMIT,
                    "Retained-record limit ignored");
}

void M6Start()
{
    M6AdapterChecks();
    std::filesystem::create_directories(m6Output);
    m6LatencyFile.open(m6Output + "/latency.csv"); m6QueueFile.open(m6Output + "/queue.csv");
    m6TrafficFile.open(m6Output + "/traffic.csv");
    m6NicQueueFile.open(m6Output + "/nic-queue.csv");
    NS_ABORT_MSG_IF(!m6LatencyFile || !m6QueueFile || !m6TrafficFile || !m6NicQueueFile, "Cannot open evaluation outputs");
    m6LatencyFile << "transaction,src,dst,size,accepted_ns,finished_ns,status,latency_ns\n";
    m6QueueFile << "time_ns,gpu,interface,event,waiting_bytes\n";
    m6TrafficFile << "time_ns,kind,gpu,transaction,opcode,bytes\n";
    m6NicQueueFile << "time_ns,gpu,event,waiting_bytes\n";
    for (auto& time : m6QueueTime) time = 1000;
    for (auto& time : m6NicQueueTime) time = 1000;
    m6Adapter = std::make_shared<MemOpNetworkAdapter>([](const MemOpHeader& op) { Send(op); }, m6Window);
    M6Pump();
    auto r = m6Submitted.at(6000);
    NS_ABORT_MSG_IF(m6Adapter->Submit(r, &M6Completion) != MemOpNetworkAdapter::Admission::DUPLICATE,
                    "Duplicate submit accepted");
    r.transaction = 999999; r.size = 0;
    NS_ABORT_MSG_IF(m6Adapter->Submit(r, &M6Completion) != MemOpNetworkAdapter::Admission::INVALID,
                    "Invalid submit accepted");
    r.size = m6Size;
    if (m6Adapter->Outstanding() == m6Window)
        NS_ABORT_MSG_IF(m6Adapter->Submit(r, &M6Completion) != MemOpNetworkAdapter::Admission::BUSY,
                        "Backpressure ignored");
    MemOpHeader bad; bad.transaction = 6000;
    NS_ABORT_MSG_IF(m6Adapter->Receive(bad, {}), "Invalid response accepted");
}

bool M6Finish()
{
    bool passed = m6Accepted == m6Count && m6Callbacks.size() == m6Count &&
                  m6Adapter->Outstanding() == 0 && m6Success + m6TimedOut == m6Count &&
                  m6PeakActive <= m6Window;
    if (!dropResponse && m6TimedOut == 0)
        passed &= m6NicPackets == uint64_t(m6Count)*2 && m6SwitchPackets == uint64_t(m6Count)*2;
    std::sort(m6Latencies.begin(), m6Latencies.end());
    auto percentile = [](uint32_t p) -> int64_t {
        return m6Latencies.empty() ? -1 : m6Latencies[(p*m6Latencies.size()+99)/100-1];
    };
    int64_t window = (m6LastCallback - MicroSeconds(1)).GetNanoSeconds();
    int64_t observationEnd = std::max(m6LastActivity, m6LastCallback).GetNanoSeconds();
    std::ofstream queues(m6Output + "/queue-summary.csv");
    queues << "gpu,interface,peak_waiting_bytes,time_mean_waiting_bytes\n";
    for (uint32_t gpu = 0; gpu < 8; ++gpu) {
        m6QueueIntegral[gpu] += static_cast<long double>(m6QueueBytes[gpu]) *
                               (observationEnd - m6QueueTime[gpu]);
        queues << gpu << ',' << ports[gpu] << ',' << m6QueuePeaks[gpu] << ','
               << double(m6QueueIntegral[gpu] / std::max<int64_t>(1, observationEnd-1000)) << '\n';
        passed &= m6QueueBytes[gpu] == 0;
    }
    std::ofstream nicQueues(m6Output + "/nic-queue-summary.csv");
    nicQueues << "gpu,peak_waiting_bytes,time_mean_waiting_bytes\n";
    for (uint32_t gpu = 0; gpu < 8; ++gpu) {
        m6NicQueueIntegral[gpu] += static_cast<long double>(m6NicQueueBytes[gpu]) *
                                  (observationEnd - m6NicQueueTime[gpu]);
        nicQueues << gpu << ',' << m6NicQueuePeaks[gpu] << ','
                  << double(m6NicQueueIntegral[gpu] / std::max<int64_t>(1, observationEnd-1000)) << '\n';
        passed &= m6NicQueueBytes[gpu] == 0;
    }
    std::ofstream summary(m6Output + "/summary.json");
    NS_ABORT_MSG_IF(!summary || !queues || !nicQueues, "Cannot write evaluation summaries");
    summary << "{\"accepted\":" << m6Accepted << ",\"success\":" << m6Success
            << ",\"timeouts\":" << m6TimedOut << ",\"busy_events\":" << m6Busy
            << ",\"peak_outstanding\":" << m6PeakActive << ",\"window_ns\":" << window
            << ",\"observation_end_ns\":" << observationEnd
            << ",\"payload_goodput_gbps\":" << double(uint64_t(m6Success)*m6Size*8)/std::max<int64_t>(1,window)
            << ",\"completed_ops_per_second\":" << double(m6Success)*1e9/std::max<int64_t>(1,window)
            << ",\"latency_p50_ns\":" << percentile(50) << ",\"latency_p95_ns\":" << percentile(95)
            << ",\"latency_p99_ns\":" << percentile(99) << ",\"nic_bytes\":" << m6NicBytes
            << ",\"switch_bytes\":" << m6SwitchBytes << ",\"total_link_bytes\":"
            << m6NicBytes+m6SwitchBytes << "}\n";
    std::cout << (passed ? "PASS" : "FAIL") << " M6 accepted=" << m6Accepted << " success=" << m6Success
              << " timeouts=" << m6TimedOut << " p95_ns=" << percentile(95)
              << " output=" << m6Output << '\n';
    m6LatencyFile.close(); m6QueueFile.close(); m6TrafficFile.close(); m6NicQueueFile.close();
    return passed;
}
