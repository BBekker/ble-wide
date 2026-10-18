# SPDX-License-Identifier: MIT
"""Viewer-owned reception, with read-only USB discovery and one capture authorization."""
import json
import mmap
import os
from pathlib import Path
import plistlib
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parent
HELPER = ROOT / 'receiver/build/BLEUSBHelper.app'
RING_BYTES = 4096 + 512 * 65600
RING_MAGIC = 0x3142585355424c42


def descendants(node):
    yield node
    for child in node.get('IORegistryEntryChildren', []):
        yield from descendants(child)


def adapter_for_address(devices, interfaces, address):
    """Only select tested hardware on the interface owning the export address."""
    names = []
    for block in re.split(r'(?=^[^\s:]+: flags=)', interfaces, flags=re.M):
        name = block.partition(':')[0]
        if re.search(r'^\s+inet ' + re.escape(address) + r'\s', block, re.M):
            if 'status: active' in block:
                names.append(name)
    candidates = []
    for root in devices:
        for node in descendants(root):
            if (node.get('idVendor'), node.get('idProduct'), node.get('bcdDevice')) != (0x0bda, 0x8157, 0x3000):
                continue
            if node.get('kUSBCurrentConfiguration') != 1:
                continue
            attached = {c.get('BSD Name') for c in descendants(node)}
            attached.update(c.get('IORegistryEntryName') for c in descendants(node)
                            if c.get('IOObjectClass') in ('IOEthernetInterface', 'IOSkywalkLegacyEthernetInterface'))
            matching = attached.intersection(names)
            if len(matching) == 1 and isinstance(node.get('locationID'), int):
                candidates.append(dict(model='RTL8157', interface=matching.pop(), location=node['locationID']))
    # A USB object can appear below a hub and as an enumeration root.
    candidates = {c['location']: c for c in candidates}
    return next(iter(candidates.values())) if len(candidates) == 1 else None


def detect_adapter(address):
    if sys.platform != 'darwin':
        return None
    registry = subprocess.check_output(['/usr/sbin/ioreg', '-a', '-r', '-c', 'IOUSBHostDevice'], timeout=10)
    interfaces = subprocess.check_output(['/sbin/ifconfig', '-a'], text=True, timeout=10)
    return adapter_for_address(plistlib.loads(registry), interfaces, address)


def terminate(process, timeout=8):
    if process is None:
        return
    if process.poll() is None:
        process.terminate()
    try:
        process.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)


class USBCapture:
    """Fixed helper executable; no root command server or installed service."""
    def __init__(self, directory, ring, adapter):
        self.directory = directory
        self.ring = ring
        self.adapter = adapter
        self.process = None
        self.mapping = None
        self.auth_output = None

    def start(self):
        with self.ring.open('r+b') as f:
            self.mapping = mmap.mmap(f.fileno(), 4096)
        if struct.unpack_from('<Q', self.mapping)[0] != RING_MAGIC:
            raise RuntimeError('Receiver USB ring was not initialized')
        bundle = self.directory / 'BLEUSBHelper.app'
        shutil.copytree(HELPER, bundle)
        log = self.directory / 'helper.log'
        log.touch(mode=0o600)
        args = [str(bundle / 'Contents/MacOS/rtl8157-helper'), '--receive', '--seconds', '86400',
                '--ring', str(self.ring), '--uid', str(os.getuid()),
                '--location', hex(self.adapter['location']), '--lease']
        command = shlex.join(args) + ' > ' + shlex.quote(str(log)) + ' 2>&1'
        script = self.directory / 'authorize.applescript'
        script.write_text('with timeout of 86500 seconds\n do shell script ' + json.dumps(command) +
                          ' with administrator privileges\nend timeout\n')
        self.auth_output = (self.directory / 'authorization.log').open('w')
        self.process = subprocess.Popen(['/usr/bin/osascript', str(script)], stdin=subprocess.DEVNULL,
                                        stdout=self.auth_output, stderr=subprocess.STDOUT)

    def counters(self):
        """Refresh the viewer's lease and inspect capture progress."""
        if self.mapping is None:
            return 0, 0
        struct.pack_into('<Q', self.mapping, 32, time.monotonic_ns())
        return struct.unpack_from('<QQ', self.mapping, 192)  # state, completed transfers

    def close(self):
        if self.mapping is not None:
            struct.pack_into('<Q', self.mapping, 40, 1)  # request graceful release
        if self.process is not None:
            try:
                self.process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                # Also cancels an unanswered approval dialog. If authorization
                # races cancellation, the helper sees stop_requested immediately.
                terminate(self.process, timeout=2)
        if self.mapping is not None:
            self.mapping.close()
            self.mapping = None
        if self.auth_output:
            self.auth_output.close()


