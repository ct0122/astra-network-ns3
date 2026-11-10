# Single-switch MemOp READ acceptance case

Run from `extern/network_backend/ns-3`:

```bash
cmake -S . -B cmake-cache -DNS3_MEMOP_FABRIC_ONLY=ON \
  -DNS3_ENABLED_MODULES='applications;internet;point-to-point'
cmake --build cmake-cache --target scratch_memop-read-smoke -j 4
./build/scratch/ns3.42-memop-read-smoke-default
```

This builds only the fabric test and its ns-3 dependencies, without requiring
Chakra or ASTRA's other dependencies. To return this build directory to the
normal ASTRA configuration, reconfigure with `-DNS3_MEMOP_FABRIC_ONLY=OFF`
and `-DNS3_ENABLED_MODULES=''` (the normal ASTRA dependencies must be available).

The program reads `scratch/topology/8_nodes_1_switch_topology.txt`: GPU0–7,
switch 8, eight 400 Gbps links, 500 ns propagation delay per link. A different
file can be selected with `--topology=PATH`; it must describe the same star.
GPU IDs map explicitly to the switch interfaces returned by QbbHelper; with
the installed loopback interface these are interfaces 1–8, rather than 0–7.

GPU3 sends READ transaction 729, address `0x1000`, size 64 bytes to GPU7.
GPU7's memory stub waits 100 ns and returns 64 bytes of `0xa5`, carrying the
same transaction ID, address and size, with source/destination reversed.
MemOp metadata is a serialized 29-byte packet header, not an out-of-band tag.
Both packets traverse the existing QBB channels and SwitchNode routing table.
Switch egress observations verify that each packet exits toward its destination
in priority group 1. Endpoint checks validate metadata and returned data.

Success requires exactly one READ, one response and two switch forwards, and
prints `PASS GPU3 READ GPU7 tx=729 requests=1 responses=1 switch_forwards=2`.
Missing completion at the 1 ms deadline returns exit status 1; invalid metadata,
wrong destination, duplicates or corrupted data abort the program.

Verified output with the checked-in topology:

```text
1000 ns SEND tx=729 GPU3->GPU7 opcode=1 address=4096 size=64
1501 ns SWITCH_FORWARD port=8 tx=729 GPU3->GPU7 opcode=1 address=4096 size=64
2002 ns RECEIVE tx=729 GPU3->GPU7 opcode=1 address=4096 size=64
2102 ns SEND tx=729 GPU7->GPU3 opcode=2 address=4096 size=64
2605 ns SWITCH_FORWARD port=4 tx=729 GPU7->GPU3 opcode=2 address=4096 size=64
3108 ns RECEIVE tx=729 GPU7->GPU3 opcode=2 address=4096 size=64
PASS GPU3 READ GPU7 tx=729 requests=1 responses=1 switch_forwards=2
READ round-trip=2108 ns (includes 100 ns memory stub delay)
```

Negative control (must print FAIL and return status 1):

```bash
./build/scratch/ns3.42-memop-read-smoke-default --dropResponse=1
```

This is a network acceptance test with a synthetic memory responder. It does
not yet integrate ASTRA system-layer MemOps, GPU execution, real GPU memory,
RDMA queue-pair reliability, or multicast/reduction/address-rewrite actions inside the switch.
The NIC control-packet injection queue is used to submit test packets; switch
forwarding uses the normal UDP priority group. Timing is a smoke-test result,
not a calibrated memory-system latency prediction. The Clos config is not used.

## M2: multiple transactions and contention

Use the same build instructions, then run:

```bash
./build/scratch/ns3.42-memop-read-smoke-default --m2=1
```

All eight GPUs submit eight READs at 1000 ns, without waiting for completion
(64 globally unique transaction IDs, 1000–1063). Each GPU reads sizes 32, 64,
128, 256, 512, 1024, 2048 and 4096 bytes. Size means requested/returned data
bytes; READ requests carry metadata only. GPUs 0–6 target GPU7 to create
switch egress contention, while GPU7 targets GPU0. All eight links retain
400 Gbps bandwidth; no artificial network delay or slowed link is introduced.

The transaction ledger records source, destination, address, size, issue time,
request receipt, completion, and request/response switch-forward counts. Every
receive and switch observation checks the serialized metadata against this
ledger. Returned bytes depend on transaction ID, address and byte offset.
Duplicate requests/responses, unknown IDs, wrong metadata and incorrect data
abort the test. Completed records remain available to detect duplicates.

