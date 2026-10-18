# SPDX-License-Identifier: MIT
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/benchmarks"))
from packet_pairing import pairing

class TransportHelpersTest(unittest.TestCase):
    def test_iq8_vector_unpack_matches_scalar_at_all_scales(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / 'unpack.c'
            binary = Path(tmp) / 'unpack'
            source.write_text(r'''
    #include "include/iq8.h"
    #include <stdio.h>
    int main(void) {
    uint8_t input[2*65536+1];
    uint32_t output[65536+2];
    const unsigned lengths[] = {0,1,7,8,9,15,16,17,31,32,33,65536};
    for (unsigned k=0;k<65536;k++) {
        input[1+2*k] = k >> 8;
        input[2+2*k] = k;
    }
    for (unsigned shift=0;shift<8;shift++) {
        for (unsigned j=0;j<sizeof(lengths)/sizeof(lengths[0]);j++) {
            unsigned n=lengths[j];
            output[0]=output[n+1]=0xdeadbeef;
            iq8_unpack_block(input+1,output+1,n,shift);
            if (output[0]!=0xdeadbeef || output[n+1]!=0xdeadbeef) return 1;
            for (unsigned k=0;k<n;k++) {
                if (output[k+1]!=iq8_unpack(input+1+2*k,shift)) {
                    fprintf(stderr,"mismatch shift=%u n=%u k=%u\n",shift,n,k);
                    return 2;
                }
            }
        }
    }
    return 0;
    }
    ''')
            subprocess.run(['cc', '-O3', '-I', str(ROOT), str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_packet_pairing(self):
        p=dict(channel=37,access_address='8e89bed6',pdu='0000',crc='123456',sample=100)
        matches,missing,extra=pairing([p,p|{'sample':200}],[p|{'sample':102},p|{'sample':205}])
        self.assertEqual((len(matches),len(missing),len(extra)),(1,1,1))
        matches,missing,extra=pairing([p,p],[p])
        self.assertEqual((len(matches),len(missing),len(extra)),(1,1,0))
