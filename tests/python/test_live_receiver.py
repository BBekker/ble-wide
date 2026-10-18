# SPDX-License-Identifier: MIT
import json
import os
from pathlib import Path
import struct
import socket
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from airspace import live_receiver as live
from airspace.server import State


def device(interface='en10', location=0x2200000, **kwargs):
    return dict(idVendor=0x0bda, idProduct=0x8157, bcdDevice=0x3000,
                kUSBCurrentConfiguration=1, locationID=location,
                IORegistryEntryChildren=[dict(IOObjectClass='IOUserNetworkEthernet',
                    IORegistryEntryChildren=[dict(IOObjectClass='IOSkywalkLegacyEthernetInterface',
                                                  IORegistryEntryName=interface)])], **kwargs)

INTERFACES = '''en0: flags=8863<UP> mtu 1500
    inet 192.168.1.2 netmask 0xffffff00
    status: active
en10: flags=8863<UP> mtu 9000
    inet 10.77.0.10 netmask 0xffffff00
    status: active
'''


class DetectionTest(unittest.TestCase):
    def test_selects_router_adapter_not_unrelated_supported_hardware(self):
        result = live.adapter_for_address([device('en0', 1), device()], INTERFACES, '10.77.0.10')
        self.assertEqual(result, dict(model='RTL8157', interface='en10', location=0x2200000))

    def test_unknown_revision_is_not_claimed(self):
        for key, value in [('idProduct', 0x8156), ('bcdDevice', 0x3100)]:
            candidate = device();candidate[key] = value
            with self.subTest(key=key):
                self.assertIsNone(live.adapter_for_address([candidate], INTERFACES, '10.77.0.10'))
        self.assertIsNone(live.adapter_for_address([device('en0')], INTERFACES, '10.77.0.10'))

    def test_cold_adapter_can_be_selected_without_link_or_vendor_configuration(self):
        candidate = device();candidate['kUSBCurrentConfiguration'] = 2
        self.assertIsNotNone(live.adapter_for_address([candidate], INTERFACES.replace('status: active', 'status: inactive'), '10.77.0.10'))
        candidate['IORegistryEntryChildren'] = []
        self.assertIsNone(live.adapter_for_address([candidate], INTERFACES, '10.77.0.10'))
        self.assertEqual(live.adapter_for_address([candidate], INTERFACES, '10.77.0.10', 0x2200000)['location'], 0x2200000)
        self.assertIsNone(live.adapter_for_address([candidate], INTERFACES, '10.77.0.10', 0x3300000))

    def test_ambiguous_adapters_are_not_claimed(self):
        self.assertIsNone(live.adapter_for_address([device(), device(location=2)], INTERFACES, '10.77.0.10'))
        self.assertIsNotNone(live.adapter_for_address([device(), device()], INTERFACES, '10.77.0.10'))

    def test_detection_never_uses_usb_authorization(self):
        with patch.object(live.sys, 'platform', 'linux'), patch.object(live.subprocess, 'check_output') as run:
            self.assertIsNone(live.detect_adapter('10.77.0.10'))
            run.assert_not_called()
        import plistlib
        with patch.object(live.sys, 'platform', 'darwin'), patch.object(live.subprocess, 'check_output', side_effect=[plistlib.dumps([device()]), INTERFACES]) as run:
            self.assertEqual(live.detect_adapter('10.77.0.10')['interface'], 'en10')
            self.assertEqual([x.args[0][0] for x in run.call_args_list], ['/usr/sbin/ioreg', '/sbin/ifconfig'])