Synthetic endpoint service delays vary from 100 to 1500 ns, deliberately causing
out-of-order responses. M2 requires observed out-of-order completion, eight
peak outstanding requests per GPU, zero outstanding requests at the deadline,
64 request receipts, 64 completions and exactly 128 switch forwards.

Switch `QbbEnqueue` and `QbbDequeue` traces measure residence time for each
(transaction ID, opcode) pair. M2 additionally requires positive switch queue
waiting. `queue_peak_bytes` is the maximum observed switch egress occupancy
including the packet about to be inserted (the enqueue trace fires before
insertion), excluding any packet already being transmitted. `waited_packets`
counts packets whose measured enqueue-to-dequeue time is positive. RTT includes
source NIC queueing, both network trips, and synthetic memory service delay.

Verified results with the supplied topology:

```text
GPU0–GPU7: peak_outstanding=8 remaining=0 (each)
queue_peak_bytes=4161 waited_packets=55 max_queue_wait_ns=48 reordered_completions=56
PASS M2 transactions=64 requests=64 responses=64 switch_forwards=128
```

Negative control:

```bash
./build/scratch/ns3.42-memop-read-smoke-default --m2=1 --dropResponse=1
```

Verified: returns status 1, 64 request receipts, zero completions, 64 switch
forwards, and eight outstanding requests per GPU. The original M1 invocation
without `--m2` also still passes.

This milestone exercises real switch contention and transaction matching using
fake MemOp producers/responders. It still uses single-packet responses (up to
4096 bytes), NIC control-queue injection, and no RDMA reliability or packet
fragmentation/reassembly. It does not establish saturated throughput, PFC
behavior, or production memory semantics.


## M3: programmable binding and metadata forwarding

Run the complete regression suite after every switch change:

```bash
cd ~/astra-sim-memops/extern/network_backend/ns-3
python3 scratch/run-memop-regressions.py
```

The runner configures/builds the standalone fabric target, checks M1, M2, M3,
and the M2 missing-response negative control, and stops on unexpected status or
missing result markers. Full packet traces are saved in
`build/memop-regressions/{M1,M2,M3,M2-timeout}.log`. Run M3 alone with:

```bash
./build/scratch/ns3.42-memop-read-smoke-default --m3=1
```

`MemOpHeader` now lives in `src/point-to-point/model/memop-header.h` and is shared
by the switch and test producer. Its wire layout remains 29 bytes, preserving
M1/M2. `SwitchNode` exposes:

```cpp
sw->EnableMemOpForwarding(true);  // disabled by default
sw->BindGpuPort(gpuId, interfaceIndex);
sw->BindMemOpTransaction(transactionId, MemOpHeader::READ, interfaceIndex);
sw->GetGpuPort(gpuId);
sw->ClearMemOpRules();
```

Bind calls return false for invalid/non-QBB/down ports and do not alter existing
bindings on failure. Rebinding replaces a previous value. Transaction binding
also rejects unsupported opcodes. Bindings use actual switch interface indices,
including the loopback offset; never assume GPU ID equals interface index.

When enabled, UDP destination port 9000 identifies this MemOp protocol. The
switch parses the serialized metadata and selects output in this order:

1. Exact `(transaction ID, opcode)` port rule.
2. `MemOp.dst` GPU-to-port mapping.
3. Drop if no valid output is bound; no IP fallback for a recognized MemOp.

Unsupported opcodes, zero sizes, truncated metadata, fragmented packets and
invalid request/response payload lengths are dropped. The switch checks the
selected output is still a live QBB port. This parser expects the backend's
UDP + SeqTs/INT envelope; it does not handle generic UDP MemOps. Traffic outside
this reserved port, or with the feature disabled, retains existing IP/ECMP
routing. No payload or destination address is rewritten by binding.

M3 submits eight READs (IDs 3000–3007), one per GPU, with sizes 128–1024 bytes.
For every request and response, the outer IP destination is intentionally one
GPU away from the MemOp destination, so ordinary IP routing would fail the
physical endpoint checks. Normal MemOps follow the metadata mapping.
Transaction 3000 is an explicit override: GPU0's logical READ destination is
GPU3, but its READ rule binds to physical GPU5. GPU5 is the selected memory
stub and replies as GPU5 to GPU0. The test ledger explicitly expects this
physical service endpoint; logical request metadata remains unchanged. The
same transaction's RESPONSE does not match the READ rule and uses GPU0's
mapping, verifying opcode-specific matching.

