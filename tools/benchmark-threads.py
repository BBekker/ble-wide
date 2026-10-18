#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare channel decoding worker counts on identical offline IQ8 input."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--capture', type=Path, required=True)
p.add_argument('--binary', type=Path, default=ROOT/'build/w1700k-ble-wide')
p.add_argument('--output', type=Path, default=ROOT/'build/thread-benchmark')
p.add_argument('--repeat', type=int, default=40)
p.add_argument('--backend', choices=['cpu', 'metal'], default='metal')
p.add_argument('--demod', choices=['adaptive', 'streaming'], default='streaming')
p.add_argument('--threads', type=int, nargs='+', default=[1, 2, 4, 6, 8, 40])
a = p.parse_args()
if a.repeat < 1 or any(n < 1 or n > 40 for n in a.threads):
    p.error('repeat must be positive and threads must be 1..40')
a.output.mkdir(parents=True, exist_ok=True)
binary = a.output/'receiver-replay'
shutil.copy2(a.binary, binary)
rows = []
for index, count in enumerate(a.threads + list(reversed(a.threads))):
    path = a.output/f'{index}-{count}.jsonl'
    with path.open('w') as f:
        subprocess.run([str(binary.resolve()), '--file', str(a.capture.resolve()), '--iq8',
                        '--backend', a.backend, '--demod', a.demod, '--repeat', str(a.repeat),
                        '--decode-threads', str(count)], stdout=f, check=True)
    status = None
    for line in path.read_text().splitlines():
        event = json.loads(line)
        if event['type'] == 'status':
            status = event
    if status is None:
        raise RuntimeError(f'No final status in {path}')
    rows.append(status)
    print(json.dumps({key: status[key] for key in
                      ('decode_threads', 'packets', 'capacity_msps', 'decode_cpu_s', 'decode_wall_s')}),
          flush=True)
summary = {
    'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
    'capture': str(a.capture.resolve()),
    'capture_sha256': hashlib.sha256(a.capture.read_bytes()).hexdigest(),
    'repeat': a.repeat, 'backend': a.backend, 'demod': a.demod, 'runs': rows,
    'medians': {
        str(n): {field: statistics.median(r[field] for r in rows if r['decode_threads'] == n)
                 for field in ('capacity_msps', 'decode_cpu_s', 'decode_wall_s', 'processing_s', 'packets')}
        for n in a.threads
    },
    'limitations': 'Processing capacity excludes file I/O and IQ8 quantization. '
                   'CPU seconds are summed across decode threads; wall seconds measure batch latency. '
                   'This is not a live-loss measurement. Other system load is uncontrolled.'
}
(a.output/'results.json').write_text(json.dumps(summary, indent=2)+'\n')
