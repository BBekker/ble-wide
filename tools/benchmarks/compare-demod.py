#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay raw snapshots through selected IQ8 decoders on identical samples."""
import argparse
import json
import shutil
import hashlib
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
from packet_pairing import pairing
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--captures',type=Path,required=True)
p.add_argument('--output',type=Path,default=ROOT/'build/demod-comparison')
p.add_argument('--decode-threads', type=int, choices=range(1, 41), default=1)
p.add_argument('--backend',choices=['cpu','metal'],default='cpu')
p.add_argument('--baseline',choices=['legacy','adaptive'],default='legacy')
p.add_argument('--candidate',choices=['adaptive','preamble','streaming'],default='adaptive')
p.add_argument('--limit',type=int,default=0,help='Limit snapshots for development checks')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
# Freeze the executable so a concurrent rebuild cannot mix decoder versions.
binary = a.output / 'receiver-replay'
shutil.copy2(ROOT/'build/w1700k-ble-wide', binary)
binary_digest = hashlib.sha256(binary.read_bytes()).hexdigest()
rows=[]
captures = sorted(a.captures.glob('capture-*.raw'))
if a.limit: captures = captures[:a.limit]
for raw in captures:
    outputs={};statuses={}
    for mode in (a.baseline,a.candidate):
        result=subprocess.check_output([str(binary),'--file',str(raw),'--iq8','--backend',a.backend,'--demod',mode,'--decode-threads',str(a.decode_threads)],text=True)
        (a.output/(raw.stem+'.'+mode+'.jsonl')).write_text(result)
        events=[json.loads(line) for line in result.splitlines()]
        outputs[mode]=[e for e in events if e['type']=='packet']
        statuses[mode]=[e for e in events if e['type']=='status'][-1]
    both,missing,added=pairing(outputs[a.baseline],outputs[a.candidate])
    row={'capture':raw.stem,'baseline':len(outputs[a.baseline]),'candidate':len(outputs[a.candidate]),
         'matched':len(both),'lost':missing,'added':added,'status':statuses}
    rows.append(row)
    (a.output/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps({k:v for k,v in row.items() if k not in ('lost','added','status')}|{'lost':len(missing),'added':len(added)}),flush=True)
summary={k:sum(r[k] for r in rows) for k in ('baseline','candidate','matched')}
summary['binary_sha256']=binary_digest
summary['decode_threads']=a.decode_threads
summary['modes']={'baseline':a.baseline,'candidate':a.candidate}
summary['lost']=sum(len(r['lost']) for r in rows)
summary['added']=sum(len(r['added']) for r in rows)
summary['seconds']=sum(r['status'][a.baseline]['samples'] for r in rows)/160e6
summary['decode_cpu_s']={mode:sum(r['status'][mode]['decode_cpu_s'] for r in rows) for mode in (a.baseline,a.candidate)}
summary['added_by_context']={context:sum(p['context']==context for r in rows for p in r['added']) for context in ('advertising','connection_request','repeated_crc_seed')}
summary['added_below_minus50_dbfs']=sum(p['signal_dbfs'] < -50 for r in rows for p in r['added'])
summary['weakest_added_dbfs']=min((p['signal_dbfs'] for r in rows for p in r['added']),default=None)
(a.output/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
print(json.dumps(summary),flush=True)