Before injection, M3 checks rejected loopback/out-of-range binds, GPU binding
replacement/restoration, and all eight mappings. It first installs transaction
3000's rule toward GPU6, then replaces it with GPU5 before sending; actual
GPU5 delivery proves replacement. The acceptance condition includes all eight
completions, zero outstanding requests, 16 switch forwards, exactly one rule
hit and 15 GPU-map hits:

```text
PASS M3 transactions=8 requests=8 responses=8 switch_forwards=16 rule_hits=1 mapping_hits=15
```

M1 and M2 run with MemOp forwarding disabled to preserve the IP-forwarding
baseline. M3 exercises the new engine independently of Compute/System, using
the same synthetic producer/responder and real QBB forwarding as M1/M2. This
milestone adds static programmable bindings; it does not implement multicast,
reduction, address rewriting, or a general programmable pipeline.

## M4: switch-side multicast versus source unicast replication

The full runner now also tests M4 at fanouts 1, 4 and 7 with response sizes
128 and 4096 bytes, compares measured traffic, and checks a missing-member
negative control. Run all milestones after modifying switch code:

```bash
python3 scratch/run-memop-regressions.py
```

Individual comparison (run both from the ns-3 directory):

```bash
./build/scratch/ns3.42-memop-read-smoke-default --m4=unicast --fanout=4 --readSize=128
./build/scratch/ns3.42-memop-read-smoke-default --m4=multicast --fanout=4 --readSize=128
```

GPU0 targets GPU1 through GPU N. Both experiments start at 1000 ns with the
same topology, 400 Gbps links, 500 ns link delay, 100 ns memory service time,
and per-recipient READ size. Unicast submits N READs at the same timestamp
and serializes them through GPU0's NIC. Multicast submits exactly one
`MULTICAST_READ` (opcode 3) to group 100; the switch creates N packet copies.
The 29-byte MemOp wire header is unchanged in size. The group destination and
transaction ID remain unchanged in every clone; no per-destination request
rewrite or source-side replication occurs.

Configure groups after GPU-port binding:

```cpp
sw->EnableMemOpForwarding(true);
sw->BindMemOpMulticastGroup(100, {1, 2, 3, 4});
// Send MULTICAST_READ with dst=100, src=0, transaction=4000.
```

The API returns false for empty groups, duplicate output ports, and missing or
invalid GPU bindings, leaving the previous group unchanged. Rebinding a group
replaces its members. On receive, the switch resolves the full group through
current GPU-to-port bindings, verifies all outputs, and rejects source
reflection or invalid members before creating any copy. Unknown groups drop.
Group forwarding applies only to MULTICAST_READ; normal READ/RESPONSE keep
M3's forwarding behavior. Every clone goes through the existing switch's
per-port admission control and queue. Congestion can still drop individual
copies; replication is not an atomic multicast delivery guarantee.

Every destination verifies the metadata, returns its own deterministic data,
and echoes transaction 4000. GPU0 tracks completion by `(transaction ID,
responder GPU)`, rejects duplicate responders, validates each data payload,
and completes the collective READ only when all members respond. The test
also checks exactly one request forward and one response forward per member,
no delivery outside the group, and actual source packet counts. Invalid group
updates are tested before sending, ensuring a rejected update preserves the
valid group.

Each `M4_METRICS` JSON record measures:

- `source_packets` / `source_bytes`: GPU0 NIC dequeue events.
- `nic_bytes`: all GPU NIC transmissions, including responses.
- `switch_bytes`: all switch egress transmissions, including responses.
- `total_link_bytes`: NIC plus switch bytes, counting each traversed link once.
- `last_delivery_ns`: time until the final member receives its request.
- `first_completion_ns` / `all_completion_ns`: time until the first/all responses
  arrive at GPU0. All latency is measured from the common 1000 ns injection time.

Byte counts include the modeled PPP, IP, UDP, SeqTs/INT, MemOp headers and
payload, excluding unmodeled physical framing. Counts come from QBB dequeue
traces, not from multiplying the requested data size. The runner checks source
bytes reduce by the fanout factor, switch egress bytes stay equal, and the total
link-byte reduction equals the source-byte reduction. It does not assume latency
must improve. Full logs and a 12-row comparison CSV are written to
`build/memop-regressions/M4-*.log` and `M4-comparison.csv`.

