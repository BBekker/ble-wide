#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Paired offline throughput measurement; never changes the live receiver."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--capture', type=Path, required=True)
p.add_argument('--binary', type=Path, default=ROOT/'build/w1700k-ble-wide')
p.add_argument('--output', type=Path, default=ROOT/'build/single-pass-benchmark')
p.add_argument('--repeat', type=int, default=40)
p.add_argument('--decode-threads', type=int, choices=range(1, 41), default=1)
p.add_argument('--backend', choices=['cpu', 'metal'], default='metal')
a = p.parse_args()
if a.repeat < 1:
    p.error('--repeat must be positive')
a.output.mkdir(parents=True, exist_ok=True)
binary = a.output/'receiver-replay'
shutil.copy2(a.binary, binary)
rows = []
for k, mode in enumerate(['adaptive', 'streaming', 'streaming', 'adaptive']):
    path = a.output/f'{k}-{mode}.jsonl'
    with path.open('w') as f:
        subprocess.run([str(binary.resolve()), '--file', str(a.capture.resolve()), '--iq8',
                        '--backend', a.backend, '--demod', mode, '--repeat', str(a.repeat), '--decode-threads', str(a.decode_threads)],
                       stdout=f, check=True)
    statuses = [e for line in path.read_text().splitlines()
                if (e := json.loads(line)).get('type') == 'status']
    row = dict(mode=mode, **statuses[-1])
    rows.append(row)
    print(json.dumps({k: row[k] for k in ('mode', 'packets', 'capacity_msps', 'decode_cpu_s')}), flush=True)
summary = {
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'capture': str(a.capture.resolve()), 'repeat': a.repeat,
    'backend': a.backend, 'decode_threads': a.decode_threads, 'runs': rows,
    'medians': {
        mode: {field: statistics.median(r[field] for r in rows if r['mode'] == mode)
               for field in ('capacity_msps', 'decode_cpu_s', 'processing_s', 'packets')}
        for mode in ('adaptive', 'streaming')
    },
    'limitations': 'processing_s is elapsed; decode_cpu_s sums decoder thread CPU time. Excludes file I/O and IQ quantization; other system load is uncontrolled.'
}
(a.output/'results.json').write_text(json.dumps(summary, indent=2)+'\n')
