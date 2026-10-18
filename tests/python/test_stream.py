# SPDX-License-Identifier: MIT
"""w1700k-stream against a fake export over loopback, read through its shared memory: samples
unpacked bit-exact, events and losses tagged, runs restarted on request, and the export's
integrity counted."""
import os
import shutil
import signal
import socket
import struct
import subprocess
import time
import unittest

import stream_shm
from stream_shm import TAG_EVENT, TAG_LOST, Shm, tag

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
STREAM = os.path.join(ROOT, "receiver")
BINARY = os.path.join(STREAM, "build", "w1700k-stream")
MAGIC = 0x31524453
SLICE = stream_shm.SLICE

def build_problem():
    if not shutil.which("make") or not shutil.which(os.environ.get("CXX", "c++")):
        return "no make or C++ compiler"
    return None


def free_udp_port(count=1):
    while True:
        sockets = []
        try:
            first = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(first)
            first.bind(("127.0.0.1", 0))
            port = first.getsockname()[1]
            if port + count - 1 > 65535:
                continue
            for offset in range(1, count):
                extra = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                sockets.append(extra)
                extra.bind(("127.0.0.1", port + offset))
            return port
        except OSError:
            continue
        finally:
            for sock in sockets:
                sock.close()


def word(i, q):
    return ((i & 0x3FFF) << 16) | (q & 0x3FFF)


def signed14(v):
    return v - 0x4000 if v & 0x2000 else v


def noise_words(seed):
    """A slice of distinct pseudo-random 14-bit samples, spare bits clear."""
    return [((seed * 7919 + n) * 2654435761) & 0x3FFF3FFF for n in range(SLICE)]


def pack(words):
    out = []
    for g in range(0, len(words), 8):
        w = words[g:g + 8]
        out.extend(w[k] | ((w[7] << (14 - 2 * k)) & 0xC000C000) for k in range(7))
    return out


def unpacked(words):
    return [(signed14(w >> 16 & 0x3FFF), signed14(w & 0x3FFF)) for w in words]