Verified default comparison (fanout 4, 128 response bytes per GPU):

| Measurement | Unicast replication | Switch multicast |
|---|---:|---:|
| GPU0 request packets | 4 | 1 |
| GPU0 transmitted bytes | 260 | 65 |
| Switch transmitted bytes | 1032 | 1032 |
| Total link bytes, requests + responses | 2064 | 1869 |
| Final request delivery (ns) | 1005 | 1002 |
| First completion (ns) | 2110 | 2110 |
| All completions (ns) | 2122 | 2122 |

Source request bytes decrease 75%; total link bytes decrease about 9.45%.
At fanout 7 / size 128, source bytes decrease 455 to 65, total link bytes
3612 to 3222, final delivery 1008 to 1002 ns, and all-completion latency remains
2134 ns. At fanout 4 / size 4096, total link bytes decrease 33808 to 33613 and
all-completion latency remains 2517 ns.

These are metadata-only READ requests: the size parameter determines returned
data, not outbound request payload. Switch multicast saves repeated request
traffic on GPU0's uplink. It still sends N requests on switch outputs and gets
N individual responses. Response traffic converges on GPU0's switch output,
which hides the small request-delivery improvement in these measurements.
There is no switch reduction/response aggregation, modeled replication compute
cost, packet fragmentation, RDMA retransmission, or real GPU memory behavior.
The results establish functional multicast and its modeled traffic tradeoff,
not a general latency speedup or multicast WRITE implementation.

Partial-completion negative control (expected exit status 1):

```bash
./build/scratch/ns3.42-memop-read-smoke-default --m4=multicast --dropResponder=2
```

Verified: four request deliveries, three completions, and FAIL at the 1 ms
deadline. It cannot report success merely because one or several members reply.

## M5: uint64 SUM reduction / aggregation

Implemented using the [M5 operation contract](M5-operation-contract.md), with fake
contributors and a fake System completion adapter. Real System integration is
still pending; no Compute dependency or real GPU-memory side effects are needed.

```bash
python3 scratch/run-memop-regressions.py
# Or run one M5 scenario:
./build/scratch/ns3.42-memop-read-smoke-default --m5=normal
```

The runner now covers M1–M5. `--m5` scenarios are `normal`, `missing`, `boundary`,
`late-result`, `result-boundary`, and `capacity`. A PASS for a timeout scenario
means the expected timeout was observed with no success event; it does not mean
the reduction completed. Logs are in `build/memop-regressions/M5-*.log`.

Switch API usage (after GPU-port binding and EnableMemOpForwarding):

```cpp
SwitchNode::ReductionDescriptor d{
    5000, 0x500000, {1, 2, 3, 4}, 0, 16, MicroSeconds(20)
}; // id, address, contributors, result GPU, payload bytes, absolute deadline
sw->RegisterReduction(d);
```

GPUs 1–4 inject REDUCE_SUM_U64 contributions `[gpu, 10*gpu]`. Switch8 consumes
them instead of forwarding them to GPU0, checks actual ingress GPU identity,
and maintains a distinct accumulator and received-member set per transaction.
It produces exactly one REDUCE_RESULT `[10,100]` for GPU0 after receiving all
members. The original ID/address/size are retained, and src is Switch8. The
result traverses the regular switch queue and output link. GPU0 checks the
full vector before the adapter calls `CompleteReduction(id, gpu)`.

Payloads are big-endian uint64 arrays, 8–4096 bytes, with addition modulo 2^64.
The first valid contribution from each member wins; duplicate inputs are counted
and ignored. Invalid metadata, ingress spoofing, nonmembers, unknown IDs and
malformed payloads are rejected without changing the accumulator. Reused IDs
are rejected at registration, and completed/timed-out IDs retain tombstones.

The absolute deadline covers result delivery, not just input collection.
The switch schedules timeout events and checks the deadline on contribution
and completion acceptance. Exactly-at-deadline and late packets cannot complete
successfully. Missing/late results are reported via `reductionEvents` as
TIMED_OUT; validated in-time receipt reports COMPLETE once. A partial sum is
never sent. There is no retry or retransmission.

