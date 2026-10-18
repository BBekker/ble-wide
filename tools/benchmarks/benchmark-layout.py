#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Paired offline comparison of GPU channel output layouts and CPU consumers."""
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
p.add_argument('--output', type=Path, default=ROOT/'build/layout-benchmark')
p.add_argument('--demod', choices=['adaptive', 'streaming'], default='streaming')
p.add_argument('--decode-threads', type=int, choices=range(1, 41), default=4)
p.add_argument('--repeat', type=int, default=40)
p.add_argument('--pairs', type=int, default=3)
a = p.parse_args()
if a.repeat < 1 or a.pairs < 1:
    p.error('repeat and pairs must be positive')
a.output.mkdir(parents=True, exist_ok=True)
binary = a.output/'receiver-replay'
shutil.copy2(a.binary, binary)
rows = []
for pair in range(a.pairs):
    order = ['interleaved', 'planar'] if pair % 2 == 0 else ['planar', 'interleaved']
    for layout in order:
        path = a.output/f'{len(rows)}-{layout}.jsonl'
        with path.open('w') as f:
            subprocess.run([str(binary.resolve()), '--file', str(a.capture.resolve()), '--iq8',
                            '--backend', 'metal', '--demod', a.demod, '--repeat', str(a.repeat),
                            '--decode-threads', str(a.decode_threads), '--bin-layout', layout],
                           stdout=f, check=True)
        events = [json.loads(line) for line in path.read_text().splitlines()]
        status = [e for e in events if e['type'] == 'status'][-1]
        rows.append(status)
        print(json.dumps({k: status[k] for k in ('bin_layout', 'packets', 'capacity_msps',
                                                'decode_cpu_s', 'decode_wall_s', 'gpu_s')}), flush=True)
summary = {
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'capture': str(a.capture.resolve()),
    'capture_sha256': hashlib.sha256(a.capture.read_bytes()).hexdigest(),
    'repeat': a.repeat, 'demod': a.demod, 'decode_threads': a.decode_threads, 'runs': rows,
    'medians': {
        layout: {field: statistics.median(r[field] for r in rows if r['bin_layout'] == layout)
                 for field in ('capacity_msps', 'processing_s', 'decode_cpu_s', 'decode_wall_s',
                               'filter_cpu_s', 'gpu_s', 'packets')}
        for layout in ('interleaved', 'planar')
    },
    'limitations': 'Offline processing excludes file I/O and IQ8 quantization; system load varies. '
                   'Both layouts use the batched waterfall. CPU times sum active thread time, '
                   'not elapsed latency. GPU time includes the planar transpose when enabled.'
}
(a.output/'results.json').write_text(json.dumps(summary, indent=2)+'\n')
