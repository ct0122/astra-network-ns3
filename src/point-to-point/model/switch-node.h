#ifndef SWITCH_NODE_H
#define SWITCH_NODE_H

#include <unordered_map>
#include <map>
#include <set>
#include <ns3/node.h>
#include "qbb-net-device.h"
#include "switch-mmu.h"
#include "pint.h"

namespace ns3 {

class Packet;
class MemOpHeader;

class SwitchNode : public Node{
	static const uint32_t pCnt = 1025;	// Number of ports used
	static const uint32_t qCnt = 8;	// Number of queues/priorities used
	uint32_t m_ecmpSeed;
	std::unordered_map<uint32_t, std::vector<int> > m_rtTable; // map from ip address (u32) to possible ECMP port (index of dev)

    bool m_memOpEnabled = false;
    std::unordered_map<uint32_t, uint32_t> m_gpuPorts;
    std::map<std::pair<uint64_t, uint8_t>, uint32_t> m_memOpRules;
    uint64_t m_memOpRuleHits = 0, m_memOpMapHits = 0;
    std::map<uint32_t, std::vector<uint32_t>> m_multicastGroups;
    bool ParseMemOp(Ptr<const Packet> packet, MemOpHeader& op) const;
    bool ValidMemOpPort(uint32_t port) const;

	// monitor of PFC
	uint32_t m_bytes[pCnt][pCnt][qCnt]; // m_bytes[inDev][outDev][qidx] is the bytes from inDev enqueued for outDev at qidx
	
	uint64_t m_txBytes[pCnt]; // counter of tx bytes

	uint32_t m_lastPktSize[pCnt];
	uint64_t m_lastPktTs[pCnt]; // ns
	double m_u[pCnt];

protected:
	bool m_ecnEnabled;
	uint32_t m_ccMode;
	uint64_t m_maxRtt;

	uint32_t m_ackHighPrio; // set high priority for ACK/NACK

private:
	int GetOutDev(Ptr<const Packet>, CustomHeader &ch);
	void SendToDev(Ptr<Packet>p, CustomHeader &ch, int forcedPort = -1);
	static uint32_t EcmpHash(const uint8_t* key, size_t len, uint32_t seed);
	void CheckAndSendPfc(uint32_t inDev, uint32_t qIndex);
	void CheckAndSendResume(uint32_t inDev, uint32_t qIndex);
public:
    struct ReductionDescriptor {
        uint64_t transaction, address;
        std::set<uint32_t> contributors;
        uint32_t resultGpu, size;
        Time deadline;
    };
    enum class ReductionStatus { UNKNOWN, REGISTERED, COLLECTING, RESULT_QUEUED, COMPLETE, TIMED_OUT };
    struct ReductionRecord {
        ReductionDescriptor descriptor;
        ReductionStatus status = ReductionStatus::REGISTERED;
        std::set<uint32_t> received;
        std::map<uint32_t, uint32_t> ingressPorts;
        uint32_t outputPort;
        std::vector<uint64_t> sum;
        Time first, last, resultQueued, completed;
    };
    bool RegisterReduction(const ReductionDescriptor& descriptor);
    ReductionStatus GetReductionStatus(uint64_t transaction);
    bool CompleteReduction(uint64_t transaction, uint32_t receivingGpu);
    const ReductionRecord* GetReductionRecord(uint64_t transaction) const;
    bool SetReductionCapacity(uint32_t active, uint32_t records);
    TracedCallback<uint64_t, uint32_t> reductionEvents; // COMPLETE or TIMED_OUT; adapter acknowledges receipt.
    uint64_t reductionRejected = 0, reductionDuplicates = 0;
    uint64_t reductionIngressBytes = 0, reductionResultBytes = 0;
private:
    std::map<uint64_t, ReductionRecord> m_reductions;
    uint32_t m_reductionActiveLimit = 16, m_reductionRecordLimit = 128;
    void ExpireReduction(uint64_t transaction);
    void HandleReduction(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader& ch,
                         const MemOpHeader& op);
public:
	Ptr<SwitchMmu> m_mmu;

	static TypeId GetTypeId (void);
	SwitchNode();
	void SetEcmpSeed(uint32_t seed);
	void AddTableEntry(Ipv4Address &dstAddr, uint32_t intf_idx);
	void ClearTable();
    // Ports are actual QBB interface indices, not logical GPU IDs.
    void EnableMemOpForwarding(bool enabled) { m_memOpEnabled = enabled; }
    bool BindGpuPort(uint32_t gpu, uint32_t port);
    int GetGpuPort(uint32_t gpu) const;
    bool BindMemOpTransaction(uint64_t transaction, uint8_t opcode, uint32_t port);
    bool BindMemOpMulticastGroup(uint32_t group, const std::vector<uint32_t>& gpus);
    void ClearMemOpRules() { m_memOpRules.clear(); }
    uint64_t GetMemOpRuleHits() const { return m_memOpRuleHits; }
    uint64_t GetMemOpMapHits() const { return m_memOpMapHits; }
	bool SwitchReceiveFromDevice(Ptr<NetDevice> device, Ptr<Packet> packet, CustomHeader &ch);
	void SwitchNotifyDequeue(uint32_t ifIndex, uint32_t qIndex, Ptr<Packet> p);

	// for approximate calc in PINT
	int logres_shift(int b, int l);
	int log2apprx(int x, int b, int m, int l); // given x of at most b bits, use most significant m bits of x, calc the result in l bits
};

} /* namespace ns3 */

#endif /* SWITCH_NODE_H */