M5 regression coverage:

| Scenario | Expected behavior |
|---|---|
| normal | Four interleaved reductions: `[10,100]`, overflow `[0]`, 512-element vector of 3s, and another `[10,100]`; two duplicates ignored; five invalid inputs rejected |
| missing | Missing member at deadline; a subsequent late contribution is rejected; zero results |
| boundary | Final contribution reaches switch exactly at deadline; zero results |
| late-result | Switch emits result before deadline, but destination receives it late; timeout, no success |
| result-boundary | Result reaches destination exactly at deadline; timeout, no success |
| capacity | Active limit rejects concurrent registration, capacity recovers after completion, retained-record limit rejects further IDs |

The suite additionally verifies one terminal event per ID, switch result-byte
counters against actual egress traces, descriptor rejection, and data contents.
Normal-case results include four completions/four result packets, two duplicate
inputs, five rejected inputs, 9675 NIC contribution bytes and 4396 result-link
bytes. These totals include deliberately invalid/duplicate test traffic; they
are not a performance comparison against a software reduction baseline.

Aggregation compute delay is zero for this functional milestone. Network
serialization, propagation and queues remain modeled. Default state limits are
16 active reductions and 128 retained IDs; configure before registration using
`SetReductionCapacity(active, records)`. The switch stores per-record first/last
input, result enqueue, and completion times. System sign-off is still needed for
real adapter integration, result source identity and address configuration.

## M6: network-side READ interface and evaluation

Scope: a reusable System-facing **READ** adapter and a network-only evaluation
harness. The real ASTRA frontend still exposes message `sim_send/sim_recv`; no
System MemOp interface was found in this checkout. The harness now submits
through the new backend adapter instead of using the M1/M2 `Issue()` ledger,
and receives exactly-once completion callbacks from real QBB response delivery.
The workload generator and 100 ns GPU memory responder remain synthetic.
This is not a claim that the real System/Compute pipeline is integrated.
M4/M5 retain their existing multicast/reduction APIs and tests; the M6 adapter
and its sweep currently cover READ only, not collective completion APIs.

### Interface available to System

Header: `src/point-to-point/model/memop-network-adapter.h`, exported as
`ns3/memop-network-adapter.h`. Construct and retain a shared_ptr for the lifetime
of outstanding operations, on the ns-3 simulation thread:

```cpp
using Adapter = ns3::MemOpNetworkAdapter;
auto adapter = std::make_shared<Adapter>(
    sendReadThroughBackend, // void(const MemOpHeader&), bound to real transport
    64,                    // outstanding capacity
    65536                  // retained IDs, including completed tombstones
);
Adapter::Request request{
    123, 0x1000, 3, 7, 128,
    ns3::Simulator::Now() + ns3::MicroSeconds(100)
}; // id, address, source GPU, destination GPU, response size, absolute deadline
const auto admission = adapter->Submit(request, completion);
// Endpoint adapter, after parsing the real response:
adapter->Receive(responseHeader, responseBytes);
```

`Submit` returns ACCEPTED, BUSY, INVALID, DUPLICATE, or RECORD_LIMIT. ACCEPTED
means ownership of completion tracking, not NIC transmission. Injection is
scheduled at the current simulation time; completion cannot occur inside
Submit. Rejected submissions never trigger a callback and can be retried only
where appropriate (BUSY). Invalid sizes/endpoints/deadlines are rejected.
This initial API accepts GPUs 0–7 and one-packet response sizes 1–4096 bytes.

Each accepted READ gets exactly one Completion containing the original Request,
SUCCESS/TIMED_OUT status, acceptance/finish timestamps, and response bytes on
success. Deadline equality counts as timeout; no partial payload is returned on
timeout. The adapter validates transaction, reversed source/destination, opcode,
address, size and payload length before success. System remains responsible for
interpreting data or committing memory effects. Invalid/unknown/duplicate/late
responses cannot cause success or a second callback. Terminal state and capacity
are updated before invoking a potentially reentrant callback. Tombstones retain
IDs until adapter destruction, with a fixed record limit. A timed-out packet is
not cancelled in the network; a late response can still consume link bandwidth.
There is no cancellation API or retry protocol.

