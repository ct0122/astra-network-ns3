#!/usr/bin/env python3
"""Network-only READ adapter evaluation; verify/build M1–M6 before measurement."""
from pathlib import Path
import csv
import json
import math
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
subprocess.run([sys.executable, str(root / 'scratch/run-memop-regressions.py')], cwd=root, check=True)
binaries = list((root / 'build/scratch').glob('ns3.*-memop-read-smoke-default'))
if len(binaries) != 1:
    sys.exit('Expected one default-profile smoke executable')
output = root / 'build/memop-evaluation'
output.mkdir(exist_ok=True)
rows = []
for size in (128, 4096):
    for window in (8, 64, 256):
        for period in (0, 20, 200):
            scenario = f's{size}-w{window}-p{period}'
            directory = output / scenario
            result = subprocess.run([str(binaries[0]), '--m6=1', '--m6Count=512',
                f'--m6Size={size}', f'--m6Window={window}', f'--m6Period={period}',
                f'--m6Output={directory}'], cwd=root, text=True, stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT)
            (output / f'{scenario}.log').write_text(result.stdout)
            if result.returncode != 0 or 'PASS M6' not in result.stdout:
                sys.exit(f'{scenario} failed: {result.stdout}')
            stats = json.loads((directory / 'summary.json').read_text())
            if stats['accepted'] != 512 or stats['success'] != 512 or stats['timeouts'] != 0:
                sys.exit(f'{scenario}: incomplete workload: {stats}')
            with (directory / 'latency.csv').open() as f:
                latencies = list(csv.DictReader(f))
            if len(latencies) != 512 or len({r['transaction'] for r in latencies}) != 512:
                sys.exit(f'{scenario}: missing/duplicate callback rows')
            durations = sorted(int(r['finished_ns']) - int(r['accepted_ns']) for r in latencies)
            if any(r['status'] != 'success' or int(r['size']) != size for r in latencies):
                sys.exit(f'{scenario}: invalid callback metadata')
            for percentile in (50, 95, 99):
                if stats[f'latency_p{percentile}_ns'] != durations[(percentile*512+99)//100-1]:
                    sys.exit(f'{scenario}: wrong latency percentile')
            with (directory / 'traffic.csv').open() as f:
                traffic = list(csv.DictReader(f))
            for kind, key in [('nic', 'nic_bytes'), ('switch', 'switch_bytes')]:
                packets = [r for r in traffic if r['kind'] == kind]
                if sum(int(r['bytes']) for r in packets) != stats[key] or len(packets) != 1024:
                    sys.exit(f'{scenario}: incorrect traffic counters')
                if any(sum(r['opcode'] == opcode for r in packets) != 512 for opcode in ('1', '2')):
                    sys.exit(f'{scenario}: request/response traffic mismatch')
            # Independently integrate event-driven occupancy, including zero-occupancy intervals.
            states = {gpu: [1000, 0, 0, 0] for gpu in range(8)}
            with (directory / 'queue.csv').open() as f:
                for event in csv.DictReader(f):
                    gpu, now, value = int(event['gpu']), int(event['time_ns']), int(event['waiting_bytes'])
                    state = states[gpu]
                    if now < state[0]:
                        sys.exit(f'{scenario}: non-monotonic queue events')
                    state[2] += (now - state[0]) * state[1]
                    state[0], state[1], state[3] = now, value, max(state[3], value)
            with (directory / 'queue-summary.csv').open() as f:
                queues = list(csv.DictReader(f))
            if len(queues) != 8:
                sys.exit(f'{scenario}: missing port summaries')
            for q in queues:
                state = states[int(q['gpu'])]
                area = state[2] + (stats['observation_end_ns'] - state[0]) * state[1]
                average = area / (stats['observation_end_ns'] - 1000)
                if state[1] != 0 or int(q['peak_waiting_bytes']) != state[3] or not math.isclose(
                        float(q['time_mean_waiting_bytes']), average, rel_tol=1e-5, abs_tol=1e-4):
                    sys.exit(f'{scenario}: occupancy integration mismatch')
            nic_states = {gpu: [1000, 0, 0, 0] for gpu in range(8)}
            with (directory / 'nic-queue.csv').open() as f:
                for event in csv.DictReader(f):
                    state = nic_states[int(event['gpu'])]
                    now, value = int(event['time_ns']), int(event['waiting_bytes'])
                    if now < state[0]:
                        sys.exit(f'{scenario}: non-monotonic NIC queue events')
                    state[2] += (now - state[0]) * state[1]
                    state[0], state[1], state[3] = now, value, max(state[3], value)
            with (directory / 'nic-queue-summary.csv').open() as f:
                nic_queues = list(csv.DictReader(f))
            if len(nic_queues) != 8:
                sys.exit(f'{scenario}: missing NIC summaries')
            for q in nic_queues:
                state = nic_states[int(q['gpu'])]
                area = state[2] + (stats['observation_end_ns'] - state[0]) * state[1]
                average = area / (stats['observation_end_ns'] - 1000)
                if state[1] != 0 or int(q['peak_waiting_bytes']) != state[3] or not math.isclose(
                        float(q['time_mean_waiting_bytes']), average, rel_tol=1e-5, abs_tol=1e-4):
                    sys.exit(f'{scenario}: NIC occupancy integration mismatch')
            row = dict(scenario=scenario, size=size, window=window, period_ns=period, **stats)
            row['max_port_peak_waiting_bytes'] = max(int(q['peak_waiting_bytes']) for q in queues)
            row['max_nic_peak_waiting_bytes'] = max(int(q['peak_waiting_bytes']) for q in nic_queues)
            rows.append(row)
            print(f"{scenario}: {stats['payload_goodput_gbps']:.3f} Gbps, "
                  f"p95={stats['latency_p95_ns']} ns, peak={row['max_port_peak_waiting_bytes']} B")
with (output / 'comparison.csv').open('w', newline='') as f:
    writer = csv.DictWriter(f, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
with (output / 'report.md').open('w') as f:
    f.write('# M6 network-only READ evaluation\n\n')
    f.write('512 finite-batch READs per scenario; 400 Gbps star links; synthetic 100 ns memory service. '
            'Period 0 means a burst bounded by adapter capacity. These are batch goodput results, '
            'not a steady-state saturation or real System integration claim.\n\n')
    f.write('| Size B | Outstanding limit | Period ns | Payload Gbps | p95 ns | Peak switch queue B | Peak NIC queue B |\n'
            '|---:|---:|---:|---:|---:|---:|---:|\n')
    for r in rows:
        f.write(f"| {r['size']} | {r['window']} | {r['period_ns']} | "
                f"{r['payload_goodput_gbps']:.3f} | {r['latency_p95_ns']} | "
                f"{r['max_port_peak_waiting_bytes']} | {r['max_nic_peak_waiting_bytes']} |\n")
print(f'PASS M6 evaluation: {output / "comparison.csv"}')