class SelectionTest(unittest.TestCase):
    def runtime(self, transport='auto'):
        return live.LiveReceiver(SimpleNamespace(transport=transport, bind='10.77.0.10'), State())

    def execute(self, runtime, adapter, usb_error=None):
        order = []
        def usb(_):
            order.append('usb')
            if usb_error:raise usb_error
        def udp(*args, **kwargs):order.append(('udp', kwargs.get('fallback', False)))
        with tempfile.TemporaryDirectory() as tmp, patch.object(live, 'ROOT', Path(tmp)), \
             patch.object(live, 'detect_adapter', return_value=adapter) as detect, \
             patch.object(runtime, '_run_usb', side_effect=usb) as usb_call, \
             patch.object(runtime, '_run_udp', side_effect=udp), \
             patch.object(runtime, '_close_pipeline', side_effect=lambda: order.append('close')):
            runtime.run()
        return order, detect.call_count, usb_call.call_count

    def test_auto_prefers_usb_and_never_retries_after_failure(self):
        order, _, calls = self.execute(self.runtime(), device(), RuntimeError('Approval declined'))
        self.assertEqual(order, ['usb', 'close', ('udp', True), 'close'])
        self.assertEqual(calls, 1)

    def test_usb_selection_and_udp_override(self):
        order, _, _ = self.execute(self.runtime(), device())
        self.assertEqual(order, ['usb', 'close'])
        order, detections, calls = self.execute(self.runtime('udp'), device())
        self.assertEqual(order, [('udp', False), 'close'])
        self.assertEqual((detections, calls), (0, 0))

    def test_unsupported_adapter_uses_udp_without_usb_attempt(self):
        order, _, calls = self.execute(self.runtime(), None)
        self.assertEqual(order, [('udp', False), 'close'])
        self.assertEqual(calls, 0)

    def test_forced_usb_failure_is_visible_and_stop_does_not_fallback(self):
        runtime = self.runtime('usb')
        order, _, calls = self.execute(runtime, device(), RuntimeError('USB unplugged'))
        self.assertEqual(order, ['usb', 'close', 'close'])
        self.assertEqual(runtime.state.snapshot(0)['receiver']['phase'], 'error')
        order, _, _ = self.execute(self.runtime(), device(), InterruptedError())
        self.assertEqual(order, ['usb', 'close'])

    def test_only_iq_records_establish_readiness(self):
        runtime = self.runtime()
        with tempfile.TemporaryDirectory() as tmp:
            runtime.directory = Path(tmp)
            log = runtime.directory / 'receiver.log'
            log.write_text('USB frames=100\n' + json.dumps(dict(slices=0)) + '\n')
            self.assertFalse(runtime._radio_samples_received())
            log.write_text('log line\n' + json.dumps(dict(slices=20)) + '\n{"slices":')
            self.assertTrue(runtime._radio_samples_received())


