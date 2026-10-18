#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local streaming BLE viewer. Capture stays on this Mac."""
import argparse
from connections import Tracker
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import threading
import subprocess
import signal
import time
import uuid
from urllib.parse import parse_qs, urlsplit

import advertisements as ads
HISTORY = 50000
SPECTRUM_HISTORY = 6000  # two minutes, 50 rows/second
WEB = Path(__file__).with_name("web")


def newer(items, after):
    """Read only the new tail of an ordered bounded ring."""
    result = []
    for item in reversed(items):
        if item["id"] <= after:
            break
        result.append(item)
    result.reverse()
    return result


class State:
    def __init__(self):
        self.session = uuid.uuid4().hex
        self.tracker = Tracker()
        self.lock = threading.RLock()
        self.changed = threading.Condition(self.lock)
        self.packets = deque(maxlen=HISTORY)
        self.spectrum = deque(maxlen=SPECTRUM_HISTORY)
        self.status = {}
        self.receiver = {}
        self.updated = 0
        self.number = self.spectrum_number = self.revision = 0

    def set_receiver(self, receiver):
        with self.changed:
            self.receiver = receiver
            self.revision += 1
            self.changed.notify_all()

    def reset_capture(self):
        with self.changed:
            self.session = uuid.uuid4().hex
            self.tracker = Tracker()
            self.packets.clear()
            self.spectrum.clear()
            self.status = {}
            self.updated = self.number = self.spectrum_number = 0
            self.revision += 1
            self.changed.notify_all()

    def ingest(self, event):
        with self.changed:
            kind = event.get("type")
            if kind == "status":
                self.status = event
                self.updated = time.monotonic()
                self.tracker.status(event)
            elif kind == "spectrum":
                values = event.get("db", [])
                if len(values) != 40:
                    return
                self.spectrum_number += 1
                event["id"] = self.spectrum_number
                self.spectrum.append(event)
            elif kind in ("gap", "capture_start"):
                self.tracker.discontinuity(event)
            elif kind == "packet":
                self.number += 1
                event.update(id=self.number, time=time.time(), kind="Data PDU", name="", address="")
                if event.get("crc_valid") is False:
                    event["kind"] = "CRC failed"
                elif event["access_address"] == "8e89bed6":
                    details = ads.advertisement(event["pdu"])
                    event.update({k: details[k] for k in ("kind", "name", "address", "fields")})
                    if details["extended"]:
                        event["kind"] = "Extended advertising"
                self.tracker.ingest(event)
                self.packets.append(event)
            else:
                return
            self.revision += 1
            self.changed.notify_all()

    def snapshot(self, after, spectrum_after=0):
        with self.lock:
            return {**self.tracker.snapshot(after), "session": self.session, "status": self.status, "receiver": self.receiver,
                    "active": time.monotonic()-self.updated < 2 and self.status.get("source_alive", False),
                    "packets": newer(self.packets, after), "cursor": self.number,
                    "spectrum": newer(self.spectrum, spectrum_after), "spectrum_cursor": self.spectrum_number,
                    "revision": self.revision, "history_limit": HISTORY,
                    "history_expired": bool(self.packets and after and after < self.packets[0]["id"]-1)}


def make_handler(state, bits=8):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            url = urlsplit(self.path)
            query = parse_qs(url.query)
            try:
                after = max(0, int(query.get("after", [0])[0]))
                spectrum_after = max(0, int(query.get("spectrum_after", [0])[0]))
            except ValueError:
                self.send_error(400)
                return
            if url.path == "/events":
                # Last-Event-ID includes the process session, so reconnecting
                # after a server restart cannot hide the beginning of a new run.
                last = self.headers.get("Last-Event-ID", "").split(":")
                if len(last) == 3 and last[0] == state.session:
                    try:
                        after, spectrum_after = max(0, int(last[1])), max(0, int(last[2]))
                    except ValueError:
                        after = spectrum_after = 0
                elif last != [""]:
                    after = spectrum_after = 0
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Accel-Buffering", "no")
                self.end_headers()
                self.connection.settimeout(5)
                revision = -1
                session = state.session
                try:
                    while True:
                        with state.changed:
                            state.changed.wait_for(lambda: state.revision != revision, timeout=1)
                        # At most 20 batches/sec; every decoded packet is included.
                        # Batching avoids one HTTP/event/render operation per packet.
                        if revision != -1:
                            time.sleep(.05)
                        with state.lock:
                            if state.session != session:
                                after = spectrum_after = 0
                                session = state.session
                            data = state.snapshot(after, spectrum_after)
                        after, spectrum_after, revision = data["cursor"], data["spectrum_cursor"], data["revision"]
                        body = json.dumps(data, separators=(",", ":"), allow_nan=False)
                        self.wfile.write(f"id: {data['session']}:{after}:{spectrum_after}\ndata: {body}\n\n".encode())
                        self.wfile.flush()
                except (OSError, TimeoutError):
                    return
            elif url.path == "/receiver":
                with state.lock:
                    data = json.dumps(dict(receiver=state.receiver,
                        active=time.monotonic()-state.updated < 2 and state.status.get("source_alive", False),
                        samples=state.status.get("samples", 0))).encode()
                kind = "application/json"
            elif url.path == "/api":
                data = json.dumps(state.snapshot(after, spectrum_after)).encode()
                kind = "application/json"
            elif url.path == "/":
                data = (WEB / "index.html").read_text().replace("{{BITS}}", str(bits)).encode()
                kind = "text/html; charset=utf-8"
            elif url.path in ("/app.js", "/app.css", "/plot-math.js", "/packet-model.js", "/connection-packets.js"):
                data = (WEB / url.path[1:]).read_bytes()
                kind = "text/javascript" if url.path.endswith(".js") else "text/css"
            else:
                self.send_error(404)
                return
            self.send_response(200)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            try:
                self.wfile.write(data)
            except OSError:
                pass

        def log_message(self, *_):
            pass
    return Handler


