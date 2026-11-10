# M5 operation contract — proposal for System / Network alignment

Status: implemented for standalone Network M5 after user authorization.
System integration/sign-off remains pending; the fake System adapter exercises
this contract without requiring Compute or real GPU memory. M1–M5 are runnable.

## Operation and result

The first operation is element-wise SUM over an array of unsigned 64-bit
integers. Arithmetic wraps modulo 2^64; there is no saturation, floating point,
signed arithmetic, average, or implicit conversion. Payload integers use network
byte order (big endian). All contributors provide the same vector length.
`size` is payload bytes, must be a positive multiple of 8, and is initially
limited to 4096 bytes (one packet, without fragmentation/reassembly).

For contributors C and element i:

```
result[i] = sum(input[gpu][i] for gpu in C) mod 2^64
```

Contributors may arrive in any order. Exactly one result packet is generated
after one valid contribution from every configured contributor. No partial
result is emitted. The result is a network completion, not a GPU memory write
or an instruction to resume execution; System decides those effects.

## Configuration boundary

System must register an immutable descriptor before injecting contributions:

```
ReductionDescriptor {
    transaction_id: uint64,      // globally unique for the simulation run
    operation: SUM_U64,
    contributors: set<GPU_ID>,   // nonempty, distinct, known GPU bindings
    result_gpu: GPU_ID,          // known binding; may also be a contributor
    address: uint64,             // opaque correlation token; no memory side effect
    size_bytes: uint32,          // 8..4096, multiple of 8
    deadline_ns: uint64          // absolute ns-3 time, greater than registration time
}
```

Implemented APIs: `RegisterReduction(ReductionDescriptor)`,
`GetReductionStatus(id)`, `GetReductionRecord(id)`, and
`CompleteReduction(id, receivingGpu)` after adapter validation. `reductionEvents`
reports COMPLETE/TIMED_OUT exactly once per terminal transaction. A scheduled
deadline event generates timeout without requiring status polling.
Registration must reject invalid descriptors or reused transaction IDs without
modifying any existing transaction. Contributor membership and destination
cannot change while the transaction is active. M5 does not reuse multicast
group bindings: its expected membership is the registered descriptor itself.

## Wire boundary

The 29-byte header layout is preserved. Implemented opcodes:
`REDUCE_SUM_U64 = 4` and `REDUCE_RESULT = 5`. System must adopt this wire contract
when connecting its adapter. In contribution headers:

- `transaction`: registered transaction ID.
- `src`: contributor GPU; must match the actual ingress GPU-port binding.
- `dst`: registered result GPU. The reduction opcode causes switch interception;
  it must not be forwarded as an ordinary request to that GPU.
- `address`, `size`: must exactly match the descriptor.
- Payload: exactly `size` bytes, uint64 elements in network byte order.

In the single result header, retain transaction ID, address and size;
`opcode=REDUCE_RESULT`, `src=8` (switch ID in this 8-GPU star), and
`dst=result_gpu`. Payload is the complete reduced vector. This explicitly
extends endpoint assumptions: src=8 is a switch, never a contributor GPU.
The switch-generated packet must have an explicit return envelope and traverse
the existing output queue/link; it must not complete System via an instantaneous
success callback. Sum payloads do not get returned individually.

## State, errors, and lifecycle

States: REGISTERED -> COLLECTING -> RESULT_QUEUED -> COMPLETE, or TIMED_OUT.
COMPLETE means destination receipt/validation in the endpoint adapter, not just
switch enqueue. The adapter associates completion with the registered ID and
reports it once to System. The switch itself cannot infer remote delivery
without this adapter signal or a future acknowledgement protocol.

The switch tracks a received-contributor set and one accumulator per active ID.
A duplicate contributor packet is ignored and counted, even if its data differs:
first valid contribution wins. It must not be added twice. Unknown IDs, incorrect
metadata/payload length, nonmember GPUs, and ingress/source mismatches are
rejected and counted without changing reduction state. No automatic retries,
implicit registration, cancellation, or new transactions are created on error.

