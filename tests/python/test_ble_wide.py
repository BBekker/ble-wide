# SPDX-License-Identifier: MIT
"""All BLE centers, simultaneous neighbors, IQ4, CRC contexts, and PFB seams."""
import json
import math
from pathlib import Path
import random
import struct
import subprocess
import sys
import tempfile
import unittest
from fixtures import SPEC_BITS, SPEC_PDU

ROOT=Path(__file__).resolve().parents[2]

class WideBLETest(unittest.TestCase):
    def test_decoder_and_all_bins(self):
        subprocess.run(["make","-C",str(ROOT)],check=True)
        exe=ROOT/"build/w1700k-ble-wide"
        with tempfile.TemporaryDirectory() as tmp:
            tmp=Path(tmp)
            test=tmp/"test-decoder"
            subprocess.run(["c++","-O2","-std=c++17","-I",str(ROOT/"src"),str(ROOT/"tests/cpp/test-decoder.cpp"),"-o",str(test)],check=True)
            subprocess.run([str(test)],check=True)
            def whiten(ch,n):
                w=ch|64;out=[]
                for _ in range(n):
                    out.append(w&1);w=(w>>1)^(0x44 if w&1 else 0)
                return out
            published=[int(b) for b in SPEC_BITS if b in '01']
            plain=[a^b for a,b in zip(published[40:],whiten(38,len(published)-40))]
            # Packets straddle a parallel worker boundary; adjacent channels
            # overlap in time. Random phases avoid an artificial coherent peak.
            count=196608
            wide=[0j]*count
            rng=random.Random(71)
            for ch in range(40):
                freq=2402 if ch==37 else 2426 if ch==38 else 2480 if ch==39 else 2404+2*ch if ch<11 else 2406+2*ch
                bits=published[:40]+[a^b for a,b in zip(plain,whiten(ch,len(plain)))]
                phase=rng.random()*2*math.pi
                for k in range(len(bits)*160):
                    phase+=2*math.pi*((freq-2441)*1e6+(250000 if bits[k//160] else -250000)+20000)/160e6
                    wide[155000+k]+=100*complex(math.cos(phase),math.sin(phase))
            raw=[((round(v.real)&16383)<<16)|(round(v.imag)&16383) for v in wide]
            path=tmp/'all40.raw';path.write_bytes(struct.pack(f'<{count}I',*raw))
            for mode in ('--raw14','--iq4','--iq8'):
                result=subprocess.run([str(exe),'--file',str(path),mode],capture_output=True,text=True,check=True)
                statuses=[json.loads(line) for line in result.stdout.splitlines() if '"type":"status"' in line]
                self.assertEqual(statuses[-1]['demodulator'], 'streaming')
                packets=[json.loads(line) for line in result.stdout.splitlines() if '"type":"packet"' in line]
                self.assertEqual({(p['channel'],p['pdu']) for p in packets},{(ch,SPEC_PDU.hex()) for ch in range(40)},result.stdout)
                self.assertEqual(len(packets),40)
                self.assertTrue(all(math.isfinite(p['signal_dbfs']) and -120 <= p['signal_dbfs'] <= 0
                                    for p in packets))
            if sys.platform == "darwin":
                # Move the same simultaneous packets across the Metal chunk boundary.
                padding = 655360-163840
                path.write_bytes(bytes(padding*4) + struct.pack(f'<{count}I',*raw))
                for demod in ('adaptive', 'streaming'):
                    expected = None
                    for backend, layout, workers in ((b, l, w) for b in ('cpu', 'metal')
                                                     for l in ('interleaved', 'planar')
                                                     for w in (1, 4, 40)):
                        result=subprocess.run([str(exe),'--file',str(path),'--iq8','--backend',backend,
                                               '--demod',demod,'--bin-layout',layout,'--decode-threads',str(workers)],
                                              capture_output=True,text=True,check=True)
                        packets=[json.loads(line) for line in result.stdout.splitlines() if '"type":"packet"' in line]
                        keys={(p['channel'],p['sample'],p['pdu']) for p in packets}
                        self.assertEqual(len(keys),40,result.stdout)
                        self.assertEqual({(p['channel'],p['pdu']) for p in packets},
                                         {(ch,SPEC_PDU.hex()) for ch in range(40)})
                        if expected is None: expected=keys
                        else: self.assertEqual(keys,expected)

if __name__=='__main__': unittest.main()
