#!/usr/bin/env python3
"""Use the already authenticated temporary USB test session; never prompts."""
# SPDX-License-Identifier: MIT
import argparse,json,socket,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--stop',action='store_true')
p.add_argument('--timeout',type=float,default=30)
p.add_argument('--log',type=Path)
p.add_argument('args',nargs=argparse.REMAINDER)
a=p.parse_args();info=json.loads((ROOT/'receiver/build/usb-research/session.json').read_text());target=Path(info['socket'])
if not target.exists():p.error('authenticated test session is not ready')
if not a.stop:
 shutil.copytree(ROOT/'receiver/build/BLEUSBHelper.app',target.parent/'BLEUSBHelper.app',dirs_exist_ok=True)
s=socket.socket(socket.AF_UNIX);s.settimeout(min(120,a.timeout)+10);s.connect(str(target))
args=a.args[1:] if a.args[:1]==['--'] else a.args
s.sendall(json.dumps({'action':'stop' if a.stop else 'run','args':args,'timeout':a.timeout}).encode()+b'\n')
with s.makefile('r') as f:response=json.loads(f.readline())
if a.log:a.log.write_text(json.dumps(response,indent=2)+'\n')
print(response.get('output',response))
raise SystemExit(1 if 'error' in response else (response.get('returncode',0)!=0))