Contributions at or after `deadline_ns` are late and rejected. Before accepting
a packet, compare the current time to the deadline, so same-timestamp event
ordering cannot make late arrivals valid. System's deadline covers result
receipt as well as collecting contributions: queued-but-undelivered results
must not be reported as successful at/after the deadline. A late result is
ignored by the adapter. Timed-out transactions generate no partial result;
System receives a timeout status through the adapter's failure path.

Completed/timed-out IDs retain a tombstone until simulation end to reject
late packets and prevent a second result. Accumulator storage can be released.
Bounded state capacity and queue capacity must be explicit implementation
parameters: registration returns a resource error when capacity is exhausted,
without evicting another transaction. Output drops cause missing completion;
M5 initially has no result retransmission protocol.

## Timing and measurement

Initial functional model: zero aggregation compute delay, while actual link
serialization, propagation and switch queueing remain modeled. This is a
stated baseline, not a hardware performance claim. Record first/last accepted
contribution, result enqueue, and destination receipt times. Measure all-finish
latency from the common workload start, and switch aggregation wait from first
to last accepted contribution. Count ingress bytes and result egress bytes
from actual packet events; do not count a hypothetical software baseline as
measured traffic.

## Acceptance criteria

1. GPUs 1–4 each contribute [gpu, 10*gpu] to one transaction, result GPU0
   receives exactly [10, 100] once, using the same ID/address/size.
2. Shuffled arrivals and interleaved outstanding IDs never mix accumulators.
3. Duplicate contributions cannot alter the sum or create extra results.
4. [UINT64_MAX] + [1] yields [0]; test multiple vector lengths, including 4096 bytes.
5. Missing member: timeout, no partial result, no completion success.
6. Nonmember, spoofed ingress, unknown ID and mismatched metadata are rejected;
   valid contributions still complete correctly afterward.
7. Deadline-boundary, late duplicate, resource rejection, and late result tests
   follow the lifecycle above.
8. Re-run M1–M4 with M5 to preserve forwarding and transaction regressions.

## System teammate sign-off

Confirm the descriptor/API boundary, opcode allocation, switch source identity,
error/status delivery, deadline behavior, and zero compute-delay baseline.
Network has implemented and tested these semantics using fake contributors;
System must confirm the integration contract before replacing the test adapter.


## Implementation notes

The C++ descriptor has `transaction`, `address`, `contributors`, `resultGpu`,
`size`, and absolute ns-3 `Time deadline`; SUM_U64 is the only supported
operation, selected by the opcode. `SetReductionCapacity(active, records)` must
be called before registering any record. Defaults are 16 active and 128 total
records including tombstones; allocation failure returns false. Descriptor
validation failures leave existing records unchanged. Registration pins input
and output interface bindings, preventing mid-transaction GPU rebinding from
silently changing participants.

Each record exposes first/last accepted contribution, result enqueue, and
adapter completion timestamps. Expiry and result enqueue release accumulator
storage; descriptor/received-set tombstones remain until simulation end.
Terminal events are local control-plane notifications, not wire acknowledgements.
Only the trusted adapter calls CompleteReduction after validating the delivered
payload. No separate System process or GPU resume integration is provided yet.

The result envelope uses the last valid contribution's UDP + SeqTs/INT envelope,
sets src to the switch node ID and IPv4 source to `10.0.0.(switch ID+1)`, and
retains the incoming result destination IPv4 address. This address convention
matches the standalone star test. Other network integrations must supply a
compatible addressing scheme or extend result-envelope configuration.
Existing switch/MMU/QBB queue capacities govern the result packet; there is no
separate aggregation output queue. `reductionResultBytes` counts actual switch
dequeue bytes, while test NIC counters measure all injected contribution bytes,
including malformed/nonmember inputs. The internal `reductionIngressBytes`
counts only contributions that passed the envelope parser, including those
subsequently rejected by descriptor/member checks.
