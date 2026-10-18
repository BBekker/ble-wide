# SPDX-License-Identifier: MIT
"""Synthetic RTL8157 USB batches -> unprivileged receiver -> public IQ ring."""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest
from stream_shm import Shm
from airspace.live_receiver import RING_BYTES
ROOT=Path(__file__).resolve().parents[2]

class USBStreamTest(unittest.TestCase):
    def test_usb_iq8_to_samples(self):
        self.check_format('--emit',80)

    def test_large_usb_batch_to_samples(self):
        self.check_format('--emit-batch',80)

    def test_unaligned_usb_packed14_to_samples(self):
        self.check_format('--emit-packed',40)

    def check_format(self, mode, slices):
        subprocess.run(['make','-C',str(ROOT/'receiver'),'all','check-usb'],check=True)
        name=f'/ble-usb-test-{os.getpid()}'
        with tempfile.TemporaryDirectory() as tmp:
            ring=Path(tmp)/'usb.ring'
            p=subprocess.Popen([str(ROOT/'receiver/build/w1700k-stream'),'--usb-ring',str(ring),
                '--name',name,'--ports','2','--ring-mib','1','--receive-mib','1'],stderr=subprocess.PIPE)
            shm=None
            try:
                deadline=time.monotonic()+10
                while time.monotonic()<deadline and p.poll() is None:
                    try:shm=Shm(name);break
                    except (OSError,AssertionError):time.sleep(.02)
                self.assertIsNotNone(shm)
                self.assertEqual(ring.stat().st_size,RING_BYTES)
                subprocess.run([str(ROOT/'receiver/build/test-usb'),mode,str(ring)],check=True,timeout=10)
                while time.monotonic()<deadline and shm.position()[1]<slices:time.sleep(.01)
                self.assertEqual(shm.position()[1],slices)
                self.assertEqual(shm.get('bad'),0)
                for slot in (0,1,38,slices-1):self.assertEqual(shm.samples(slot),[(44,-55)]*2048)
            finally:
                if shm:shm.close()
                if p.poll() is None:p.send_signal(signal.SIGTERM)
                _,err=p.communicate(timeout=10)
                self.assertEqual(p.returncode,0,err.decode())
