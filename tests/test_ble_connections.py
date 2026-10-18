# SPDX-License-Identifier: MIT
"""Connection lifecycle, hopping and update-instants; no radio required."""
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from connections import Tracker, channel_for, channel_map

ALL = bytes.fromhex('ffffffff1f')
AA = '12345678'
CRC = 'abcdef'


def request(chsel=False, aa=AA):
    # WinSize=1, WinOffset=0, interval=24 (30 ms), latency=0,
    # timeout=200 (2 s), all channels, hop=10, SCA=0.
    return bytes([0x25 if chsel else 5, 34]) + bytes.fromhex('010203040506111213141516') + bytes.fromhex(aa)[::-1] + bytes.fromhex(CRC)[::-1] + bytes.fromhex('01000018000000c800') + ALL + bytes([10])


class ConnectionTest(unittest.TestCase):
    def setUp(self):
        self.tracker = Tracker()
        self.number = 0
        self.anchor = 1 + 44 * 8e-6 + .00125 + .0003

    def feed(self, b, t, ch=37, aa='8e89bed6', crc=CRC):
        self.number += 1
        p = dict(type='packet', id=self.number, capture_s=t, epoch=1,
                 sample=int(t*2e6), channel=ch, access_address=aa,
                 crc_init=crc, pdu=b.hex())
        self.tracker.ingest(p)
        return p

    def connect(self, chsel=False):
        self.feed(request(chsel), 1)
        return self.tracker.current[AA]

    def data(self, event, ch=None, payload=b'\x01\x00', **kw):
        return self.feed(payload, self.anchor + event*.03, (event+1)*10%37 if ch is None else ch, AA, **kw)

    def test_official_csa2_vectors(self):
        # Bluetooth Core Vol 6 Part C section 3, independent published vectors.
        aa = 0x8e89bed6
        self.assertEqual([channel_for(2, aa, 0, e, list(range(37))) for e in range(4)], [25,20,6,21])
        used = channel_map(bytes.fromhex('0006e0001e'))
        self.assertEqual([channel_for(2, aa, 0, e, used) for e in (6,7,8)], [23,9,34])

    def test_setup_addresses_and_hops(self):
        c = self.connect()
        self.assertEqual(c['central'], '06:05:04:03:02:01')
        self.assertEqual(c['peripheral'], '16:15:14:13:12:11')
        self.assertEqual(c['interval_ms'], 30)
        for e in range(20):
            p = self.data(e)
            self.assertEqual(p['connection_event'], e)
        self.assertEqual(c['count'], 20)

    def test_map_applies_at_instant_not_reception(self):
        c = self.connect()
        new = bytes.fromhex('fff7ffff1f')  # exclude channel 11
        for e in range(100):
            payload = b'\x03\x08\x01'+new+(100).to_bytes(2,'little') if e in (90,91) else b'\x01\x00'
            self.data(e, payload=payload)
        self.assertIn(11, c['used'])
        self.assertEqual(c['map_updates'], 1)  # retransmission deduplicated
        p = self.data(100, ch=12)
        self.assertEqual(p['connection_event'], 100)
        self.assertNotIn(11, c['used'])
        self.assertEqual(c['events'][-1]['kind'], 'map_active')
        self.assertEqual(channel_for(1, int(AA,16), 10, 100, c['used']), 12)

    def test_wrap_and_missed_events(self):
        c = self.connect()
        # Packet gaps are allowed; events advance regardless of reception.
        for e in range(0,65535,20): self.data(e)
        self.data(65534, payload=b'\x03\x08\x01'+bytes.fromhex('fff7ffff1f')+b'\x02\x00')
        self.assertEqual(c['maps'][-1]['event'], 65538)
        for e in (65535,65536,65537,65538):
            used=list(range(37)) if e<65538 else [ch for ch in range(37) if ch!=11]
            ch=channel_for(1,int(AA,16),10,e,used)
            self.assertEqual(self.data(e,ch)['connection_event'], e&65535)
        self.assertNotIn(11,c['used'])

    def test_inferred_and_encrypted_never_parse_map(self):
        bogus = b'\x03\x08\x01'+bytes.fromhex('fff7ffff1f')+b'\x06\x00'
        self.data(0, payload=bogus)
        c=self.tracker.current[AA]
        self.assertFalse(c['observed']);self.assertEqual(c['map_updates'],0)
        c=self.connect()
        self.data(0,payload=b'\x03\x17\x03'+bytes(22))
        self.data(1,payload=bogus)
        self.assertTrue(c['encrypted']);self.assertEqual(c['map_updates'],0)
        self.assertFalse(c['timing_valid'])

    def test_gap_and_restart(self):
        c=self.connect();self.data(0)
        self.tracker.discontinuity(dict(type='gap',capture_s=1.01,duration_s=.002))
        self.assertIsNone(self.data(1)['connection_event'])
        self.assertFalse(c['controls_trusted'])
        self.tracker.discontinuity(dict(type='capture_start',epoch=2,capture_s=2))
        self.assertTrue(c['ended'])
        self.data(40)
        self.assertEqual(len(self.tracker.connections),2)
        self.assertFalse(self.tracker.current[AA]['observed'])

    def test_termination_aa_reuse_crc_and_malformed(self):
        c=self.connect();self.data(0,payload=b'\x03\x02\x02\x13')
        self.assertTrue(c['ended'])
        self.data(1);self.assertFalse(self.tracker.current[AA]['observed'])
        self.data(2,crc='123456');self.assertEqual(len(self.tracker.connections),3)
        before=len(self.tracker.connections)
        bad=bytearray(request());bad[30:35]=bytes(5)
        self.feed(bad,2)
        self.assertEqual(len(self.tracker.connections),before)

    def test_chsel_negotiation_and_unseen_advertiser(self):
        c=self.connect(True);self.assertEqual(c['algorithms'],[1,2])
        self.tracker=Tracker()
        self.feed(bytes.fromhex('2006111213141516'),.999)
        c=self.connect(True);self.assertEqual(c['algorithms'],[2])
        for e in range(10):
            ch=channel_for(2,int(AA,16),10,e,list(range(37)))
            self.assertEqual(self.data(e,ch)['connection_event'],e)

    def test_interval_update_then_channel_map(self):
        c=self.connect()
        # Valid LL_CONNECTION_UPDATE_IND at event 0, instant 6.
        self.data(0,payload=bytes.fromhex('030c0001000030000000c8000600'))
        for e in range(1,6): self.data(e)
        self.assertEqual(self.data(6)['connection_event'],6)
        self.assertEqual(c['interval_ms'],60)
        self.assertTrue(c['timing_valid'])
        new_anchor=self.anchor+6*.03
        newmap=bytes.fromhex('fff7ffff1f')
        for e in range(7,25):
            used=list(range(37)) if e<15 else [ch for ch in range(37) if ch!=11]
            payload=b'\x03\x08\x01'+newmap+b'\x0f\x00' if e==7 else b'\x01\x00'
            p=self.feed(payload,new_anchor+(e-6)*.06,channel_for(1,int(AA,16),10,e,used),AA)
            self.assertEqual(p['connection_event'],e)
        self.assertNotIn(11,c['used'])

    def test_bounded_and_incremental_snapshot(self):
        c=self.connect()
        for e in range(1500): self.data(e)
        self.assertEqual(len(c['events']),1024)
        snap=self.tracker.snapshot(self.number-2)
        self.assertEqual(len(snap['connections'][0]['events']),2)
        self.assertTrue(snap['connections'][0]['history_truncated'])
        self.assertEqual(c['markers'][0]['kind'], 'connect')
        self.assertEqual(sum(b['count'] for b in c['activity']), 1500)
        self.assertTrue(all(b['id'] > self.number-2 for b in snap['connections'][0]['activity']))
        for n in range(140):
            self.feed(b'\x01\x00',50+n,ch=0,aa=f'{n+0x12340000:08x}')
        self.assertEqual(len(self.tracker.connections),128)
        self.assertEqual(len(self.tracker.current),128)

    def test_interval_update_window_offset_and_second_change(self):
        c=self.connect()
        self.data(0,payload=bytes.fromhex('030c0002020030000000c8000600'))
        for e in range(1,6): self.data(e)
        # New window starts 2.5 ms after the old event-6 anchor. First
        # central packet lands 0.5 ms into that new window.
        anchor=self.anchor+6*.03+.003
        for e in range(6,20):
            payload=bytes.fromhex('030c0001000018000000c8001400') if e==10 else b'\x01\x00'
            p=self.feed(payload,anchor+(e-6)*.06,(e+1)*10%37,AA)
            self.assertEqual(p['connection_event'],e)
        self.assertEqual(c['interval_ms'],60)
        anchor+=14*.06
        for e in range(20,30):
            p=self.feed(b'\x01\x00',anchor+(e-20)*.03,(e+1)*10%37,AA)
            self.assertEqual(p['connection_event'],e)
        self.assertEqual(c['interval_ms'],30)

    def test_snapshot_does_not_share_mutable_activity(self):
        c=self.connect();self.data(0)
        before=self.tracker.snapshot(0)
        self.data(1)
        self.assertEqual(before['connections'][0]['activity'][0]['count'],1)
        self.assertEqual(len(before['connections'][0]['activity'][0]['channels']),1)
        self.assertEqual(c['activity'][0]['count'],2)

    def test_capture_clock_preserves_loss_and_discarded_tail(self):
        code = r'''
#include "capture-clock.h"
#include <cassert>
#include <cmath>
int main() {
 CaptureClock c;
 c.input(2048); for(int i=0;i<25;i++) c.frame();
 assert(std::abs(c.packet(20) - (20/2e6-8e-6)) < 1e-12);
 c.segment(); c.missing(4096); c.input(2048);
 // The discarded 48 raw samples and missing 4096 samples remain in time.
 assert(std::abs(c.packet(25) - (6144/160e6-8e-6)) < 1e-12);
 assert(std::abs(c.now() - 8192/160e6) < 1e-12);
}'''
        with tempfile.TemporaryDirectory() as tmp:
            src=Path(tmp)/'test.cpp';src.write_text(code)
            subprocess.run(['c++','-std=c++17','-I',str(ROOT),str(src),'-o',tmp+'/test'],check=True)
            subprocess.run([tmp+'/test'],check=True)

if __name__ == '__main__': unittest.main()