The injected transport is already bound to the smoke driver's real QBB send
path for M6. To integrate with System, its teammate must wire the frontend's
MemOp submission to this adapter, bind the real transport and response endpoint,
and decide how completion status/data resumes System/GPU work. The address is
still opaque to Network. Do not map network acceptance or switch enqueue to
System completion. Destruction of the owning adapter before completion abandons
its pending operations; retain it through all callbacks/Simulator::Destroy.

### Commands and artifacts

```bash
# Build and run all M1–M6 regressions:
python3 scratch/run-memop-regressions.py

# Full 18-scenario evaluation (runs regressions first):
python3 scratch/run-memop-evaluation.py

# One configurable READ workload:
./build/scratch/ns3.42-memop-read-smoke-default --m6=1 \
  --m6Count=512 --m6Size=4096 --m6Window=64 --m6Period=0 \
  --m6Timeout=100000 --m6Output=build/memop-evaluation/custom
```

The sweep varies response size 128/4096 bytes, global outstanding limit
8/64/256, and offered submission interval 0/20/200 ns, with 512 accepted READs
per scenario. GPU sources rotate 0–7: GPUs 0–6 READ GPU7, GPU7 READs GPU0.
Period 0 means a burst bounded by adapter capacity. On BUSY, the generator waits
for a completion before retrying. Latency starts at accepted submission and
includes source NIC queueing, both network trips and synthetic memory service;
it excludes time waiting outside the adapter for admission. Timeout is measured
from each accepted submission. The test stops with a conservative drain period
so late responses are included in traffic observations.

Artifacts under `build/memop-evaluation/`:

| File | Meaning |
|---|---|
| `report.md`, `comparison.csv` | Cross-scenario report and machine-readable metrics |
| `<scenario>/summary.json` | Success/timeout counts, BUSY events, peak outstanding, throughput, latency percentiles and traffic |
| `latency.csv` | One terminal callback per transaction, status and timestamps |
| `queue.csv`, `queue-summary.csv` | Event-driven switch egress occupancy and per-port peak/time-weighted mean |
| `nic-queue.csv`, `nic-queue-summary.csv` | NIC injection/control-queue occupancy and per-GPU peak/time-weighted mean |
| `traffic.csv` | Actual NIC/switch dequeue event, transaction, opcode and packet bytes |

Per-scenario filenames in the last four rows live inside each scenario directory.
All times are ns-3 simulated nanoseconds. Queue counts exclude the packet already
being transmitted; enqueue observations include the packet about to be inserted.
A transient peak can include a packet dequeued at the same timestamp even when
its waiting duration is zero. Means use event-based byte-nanosecond integration
from the common first submission (1000 ns) to the last callback or observed
network activity, whichever is later. Zero-queue intervals are included.

Payload goodput is successful returned-data bits divided by the batch window
from first accepted request to final callback; completed_ops_per_second uses the
same window. It is aggregate across GPUs, so it may exceed one 400 Gbps link's
capacity. It is not per-link wire throughput. Byte counters count modeled packet
bytes once per link traversal, including headers; `total_link_bytes` is NIC plus
switch bytes. They include late traffic even after timeout; malformed response
API probes used by the test never travel on the network and are not counted.

The evaluation script independently reconstructs callback latency percentiles,
NIC/switch byte counts and request/response packet counts, and switch/NIC queue
peaks and time-weighted means from CSV events. It requires all 512 operations
to succeed in each normal sweep case. M6 regression additionally verifies
admission guards, duplicate IDs, invalid response matching, retained-record
capacity, reentrant duplicate completion rejection, no-response timeouts, and
responses arriving after timeout.

Representative verified finite-batch results at period 0:

| Response bytes | Outstanding limit | Aggregate payload Gbps | p95 latency ns |
|---|---:|---:|---:|
| 128 | 8 | 3.882 | 2110 |
| 128 | 64 | 30.660 | 2230 |
| 128 | 256 | 102.560 | 2902 |
| 4096 | 8 | 115.189 | 2268 |
| 4096 | 64 | 425.310 | 5150 |
| 4096 | 256 | 423.785 | 20486 |

Larger outstanding windows can raise batch goodput but also increase waiting.
The NIC summaries distinguish source/responder injection bottlenecks from
switch egress waiting. These measurements are functional simulation results,
not calibrated GPU memory latency, steady-state saturation measurements, a
Compute/System integration result, or evaluation of multicast/reduction through
the new adapter. M1–M5 continue to exercise those earlier network behaviors.
