#ifndef NS3_MEMOP_NETWORK_ADAPTER_H
#define NS3_MEMOP_NETWORK_ADAPTER_H
#include "memop-header.h"
#include "ns3/simulator.h"
#include <functional>
#include <memory>
#include <map>
#include <vector>

namespace ns3 {
// System-facing READ boundary. Construct with make_shared; transport uses simulation time.
// This adapter is independent of the smoke driver's transaction ledger.
class MemOpNetworkAdapter : public std::enable_shared_from_this<MemOpNetworkAdapter> {
public:
    struct Request {
        uint64_t transaction, address;
        uint32_t src, dst, size;
        Time deadline; // absolute; covers destination receipt
    };
    enum class Admission { ACCEPTED, BUSY, INVALID, DUPLICATE, RECORD_LIMIT };
    enum class Status { SUCCESS, TIMED_OUT };
    struct Completion {
        Request request;
        Status status;
        Time accepted, finished;
        std::vector<uint8_t> data;
    };
    using Callback = std::function<void(const Completion&)>;
    using Transport = std::function<void(const MemOpHeader&)>;
    MemOpNetworkAdapter(Transport transport, uint32_t maxOutstanding = 64,
                         uint32_t maxRecords = 65536)
        : m_transport(std::move(transport)), m_limit(maxOutstanding), m_recordLimit(maxRecords) {}

    Admission Submit(const Request& request, Callback completion) {
        if (!m_transport || !completion || m_limit == 0 || request.src >= 8 || request.dst >= 8 ||
            request.size == 0 || request.size > 4096 || request.deadline <= Simulator::Now())
            return Admission::INVALID;
        if (m_records.count(request.transaction)) return Admission::DUPLICATE;
        if (m_active >= m_limit) return Admission::BUSY;
        if (m_records.size() >= m_recordLimit) return Admission::RECORD_LIMIT;
        auto weak = weak_from_this();
        if (weak.expired()) return Admission::INVALID;
        Record record{request, Simulator::Now(), std::move(completion), false, EventId()};
        record.timeout = Simulator::Schedule(request.deadline - Simulator::Now(), [weak, id=request.transaction] {
            if (auto self = weak.lock()) self->Finish(id, Status::TIMED_OUT, {});
        });
        m_records.emplace(request.transaction, std::move(record)); ++m_active;
        // Submission acceptance does not mean NIC transmission. Never callback inside Submit.
        Simulator::ScheduleNow([weak, id=request.transaction] {
            if (auto self = weak.lock()) {
                auto it = self->m_records.find(id);
                if (it == self->m_records.end() || it->second.done) return;
                const auto& r = it->second.request;
                MemOpHeader op; op.transaction = id; op.address = r.address;
                op.src = r.src; op.dst = r.dst; op.size = r.size; op.opcode = MemOpHeader::READ;
                self->m_transport(op);
            }
        });
        return Admission::ACCEPTED;
    }

    bool Receive(const MemOpHeader& response, const std::vector<uint8_t>& data) {
        auto it = m_records.find(response.transaction);
        if (it == m_records.end() || it->second.done) { ++rejectedResponses; return false; }
        const auto& r = it->second.request;
        if (response.opcode != MemOpHeader::RESPONSE || response.src != r.dst ||
            response.dst != r.src || response.address != r.address || response.size != r.size ||
            data.size() != r.size) { ++rejectedResponses; return false; }
        if (Simulator::Now() >= r.deadline) {
            Finish(response.transaction, Status::TIMED_OUT, {}); ++rejectedResponses; return false;
        }
        Finish(response.transaction, Status::SUCCESS, data); return true;
    }
    uint32_t Outstanding() const { return m_active; }
    uint64_t rejectedResponses = 0;
private:
    struct Record { Request request; Time accepted; Callback callback; bool done; EventId timeout; };
    std::map<uint64_t, Record> m_records; // Tombstones retain IDs; capped by maxRecords.
    Transport m_transport;
    uint32_t m_limit, m_recordLimit, m_active = 0;
    void Finish(uint64_t id, Status status, std::vector<uint8_t> data) {
        auto it = m_records.find(id);
        if (it == m_records.end() || it->second.done) return;
        auto& record = it->second;
        Completion result{record.request, status, record.accepted, Simulator::Now(), std::move(data)};
        auto callback = std::move(record.callback);
        record.done = true; record.timeout.Cancel(); --m_active;
        callback(result); // State is final before invoking a potentially reentrant System callback.
    }
};
} // namespace ns3
#endif