class CaptureTest(unittest.TestCase):
    def test_sleep_wake_keeps_one_usb_authorization_and_pipeline(self):
        args = SimpleNamespace(name='/test', bind='127.0.0.1', ports=1)
        runtime = live.LiveReceiver(args, State())
        runtime.receiver = Mock();runtime.receiver.poll.return_value = None
        runtime.decoder = Mock();runtime.decoder.poll.return_value = None
        runtime.stopping = Mock()
        runtime.stopping.is_set.return_value = False
        runtime.stopping.wait.side_effect = [False, False, False, False, True]
        capture = Mock();capture.process.poll.return_value = None
        capture.counters.side_effect = [(1, 10), (4, 10), (5, 10), (3, 10), (1, 20)]
        with tempfile.TemporaryDirectory() as tmp, \
             patch.object(live.subprocess, 'run'), patch.object(live, 'USBCapture', return_value=capture), \
             patch.object(runtime, '_start_receiver') as receiver, \
             patch.object(runtime, '_start_decoder') as decoder, \
             patch.object(runtime, '_radio_samples_received', return_value=True), \
             patch.object(runtime, 'report') as report:
            runtime.directory = Path(tmp)
            runtime._run_usb(dict(model='RTL8157'))
        capture.start.assert_called_once()
        receiver.assert_called_once();decoder.assert_called_once()
        capture.close.assert_not_called()
        messages = [call.args[2] for call in report.call_args_list]
        self.assertTrue(any('sleeping' in text for text in messages))
        self.assertTrue(any('Resuming' in text for text in messages))
        self.assertEqual(report.call_args.args[:3], ('usb', 'receiving', 'Direct USB · RTL8157'))

    def test_usb_failure_reason_preserves_heartbeat_and_io_diagnostics(self):
        with tempfile.TemporaryDirectory() as tmp:
            capture = live.USBCapture(Path(tmp), Path(tmp)/'ring', {})
            self.assertEqual(capture.failure_reason('ended'), 'ended')
            log = Path(tmp)/'helper.log'
            log.write_text('Viewer heartbeat stopped; releasing USB adapter\nrtl8157-helper: USB receive failed\n')
            self.assertIn('heartbeat expired', capture.failure_reason('ended'))
            log.write_text('USB input completion: 0xe00002ed\nrtl8157-helper: USB receive failed\n')
            self.assertIn('0xe00002ed', capture.failure_reason('ended'))
            log.write_text('normal shutdown\n')
            self.assertEqual(capture.failure_reason('ended'), 'ended')

    @unittest.skipUnless((ROOT / 'build/w1700k-ble-wide').exists(), 'build the receiver and decoder first')
    def test_managed_udp_receives_samples_and_stops_both_processes(self):
        args = SimpleNamespace(transport='udp', bind='127.0.0.1', ports=1,
            name=f'/ble-managed-test-{os.getpid()}', backend='cpu', demod='streaming', decode_threads=1)
        state = State();runtime = live.LiveReceiver(args, state)
        with patch.object(live.USBCapture, 'start') as authorization:
            runtime.start()
            try:
                deadline = time.monotonic() + 15
                while state.snapshot(0)['receiver'].get('phase') != 'receiving':
                    self.assertTrue(runtime.thread.is_alive())
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.05)
                with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
                    for seq in range(1024):
                        sender.sendto(struct.pack('<IHHQ', 0x31385149, 0, 2048, seq) + bytes(4096), ('127.0.0.1', 7700))
                        if seq % 16 == 15:time.sleep(.002)
                while not state.snapshot(0)['status'].get('samples', 0):
                    self.assertLess(time.monotonic(), deadline)
                    time.sleep(.05)
                self.assertEqual(state.snapshot(0)['receiver']['transport'], 'udp')
                authorization.assert_not_called()
            finally:
                runtime.stop()
        self.assertFalse(runtime.thread.is_alive())
        self.assertIsNone(runtime.receiver)
        self.assertIsNone(runtime.decoder)

    def test_one_authorization_and_graceful_shared_ring_stop(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            bundle = directory / 'source.app'
            (bundle / 'Contents/MacOS').mkdir(parents=True)
            (bundle / 'Contents/MacOS/rtl8157-helper').touch()
            stage = directory / 'capture with spaces'
            stage.mkdir()
            ring = stage / 'usb.ring'
            contents = bytearray(4096)
            struct.pack_into('<Q', contents, 0, live.RING_MAGIC)
            ring.write_bytes(contents)
            capture = live.USBCapture(stage, ring, dict(location=0x2200000))
            with patch.object(live, 'HELPER', bundle), patch.object(live.subprocess, 'Popen') as launch:
                capture.start()
                self.assertEqual(launch.call_count, 1)
                self.assertEqual(launch.call_args.args[0][0], '/usr/bin/osascript')
                script = (stage / 'authorize.applescript').read_text()
                self.assertIn('--lease', script)
                self.assertIn('--location 0x2200000', script)
                self.assertEqual(script.count('with administrator privileges'), 1)
                capture.close()
            self.assertEqual(struct.unpack_from('<Q', ring.read_bytes(), 40)[0], 1)

    def test_receiver_status_survives_capture_reset(self):
        state = State()
        state.set_receiver(dict(transport='udp', phase='starting', message='Fallback'))
        previous = state.session
        state.reset_capture()
        self.assertNotEqual(state.session, previous)
        self.assertEqual(state.snapshot(0)['receiver']['transport'], 'udp')


if __name__ == '__main__':
    unittest.main()
