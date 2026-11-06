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
