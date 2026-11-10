#!/usr/bin/env python3
"""Build and run the M1–M6 network regression suite. No ASTRA dependencies."""
from pathlib import Path
import subprocess
import sys
import json
import csv

root = Path(__file__).resolve().parent.parent

def run(args, **kwargs):
    return subprocess.run(args, cwd=root, check=True, **kwargs)

run(['cmake', '-S', '.', '-B', 'cmake-cache', '-DNS3_MEMOP_FABRIC_ONLY=ON',
     '-DNS3_ENABLED_MODULES=applications;internet;point-to-point'])
run(['cmake', '--build', 'cmake-cache', '--target', 'scratch_memop-read-smoke', '-j', '4'])
binaries = list((root / 'build/scratch').glob('ns3.*-memop-read-smoke-default'))
if len(binaries) != 1:
    sys.exit('Expected exactly one default-profile memop-read-smoke binary')
binary = str(binaries[0])
logs = root / 'build/memop-regressions'
logs.mkdir(exist_ok=True)
for name, args, status, marker in [
    ('M1', [], 0, 'PASS GPU3 READ GPU7'),
    ('M2', ['--m2=1'], 0, 'PASS M2 transactions=64'),
    ('M3', ['--m3=1'], 0, 'PASS M3 transactions=8'),
    ('M2-timeout', ['--m2=1', '--dropResponse=1'], 1, 'FAIL M2'),
]:
    result = subprocess.run([binary, *args], cwd=root, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (logs / f'{name}.log').write_text(result.stdout)
    if result.returncode != status or marker not in result.stdout:
        print(result.stdout)
        sys.exit(f'{name} regression failed (exit {result.returncode}, expected {status})')
    print(f'{name}: PASS ({logs / (name + ".log")})')
# Sweep fanout and data size with identical topology and endpoint service times.
rows = []
for fanout in (1, 4, 7):
    for size in (128, 4096):
        pair = {}
        for mode in ('unicast', 'multicast'):
            name = f'M4-{mode}-n{fanout}-s{size}'
            result = subprocess.run([binary, f'--m4={mode}', f'--fanout={fanout}',
                                     f'--readSize={size}'], cwd=root, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (logs / f'{name}.log').write_text(result.stdout)
            if result.returncode != 0 or f'PASS M4 {mode}' not in result.stdout:
                sys.exit(f'{name} failed: {result.stdout}')
            metrics = [json.loads(line.removeprefix('M4_METRICS '))
                       for line in result.stdout.splitlines() if line.startswith('M4_METRICS ')]
            if len(metrics) != 1:
                sys.exit(f'{name}: expected one metrics record')
            pair[mode] = metrics[0]
            rows.append(metrics[0])
        uni, multi = pair['unicast'], pair['multicast']
        if not (uni['source_packets'] == fanout and multi['source_packets'] == 1
                and uni['source_bytes'] == fanout * multi['source_bytes']
                and uni['switch_bytes'] == multi['switch_bytes']
                and uni['total_link_bytes'] - multi['total_link_bytes'] ==
                    uni['source_bytes'] - multi['source_bytes']):
            sys.exit(f'M4 traffic comparison failed: {pair}')
        print(f'M4 fanout={fanout} size={size}: PASS; source bytes '
              f"{uni['source_bytes']} -> {multi['source_bytes']}; all-completion ns "
              f"{uni['all_completion_ns']} -> {multi['all_completion_ns']}")
with (logs / 'M4-comparison.csv').open('w', newline='') as output:
    writer = csv.DictWriter(output, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
result = subprocess.run([binary, '--m4=multicast', '--dropResponder=2'], cwd=root,
                        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
(logs / 'M4-missing-member.log').write_text(result.stdout)
if result.returncode != 1 or 'FAIL M4 multicast recipients=4 deliveries=4 completions=3' not in result.stdout:
    sys.exit(f'M4 partial-completion negative control failed: {result.stdout}')
print('M4 missing-member negative control: PASS')
for scenario in ('normal', 'missing', 'boundary', 'late-result', 'result-boundary', 'capacity'):
    result = subprocess.run([binary, '--m5=' + scenario], cwd=root, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (logs / f'M5-{scenario}.log').write_text(result.stdout)
    if result.returncode != 0 or f'PASS M5 {scenario}' not in result.stdout:
        sys.exit(f'M5 {scenario} regression failed: {result.stdout}')
    print(result.stdout.splitlines()[-1])
for name, args, successes, timeouts in [
    ('read', [], 32, 0),
    ('timeout', ['--dropResponse=1', '--m6Timeout=5000'], 0, 32),
    ('late', ['--m6Timeout=500'], 0, 32),
]:
    output = logs / ('M6-' + name)
    result = subprocess.run([binary, '--m6=1', '--m6Count=32', '--m6Window=8',
                             '--m6Output=' + str(output), *args], cwd=root, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (logs / f'M6-{name}.log').write_text(result.stdout)
    if result.returncode != 0 or 'PASS M6' not in result.stdout:
        sys.exit(f'M6 {name} failed: {result.stdout}')
    stats = json.loads((output / 'summary.json').read_text())
    if stats['success'] != successes or stats['timeouts'] != timeouts:
        sys.exit(f'M6 {name} wrong callback outcomes: {stats}')
    print(result.stdout.splitlines()[-1])
print('PASS all MemOp regressions')
