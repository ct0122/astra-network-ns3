#ifndef NS3_MEMOP_HEADER_H
#define NS3_MEMOP_HEADER_H
#include "ns3/header.h"
namespace ns3 {
class MemOpHeader : public Header
{
public:
    enum : uint8_t { READ = 1, RESPONSE = 2, MULTICAST_READ = 3, REDUCE_SUM_U64 = 4, REDUCE_RESULT = 5 };
    uint64_t transaction = 0, address = 0;
    uint32_t src = 0, dst = 0, size = 0;
    uint8_t opcode = READ;
    static TypeId GetTypeId()
    {
        static TypeId tid = TypeId("ns3::MemOpHeader")
            .SetParent<Header>().AddConstructor<MemOpHeader>();
        return tid;
    }
    TypeId GetInstanceTypeId() const override { return GetTypeId(); }
    uint32_t GetSerializedSize() const override { return 29; }
    void Serialize(Buffer::Iterator i) const override
    {
        i.WriteHtonU64(transaction); i.WriteHtonU64(address);
        i.WriteHtonU32(src); i.WriteHtonU32(dst); i.WriteHtonU32(size);
        i.WriteU8(opcode);
    }
    uint32_t Deserialize(Buffer::Iterator i) override
    {
        transaction = i.ReadNtohU64(); address = i.ReadNtohU64();
        src = i.ReadNtohU32(); dst = i.ReadNtohU32(); size = i.ReadNtohU32();
        opcode = i.ReadU8(); return GetSerializedSize();
    }
    void Print(std::ostream& os) const override
    {
        os << "tx=" << transaction << " GPU" << src << "->GPU" << dst
           << " opcode=" << unsigned(opcode) << " address=" << address << " size=" << size;
    }
};

}
#endif