class LiveReceiver:
    def __init__(self, args, state):
        self.args, self.state = args, state
        self.stopping = threading.Event()
        self.receiver = self.decoder = self.usb = None
        self.reader = None
        self.logs = []
        self.directory = None
        self.thread = threading.Thread(target=self.run, name='live-receiver', daemon=True)

    def report(self, transport, phase, message, adapter=None, fallback=False):
        self.state.set_receiver(dict(transport=transport, phase=phase, message=message,
                                     adapter=adapter, fallback=fallback))

    def start(self):
        self.thread.start()

    def stop(self):
        self.stopping.set()
        self.thread.join(timeout=60)

    def _log(self, name):
        log = (self.directory / name).open('w')
        self.logs.append(log)
        return log

    def _start_receiver(self, ring=None):
        args = [str(ROOT / 'receiver/build/w1700k-stream'), '--name', self.args.name,
                '--bind', self.args.bind, '--ports', str(self.args.ports), '--report', '1']
        if ring:
            args += ['--usb-ring', str(ring)]
        self.receiver = subprocess.Popen(args, stdout=self._log('receiver.log'), stderr=subprocess.STDOUT)
        # Receiver creates the USB file only after acquiring its sample ring.
        deadline = time.monotonic() + 10
        while not self.stopping.is_set():
            if self.receiver.poll() is not None:
                raise RuntimeError('Sample receiver could not start; another receiver may own this stream')
            if ring:
                if ring.exists() and ring.stat().st_size == RING_BYTES:
                    with ring.open('rb') as f:
                        if struct.unpack('<Q', f.read(8))[0] == RING_MAGIC:
                            return
            elif time.monotonic() > deadline - 9.5:
                return
            if time.monotonic() >= deadline:
                raise RuntimeError('Sample receiver did not become ready')
            self.stopping.wait(.05)
        raise InterruptedError()

    def _radio_samples_received(self):
        # The USB endpoint may also deliver ARP or other Ethernet frames. Only
        # parsed IQ records establish readiness for the decoder.
        with (self.directory / 'receiver.log').open('rb') as log:
            log.seek(0, 2)
            log.seek(max(0, log.tell() - 4096))
            lines = log.read().splitlines()
        for line in reversed(lines):
            try:
                if json.loads(line).get('slices', 0) > 0:
                    return True
            except (ValueError, AttributeError):
                pass
        return False

    def _start_decoder(self):
        if self.stopping.is_set():
            raise InterruptedError()
        self.state.reset_capture()
        self.decoder = subprocess.Popen([str(ROOT / 'build/w1700k-ble-wide'), '--stream', self.args.name,
            '--backend', self.args.backend, '--demod', self.args.demod, '--decode-threads', str(self.args.decode_threads)],
            stdout=subprocess.PIPE, stderr=self._log('decoder.log'), text=True)
        child = self.decoder
        def read():
            for line in child.stdout:
                try:
                    self.state.ingest(json.loads(line))
                except (ValueError, KeyError, TypeError):
                    continue
        self.reader = threading.Thread(target=read, daemon=True)
        self.reader.start()

    def _close_pipeline(self):
        # Release USB before stopping the sample receiver; the stock driver
        # can rematch before a UDP fallback binds to the restored interface.
        if self.usb:
            self.usb.close()
            self.usb = None
        terminate(self.decoder)
        if self.reader:
            self.reader.join(timeout=5)
            self.reader = None
        if self.decoder and self.decoder.stdout:
            self.decoder.stdout.close()
        self.decoder = None
        terminate(self.receiver)
        self.receiver = None
        for log in self.logs:
            log.close()
        self.logs = []

    def _run_usb(self, adapter):
        self.report('usb', 'starting', 'Preparing direct USB reception', adapter)
        with self._log('build.log') as log:
            subprocess.run(['make', '-C', str(ROOT / 'receiver'), 'usb-helper'], check=True,
                           stdout=log, stderr=subprocess.STDOUT, timeout=45)
        if self.stopping.is_set():
            raise InterruptedError()
        ring = self.directory / 'usb.ring'
        self._start_receiver(ring)
        self._start_decoder()
        self.usb = USBCapture(self.directory, ring, adapter)
        self.report('usb', 'authorizing', 'Approve the macOS prompt to start direct USB', adapter)
        self.usb.start()  # exactly one authorization attempt in this viewer run
        deadline = time.monotonic() + 180
        captured_at = None
        while not self.stopping.is_set():
            state, transfers = self.usb.counters()
            if self.usb.process.poll() is not None:
                raise RuntimeError('Direct USB was not authorized or could not start')
            if self.receiver.poll() is not None:
                raise RuntimeError('USB sample receiver stopped')
            if transfers and self._radio_samples_received():
                break
            if state == 1 and captured_at is None:
                captured_at = time.monotonic()
                self.report('usb', 'starting', 'Waiting for radio samples over direct USB', adapter)
            if captured_at is not None and time.monotonic() - captured_at > 15:
                raise RuntimeError('Direct USB received no radio samples')
            if time.monotonic() >= deadline:
                raise RuntimeError('Direct USB approval timed out')
            self.stopping.wait(.1)
        if self.stopping.is_set():
            raise InterruptedError()
        self.report('usb', 'receiving', 'Direct USB · RTL8157', adapter)
        while not self.stopping.wait(.25):
            if self.usb.process.poll() is not None or self.usb.counters()[0] == 2:
                raise RuntimeError('Direct USB capture ended')
            if self.receiver.poll() is not None or self.decoder.poll() is not None:
                raise RuntimeError('Direct USB sample processing stopped')

    def _run_udp(self, reason='', fallback=False):
        self.report('udp', 'starting', reason or 'Starting UDP reception', fallback=fallback)
        # On fallback, allow the normal driver to recreate the interface. No
        # further USB attempts or authorization prompts occur in this session.
        if fallback:
            deadline = time.monotonic() + 20
            while not self.stopping.is_set() and time.monotonic() < deadline:
                try:
                    interfaces = subprocess.check_output(['/sbin/ifconfig', '-a'], text=True, timeout=3)
                    if re.search(r'^\s+inet ' + re.escape(self.args.bind) + r'\s', interfaces, re.M):
                        break
                except (OSError, subprocess.SubprocessError):
                    pass
                self.stopping.wait(.5)
        if self.stopping.is_set():
            raise InterruptedError()
        self._start_receiver()
        self._start_decoder()
        self.report('udp', 'receiving', ('UDP · ' + reason) if reason else 'UDP reception', fallback=fallback)
        while not self.stopping.wait(.25):
            if self.receiver.poll() is not None or self.decoder.poll() is not None:
                raise RuntimeError('UDP sample processing stopped')

    def run(self):
        try:
            with tempfile.TemporaryDirectory(prefix='ble-viewer-', dir='/tmp') as temporary:
                self.directory = Path(temporary)
                try:
                    adapter = None
                    if self.args.transport != 'udp':
                        self.report('auto', 'detecting', 'Checking the connected adapter')
                        try:
                            adapter = detect_adapter(self.args.bind)
                        except (OSError, ValueError, subprocess.SubprocessError):
                            pass
                    if adapter:
                        try:
                            self._run_usb(adapter)
                            return
                        except InterruptedError:
                            return
                        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                            self._close_pipeline()
                            if self.args.transport == 'usb':
                                raise
                            self._run_udp(str(error), fallback=True)
                    elif self.args.transport == 'usb':
                        raise RuntimeError('No supported RTL8157 found on the configured router interface')
                    else:
                        self._run_udp('No supported direct-USB adapter detected' if self.args.transport == 'auto' else '')
                finally:
                    self._close_pipeline()
                    saved = ROOT / 'build/live-receiver'
                    saved.mkdir(parents=True, exist_ok=True)
                    for log in self.directory.glob('*.log'):
                        shutil.copy2(log, saved / log.name)
        except InterruptedError:
            pass
        except (OSError, RuntimeError, subprocess.SubprocessError) as error:
            self.report(self.args.transport, 'error', str(error))
            print(f'Receiver: {error}', file=sys.stderr, flush=True)
