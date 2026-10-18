#!/usr/bin/env python3
"""Temporary, single-authentication USB test runner; no installed service or sudo rule.

Run as root with a private socket path and the invoking user's UID. Only runs
this checkout's fixed USB test executable, never shell commands. That executable
is intentionally rebuilt during development: this session trusts the selected
user and checkout for its bounded lifetime, not arbitrary local clients.
"""
# SPDX-License-Identifier: MIT
import argparse
import ctypes
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parent
HELPER = ROOT / 'BLEUSBHelper.app/Contents/MacOS/rtl8157-helper'

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--socket', required=True, type=Path)
    p.add_argument('--uid', required=True, type=int)
    a = p.parse_args()
    if os.geteuid() != 0 or a.uid <= 0:
        p.error('needs root and a non-root client UID')
    parent = a.socket.parent.stat()
    if parent.st_uid != a.uid or parent.st_mode & 0o077:
        p.error('socket directory must be private and owned by the client')
    libc = ctypes.CDLL(None, use_errno=True)
    libc.getpeereid.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(a.socket));os.chmod(a.socket,0o600);os.chown(a.socket,a.uid,-1)
    listener.listen(1);listener.settimeout(1)
    deadline = time.monotonic()+7200
    try:
        while time.monotonic()<deadline:
            try: client,_=listener.accept()
            except socket.timeout:continue
            with client:
                uid,gid=ctypes.c_uint(),ctypes.c_uint()
                if libc.getpeereid(client.fileno(),ctypes.byref(uid),ctypes.byref(gid)) or uid.value!=a.uid:continue
                client.settimeout(5);data=b''
                try:
                    while not data.endswith(b'\n') and len(data)<8192:
                        chunk=client.recv(8192-len(data))
                        if not chunk:break
                        data+=chunk
                    req=json.loads(data)
                    if req.get('action')=='stop':client.sendall(b'{"stopped":true}\n');break
                    if req.get('action')!='run':raise ValueError('unknown action')
                    args=req.get('args',[])
                    if not isinstance(args,list) or any(not isinstance(x,str) or '\0' in x or len(x)>1024 for x in args):raise ValueError('invalid arguments')
                    timeout=min(120,max(1,float(req.get('timeout',30))))
                    # Isolated process group allows bounded cancellation of this test only.
                    proc=subprocess.Popen([str(HELPER),*args],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,
                        cwd=ROOT,start_new_session=True,env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','BLE_USB_CLIENT_UID':str(a.uid)})
                    try:out,_=proc.communicate(timeout=timeout)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid,signal.SIGTERM)
                        try:out,_=proc.communicate(timeout=3)
                        except subprocess.TimeoutExpired:
                            os.killpg(proc.pid,signal.SIGKILL);out,_=proc.communicate()
                    result={'returncode':proc.returncode,'output':out.decode(errors='replace')[-131072:]}
                except Exception as e:result={'error':str(e)}
                try:client.sendall(json.dumps(result).encode()+b'\n')
                except OSError:pass
    finally:
        listener.close();a.socket.unlink(missing_ok=True)

if __name__=='__main__':main()