@unittest.skipIf(build_problem(), build_problem())
class StreamServiceTest(unittest.TestCase):
    counter = 0

    @classmethod
    def setUpClass(cls):
        result = subprocess.run(["make", "-C", STREAM], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError("build failed:\n" + result.stdout + result.stderr)

    def setUp(self):
        StreamServiceTest.counter += 1
        self.name = "/w1700k-test-%d-%d" % (os.getpid(), self.counter)
        self.ports = 2 if self._testMethodName == "test_two_ports_merge_by_dma_sequence" else 1
        self.port = free_udp_port(self.ports)
        self.proc = subprocess.Popen([BINARY, "--bind", "127.0.0.1", "--port", str(self.port), "--name", self.name,
                                      "--ring-mib", "16", "--rx-threads", "2", "--ports", str(self.ports)],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        deadline = time.monotonic() + 10
        while True:
            try:
                self.shm = Shm(self.name)
                break
            except (OSError, AssertionError):
                if time.monotonic() > deadline or self.proc.poll() is not None:
                    self.proc.terminate()
                    _, error = self.proc.communicate(timeout=10)
                    raise AssertionError("w1700k-stream did not start: " + error.decode())
                time.sleep(0.05)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        time.sleep(0.2)

    def tearDown(self):
        self.sock.close()
        self.shm.close()
        self.proc.send_signal(signal.SIGINT)
        self.assertEqual(self.proc.wait(10), 0)
        self.proc.stderr.close()
        self.assertFalse(stream_shm.exists(self.name), "the object is unlinked at exit")

    def send(self, seq, payload):
        self.sock.sendto(struct.pack("<IHHQ", MAGIC, 0, len(payload), seq) + payload, ("127.0.0.1", self.port))

    def send_words(self, seq, words):
        self.send(seq, struct.pack("<%dI" % len(words), *words))
        if seq % 32 == 31:
            time.sleep(0.002)  # loopback: let the receivers keep up

    def wait_newest(self, n, timeout=10):
        """The run once `n` slots are published, and the counters refreshed (every 100 ms)."""
        deadline = time.monotonic() + timeout
        while True:
            run, newest = self.shm.position()
            if newest >= n:
                time.sleep(0.25)
                return run
            if time.monotonic() > deadline:
                raise AssertionError("published %d of %d" % (newest, n))
            time.sleep(0.02)

    def test_slices_unpacked_bit_exact_events_and_losses_tagged(self):
        words = {s: noise_words(s) for s in range(1200)}
        for s in range(1200):
            if s == 20:
                self.send(s, b"\x00" * 64)             # a firmware event
            elif s == 30:
                continue                               # lost
            elif s == 10:
                self.send_words(s, pack(words[s]))     # packed
            else:
                self.send_words(s, words[s])
        run = self.wait_newest(1200)
        self.assertEqual(self.shm.tag(20), tag(run, 20) | TAG_EVENT)
        self.assertEqual(self.shm.tag(30), tag(run, 30) | TAG_LOST)
        for s in (0, 9, 10, 11, 31, 1199):
            self.assertEqual(self.shm.tag(s), tag(run, s), "slot %d" % s)
            self.assertEqual(self.shm.samples(s), unpacked(words[s]), "slot %d" % s)
        self.assertEqual((self.shm.get("packed"), self.shm.get("events"), self.shm.get("lost")), (1, 1, 1))
        self.assertEqual(self.shm.get("slices"), 1198)

    def test_iq4_scale_sign_and_malformed_packets(self):
        payload = bytes(range(256)) * 8
        for shift in range(12):
            packet = struct.pack("<IHHQ", 0x31345149, shift, SLICE, shift) + payload
            self.sock.sendto(packet, ("127.0.0.1", self.port))
        self.send(12, bytes(64))  # A genuine event remains an event, not IQ4.
        for shift, count, body in ((12, SLICE, payload), (1, 100, payload), (1, SLICE, payload[:-1])):
            self.sock.sendto(struct.pack("<IHHQ", 0x31345149, shift, count, 13) + body,
                             ("127.0.0.1", self.port))
        run = self.wait_newest(13)
        for shift in range(12):
            expected = []
            for b in payload:
                vals = [((v ^ 8) - 8) * (1 << shift) for v in (b >> 4, b & 15)]
                expected.append(tuple(max(-8192, min(8191, v)) for v in vals))
            self.assertEqual(self.shm.samples(shift), expected)
            self.assertEqual(self.shm.tag(shift), tag(run, shift))
        self.assertEqual(self.shm.tag(12), tag(run, 12) | TAG_EVENT)
        self.assertEqual(self.shm.get("packed"), 12)
        self.assertEqual(self.shm.get("bad"), 3)

    def test_two_ports_merge_by_dma_sequence(self):
        payload = bytes(range(256)) * 16
        self.sock.sendto(struct.pack("<IHHQ", 0x31385149, 2, SLICE, 0) + payload,
                         ("127.0.0.1", self.port))
        time.sleep(.05)  # establish a run before deliberately reversing each pair
        for base in range(1, 1200, 2):
            for seq in (base+1, base):
                self.sock.sendto(struct.pack("<IHHQ", 0x31385149, 2, SLICE, seq) + payload,
                                 ("127.0.0.1", self.port + seq % 2))
            if base % 32 == 1: time.sleep(.002)
        run = self.wait_newest(1201)
        self.assertEqual(self.shm.get("lost"), 0)
        self.assertEqual(self.shm.get("slices"), 1201)
        for seq in range(1201):
            self.assertEqual(self.shm.tag(seq), tag(run, seq))
        self.assertEqual(self.shm.samples(1), self.shm.samples(2))

    def test_iq8_jumbo_order_scales_and_atomic_rejection(self):
        payload = bytes(range(256))*16
        def record(seq, shift=2, count=SLICE, magic=0x31385149):
            return struct.pack("<IHHQ", magic, shift, count, seq)+payload
        def send(packet): self.sock.sendto(packet, ("127.0.0.1", self.port))
        send(record(0,0)+record(2,7))
        send(record(1,1)+record(3,6))
        self.send(4, bytes(64))
        good = record(5)+record(7)
        for packet in (record(5)+record(7,8), record(5)+record(7,count=2047),
                       record(5)+record(7,magic=MAGIC), good[:-1], good+b'x',
                       record(5,8)+record(7), record(5,count=2047)+record(7)):
            send(packet)
        send(record(6,3)+record(8,4))
        send(record(0,0)+record(2,7))  # two duplicate records, one datagram
        for seq in range(9,1200): self.send_words(seq,noise_words(seq))
        run=self.wait_newest(1200)
        for seq,shift in ((0,0),(1,1),(2,7),(3,6),(6,3),(8,4)):
            vals=[max(-8192,min(8191,((b^128)-128)*(1<<shift))) for b in payload]
            self.assertEqual(self.shm.samples(seq),list(zip(vals[::2],vals[1::2])))
            self.assertEqual(self.shm.tag(seq),tag(run,seq))
        # A malformed second record must not partially publish the first.
        for seq in (5,7): self.assertEqual(self.shm.tag(seq),tag(run,seq)|TAG_LOST)
        self.assertEqual(self.shm.get("packed"),6)
        self.assertEqual(self.shm.get("events"),1)
        self.assertEqual(self.shm.get("lost"),2)
        self.assertEqual(self.shm.get("duplicates"),2)
        self.assertEqual(self.shm.get("bad"),7)
        self.assertEqual(self.shm.get("datagrams"),1203)

    def test_iq8_scale_sign_and_malformed_packets(self):
        payload = bytes(range(256)) * 16
        for shift in range(8):
            self.sock.sendto(struct.pack("<IHHQ", 0x31385149, shift, SLICE, shift) + payload,
                             ("127.0.0.1", self.port))
        self.send(8, bytes(64))
        for shift, count, body in ((8, SLICE, payload), (1, 100, payload),
                                   (1, SLICE, payload[:-1]), (1, SLICE, payload[:2048])):
            self.sock.sendto(struct.pack("<IHHQ", 0x31385149, shift, count, 9) + body,
                             ("127.0.0.1", self.port))
        run = self.wait_newest(9)
        for shift in range(8):
            vals = [max(-8192, min(8191, ((b ^ 128)-128)*(1 << shift))) for b in payload]
            self.assertEqual(self.shm.samples(shift), list(zip(vals[::2], vals[1::2])))
            self.assertEqual(self.shm.tag(shift), tag(run, shift))
        self.assertEqual(self.shm.tag(8), tag(run, 8) | TAG_EVENT)
        self.assertEqual(self.shm.get("packed"), 8)
        self.assertEqual(self.shm.get("bad"), 4)

    def test_truncated_datagram_does_not_poison_next_batch(self):
        # recvmsg_x stops a batch at a truncated datagram. The next receive
        # must clear MSG_TRUNC and retain the full capacity of every buffer.
        self.send_words(0, noise_words(0))
        packet = struct.pack("<IHHQ", 0x31385149, 0, SLICE, 1) + bytes(12000)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
        self.sock.sendto(packet, ("127.0.0.1", self.port))
        for seq in range(1, 160):
            self.send_words(seq, noise_words(seq))
        run = self.wait_newest(160)
        self.assertEqual(self.shm.get("bad"), 1)
        self.assertEqual(self.shm.get("lost"), 0)
        self.assertEqual(self.shm.get("datagrams"), 161)
        for seq in range(160):
            self.assertEqual(self.shm.tag(seq), tag(run, seq))
            self.assertEqual(self.shm.samples(seq), unpacked(noise_words(seq)))

    def test_idle_tail_published_without_a_full_chunk(self):
        # Each burst ends before another full 16-slot chunk. Publication must
        # finish on idle, including an event that carries no samples.
        seq = 0
        for count in (1, 15, 17, 65):
            first = seq
            for offset in range(count):
                if offset == 2:
                    self.send(seq, bytes(64))
                else:
                    self.send_words(seq, noise_words(seq))
                seq += 1
            run = self.wait_newest(seq)
            for index in range(first, seq):
                event = index - first == 2
                self.assertEqual(self.shm.tag(index), tag(run, index) | (TAG_EVENT if event else 0))
                if not event:
                    self.assertEqual(self.shm.samples(index), unpacked(noise_words(index)))
            self.assertEqual(self.shm.get("lost"), 0)

    def test_restart_and_integrity_counters(self):
        base = [noise_words(s) for s in range(128)]
        for s in range(300):
            w = list(base[s % 128])                     # every slice repeats one lap later: stale
            if s == 3:
                w[0] |= 0xC0000000                      # spare bits set in a raw slice
            self.send_words(s, w)
        run = self.wait_newest(300)
        self.assertEqual(self.shm.get("spare_words"), 1)
        self.assertEqual(self.shm.get("stale_slices"), 300 - 128 - 1)  # 131 differs from 3 by the spare bit
        self.assertGreater(self.shm.get("stale_lines"), 0)
        self.shm.set("restart_requests", self.shm.get("restart_requests") + 1)
        time.sleep(0.3)
        for s in range(300, 340):
            self.send_words(s, noise_words(s))
        deadline = time.monotonic() + 10
        while self.shm.position()[0] == run:
            self.assertLess(time.monotonic(), deadline, "no new run")
            time.sleep(0.02)
        run2 = self.wait_newest(40)
        self.assertEqual(run2, run + 1)
        self.assertEqual(self.shm.samples(0), unpacked(noise_words(300)), "the new run counts from its first slice")


if __name__ == "__main__":
    unittest.main()
