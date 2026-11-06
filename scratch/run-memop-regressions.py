#!/usr/bin/env python3
"""Build and run the M1/M2/M3 network regression suite. No ASTRA dependencies."""
from pathlib import Path
import subprocess
import sys

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
print('PASS all MemOp regressions')