def follow(path, state):
    while True:
        try:
            with path.open() as f:
                inode = path.stat().st_ino
                while True:
                    line = f.readline()
                    if not line:
                        time.sleep(.02)
                        if not path.exists() or path.stat().st_ino != inode or path.stat().st_size < f.tell():
                            break
                        continue
                    try:
                        state.ingest(json.loads(line))
                    except (ValueError, KeyError, TypeError):
                        continue
        except FileNotFoundError:
            time.sleep(.5)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group()
    source.add_argument("--events", type=Path)
    source.add_argument("--stream", help="Attach to an externally managed sample stream; do not take over its adapter")
    source.add_argument("--receive", action="store_true", help="Manage reception automatically (the default)")
    p.add_argument("--transport", choices=["auto", "udp", "usb"], default="auto")
    p.add_argument("--usb-location", type=lambda s: int(s, 0), help="Explicit USB location ID (hex allowed); works without a stock-driver network interface")
    p.add_argument("--bind", default="10.77.0.10", help="Router export destination on this computer")
    p.add_argument("--ports", type=int, choices=range(1, 65), default=2, metavar="1..64")
    p.add_argument("--name", default="/w1700k-iq4", help="Sample stream created by managed reception")
    p.add_argument("--backend", choices=["cpu", "metal", "auto"], default="auto")
    p.add_argument("--demod", choices=["legacy", "adaptive", "preamble", "streaming"], default="streaming")
    p.add_argument("--decode-threads", type=int, choices=range(1, 41), default=4, metavar="1..40", help="CPU packet decoding workers")
    p.add_argument("--bits", type=int, choices=[4, 8], default=8)
    p.add_argument("--port", type=int, default=8076)
    a = p.parse_args()
    if a.usb_location is not None:
        if not 0 < a.usb_location <= 0xffffffff:
            p.error("--usb-location must be a nonzero 32-bit USB location ID")
        if a.transport == 'udp' or a.stream or a.events:
            p.error("--usb-location requires managed USB reception")
    state = State()
    child = None
    runtime = None
    if a.events:
        state.set_receiver(dict(transport="replay", phase="receiving", message="Recording playback"))
        threading.Thread(target=follow, args=(a.events, state), daemon=True).start()
    elif a.stream:
        state.set_receiver(dict(transport="external", phase="receiving", message="Externally managed receiver"))
        child = subprocess.Popen([str(Path(__file__).with_name("build")/"w1700k-ble-wide"),
                                  "--stream", a.stream, "--backend", a.backend, "--demod", a.demod, "--decode-threads", str(a.decode_threads)], stdout=subprocess.PIPE, text=True)
        def read():
            for line in child.stdout:
                try:
                    state.ingest(json.loads(line))
                except (ValueError, KeyError, TypeError):
                    continue
        threading.Thread(target=read, daemon=True).start()
    else:
        from live_receiver import LiveReceiver
        runtime = LiveReceiver(a, state)
    print(f"All-channel BLE viewer: http://127.0.0.1:{a.port}/", flush=True)
    def stop(*_):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, stop)
    try:
        with ThreadingHTTPServer(("127.0.0.1", a.port), make_handler(state, a.bits)) as server:
            if runtime:
                runtime.start()
            server.timeout = .5
            while child is None or child.poll() is None:
                server.handle_request()
    except KeyboardInterrupt:
        pass
    finally:
        if runtime and runtime.thread.ident is not None:
            runtime.stop()
        if child:
            child.terminate()
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()

if __name__ == "__main__":
    main()
