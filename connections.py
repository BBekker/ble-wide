# SPDX-License-Identifier: MIT
"""Passive LE 1M connection tracking (Core Vol 6, Part B, 4.5 and 5.1.2).

Packet times are capture-clock seconds at the preamble, not arrival wall time.
An event number is an inference constrained by timing AND the hopping sequence.
We never infer plaintext control PDUs for streams joined after their setup.
"""
from collections import OrderedDict, deque
import math

ADV_AA = "8e89bed6"
MAX_CONNECTIONS = 128
MAX_POINTS = 1024


def channel_map(data):
    if len(data) != 5 or data[4] & 0xe0:
        raise ValueError("invalid channel map")
    used = [ch for ch in range(37) if data[ch // 8] & (1 << (ch % 8))]
    if len(used) < 2:
        raise ValueError("channel map needs at least two channels")
    return used


def channel_for(algorithm, aa, hop, event, used):
    if algorithm == 1:
        # Do not reset the unmapped progression at a map update or 16-bit wrap.
        unmapped = ((event + 1) * hop) % 37
        return unmapped if unmapped in used else used[unmapped % len(used)]
    identifier = (aa >> 16) ^ (aa & 0xffff)
    value = (event & 0xffff) ^ identifier
    for _ in range(3):
        value = ((value & 0xaaaa) >> 1) | ((value & 0x5555) << 1)
        value = ((value & 0xcccc) >> 2) | ((value & 0x3333) << 2)
        value = ((value & 0xf0f0) >> 4) | ((value & 0x0f0f) << 4)
        value = (17 * value + identifier) & 0xffff
    value ^= identifier
    unmapped = value % 37
    return unmapped if unmapped in used else used[(len(used) * value) >> 16]


def address(data):
    return ":".join(f"{b:02x}" for b in reversed(data))


class Tracker:
    def __init__(self):
        self.connections = OrderedDict()
        self.current = {}
        self.advertisers = OrderedDict()
        self.serial = 0
        self.epoch = None
        self.now = 0.0
        self.gaps = deque(maxlen=256)

    def status(self, event):
        self.now = max(self.now, event.get("capture_s", self.now))

    def discontinuity(self, event):
        self.now = max(self.now, event["capture_s"])
        if event["type"] == "capture_start":
            for c in self.connections.values():
                if not c["ended"]:
                    c["state"] = "Capture restarted"
                    c["ended"] = True
            self.current.clear()
            self.advertisers.clear()
            self.epoch = event["epoch"]
            return
        start, end = event["capture_s"], event["capture_s"] + event.get("duration_s", 0)
        if self.gaps and start <= self.gaps[-1]["end"] + .000001:
            self.gaps[-1]["end"] = max(end, self.gaps[-1]["end"])
        else:
            self.gaps.append({"start": start, "end": end})
        for c in self.connections.values():
            if not c["ended"]:
                c["controls_trusted"] = False
                c["warning"] = "Input gap: control updates may have been missed"
                # Firmware events have unknown duration; known sequence loss has
                # a preserved clock, but neither proves that interval/map stayed fixed.
                c["timing_valid"] = False

    def _new(self, packet, observed, aa, crc):
        self.serial += 1
        c = dict(id=str(self.serial), access_address=aa, crc_init=crc,
                 observed=observed, start=packet["capture_s"], last=packet["capture_s"],
                 central="", peripheral="", interval_ms=None, timeout_ms=4000,
                 latency=None, hop=None, algorithms=[], used=[], maps=[],
                 events=deque(maxlen=MAX_POINTS), markers=deque(maxlen=128),
                 activity=deque(maxlen=1201), count=0, channels=set(),
                 state="Setup observed" if observed else "Setup not captured",
                 ended=False, encrypted=False, controls_trusted=observed,
                 timing_valid=False, warning="" if observed else "Encryption and hopping parameters unknown",
                 last_event=None, map_updates=0, seen_controls=deque(maxlen=128))
        self.connections[c["id"]] = c
        self.current[aa] = c
        while len(self.connections) > MAX_CONNECTIONS:
            _, old = self.connections.popitem(last=False)
            if self.current.get(old["access_address"]) is old:
                del self.current[old["access_address"]]
        return c

    def _point(self, c, p, kind="packet", **fields):
        point = dict(id=p["id"], time=p["capture_s"], channel=p["channel"], kind=kind, **fields)
        c["events"].append(point)
        if kind != "packet":
            c["markers"].append(point)
        c["last"] = max(c["last"], p["capture_s"])
        p["connection_id"] = c["id"]
        return point

    def _mark(self, c, point, **fields):
        if point["kind"] == "packet":
            c["markers"].append(point)
        point.update(**fields)

    def _connect(self, p, b):
        if len(b) != 36 or b[1] != 34:
            return
        used = channel_map(b[30:35])
        interval = int.from_bytes(b[24:26], "little")
        latency = int.from_bytes(b[26:28], "little")
        timeout = int.from_bytes(b[28:30], "little")
        offset = int.from_bytes(b[22:24], "little")
        hop = b[35] & 31
        if not (6 <= interval <= 3200 and 0 <= latency <= 499 and
                10 <= timeout <= 3200 and timeout * 8 > 2 * (latency + 1) * interval and
                1 <= b[21] <= min(8, interval - 1) and offset <= interval and 5 <= hop <= 16):
            return
        aa = f"{int.from_bytes(b[14:18], 'little'):08x}"
        crc = f"{int.from_bytes(b[18:21], 'little'):06x}"
        previous = self.current.get(aa)
        if previous:
            if previous.get("request") == b.hex() and p["capture_s"] - previous["start"] < .002:
                return  # duplicate timing/channel detection, not a new session
            previous["ended"] = True
            previous["state"] = "Access address reused"
        c = self._new(p, True, aa, crc)
        advertiser_key = (bytes(b[8:14]), bool(b[0] & 0x80))
        advertiser = self.advertisers.get(advertiser_key)
        # Only a nearby preceding connectable advertisement establishes ChSel.
        adv_chsel = advertiser[1] if advertiser and 0 <= p["capture_s"] - advertiser[0] < .02 else None
        algorithms = [1] if not b[0] & 0x20 or adv_chsel is False else [2] if adv_chsel else [1, 2]
        end = p["capture_s"] + (1 + 4 + len(b) + 3) * 8e-6
        low = end + .00125 * (1 + offset)
        c.update(central=address(b[2:8]), peripheral=address(b[8:14]),
                 central_random=bool(b[0] & 0x40), peripheral_random=bool(b[0] & 0x80),
                 interval_ms=interval * 1.25, latency=latency, timeout_ms=timeout * 10,
                 hop=hop, algorithms=algorithms, used=used, request=b.hex(),
                 anchor_low=low - .000016, anchor_high=low + b[21] * .00125 + .000016,
                 anchor_checked=p["capture_s"], anchor_event=0, timing_valid=True, pending_timing=None,
                 maps=[dict(event=0, used=used)], algorithm_evidence=set())
        self._point(c, p, "connect", label="CONNECT_IND")

    def _used(self, c, event):
        return next(m["used"] for m in reversed(c["maps"]) if m["event"] <= event)

    def _event(self, c, p):
        if not c["timing_valid"]:
            return None
        t, interval = p["capture_s"], c["interval_ms"] / 1000
        # Oscillator drift expands the possible event anchor rather than silently
        # assuming that every first captured PDU was the central's first PDU.
        drift = max(0, t - c["anchor_checked"]) * .00055
        low, high = c["anchor_low"] - drift, c["anchor_high"] + drift
        pending = c["pending_timing"]
        base = c["anchor_event"]
        schedules = [(base, interval, low, high, False)]
        if pending:
            offset = (pending["event"] - base) * interval + pending["offset_s"]
            schedules.append((pending["event"], pending["interval_ms"] / 1000,
                              low + offset, high + offset + pending["window_s"], True))
        candidates = []
        bounds = {}
        for base, period, lo, hi, updated in schedules:
            first = max(base, base + math.floor((t - hi) / period))
            last = base + math.floor((t - lo) / period)
            if pending and not updated:
                last = min(last, pending["event"] - 1)
            if last - first > 4:
                c["warning"] = "Event timing uncertain after a long capture gap"
                return None
            for event in range(first, last + 1):
                for algorithm in c["algorithms"]:
                    if channel_for(algorithm, int(c["access_address"], 16), c["hop"], event, self._used(c, event)) == p["channel"]:
                        candidates.append((event, algorithm))
                        bounds[event] = (base, period, lo, hi, updated)
        events = {e for e, _ in candidates}
        if len(events) != 1:
            c["warning"] = "Packet does not uniquely match the known hopping schedule"
            return None
        event = events.pop()
        base, interval, low, high, updated = bounds[event]
        c["anchor_low"] = max(low, t - (event - base + 1) * interval)
        c["anchor_high"] = min(high, t - (event - base) * interval)
        c["anchor_checked"] = t
        if updated:
            c["anchor_event"] = base
            for key in ("interval_ms", "latency", "timeout_ms"):
                c[key] = pending[key]
            c["pending_timing"] = None
        algorithms = {a for _, a in candidates}
        if len(algorithms) == 1 and len(c["algorithms"]) > 1:
            c["algorithm_evidence"].add((event, next(iter(algorithms))))
            for algorithm in algorithms:
                if sum(a == algorithm for _, a in c["algorithm_evidence"]) >= 2:
                    c["algorithms"] = [algorithm]
                    c["algorithm_evidence"].clear()
        c["last_event"] = max(event, c["last_event"] or 0)
        c["used"] = self._used(c, c["last_event"])
        if c["controls_trusted"]:
            c["warning"] = ""
        return event

    def ingest(self, p):
        if "capture_s" not in p:
            # Old JSONL is still viewable, but has no reliable clock across loss.
            p["capture_s"] = p["sample"] / 2e6
            p["legacy_clock"] = True
        self.now = max(self.now, p["capture_s"])
        if p.get("crc_valid") is False:
            # Attribute diagnostic frames only to an existing matching context.
            # Damaged headers/control bytes cannot create or mutate a connection.
            c = self.current.get(p["access_address"])
            if c and not c["ended"] and c["crc_init"] == p["crc_init"]:
                p["connection_id"] = c["id"]
            return
        b = bytes.fromhex(p["pdu"])
        if len(b) < 2 or len(b) != b[1] + 2:
            return
        if p["access_address"] == ADV_AA:
            if p["channel"] < 37:
                return  # AUX_CONNECT_REQ has different timing/ChSel semantics
            kind = b[0] & 15
            if kind in (0, 1) and len(b) >= 8:
                key = (bytes(b[2:8]), bool(b[0] & 0x40))
                self.advertisers[key] = (p["capture_s"], bool(b[0] & 0x20))
                self.advertisers.move_to_end(key)
                if len(self.advertisers) > 1024:
                    self.advertisers.popitem(last=False)
            elif kind == 5:
                try:
                    self._connect(p, b)
                except ValueError:
                    pass
            return
        if p["channel"] >= 37:
            return
        c = self.current.get(p["access_address"])
        if c is None or c["crc_init"] != p["crc_init"] or c["ended"]:
            c = self._new(p, False, p["access_address"], p["crc_init"])
        if p.get("legacy_clock"):
            c["timing_valid"] = False
            c["controls_trusted"] = False
            c["warning"] = "Recording lacks a loss-aware capture clock"
        previous_map = c["used"]
        previous_interval = c["interval_ms"]
        previous_anchor = c.get("anchor_event")
        event = self._event(c, p) if c["observed"] else None
        p["connection_event"] = None if event is None else event & 0xffff
        point = self._point(c, p, event=p["connection_event"], event_index=event,
                            bytes=len(b), encrypted=c["encrypted"])
        if previous_map != c["used"]:
            self._mark(c, point, kind="map_active", label=f"New channel map active by event {event & 0xffff}")
        if previous_interval != c["interval_ms"] or previous_anchor != c.get("anchor_event"):
            self._mark(c, point, kind="timing_active", label=f"Interval {c['interval_ms']} ms active by event {event & 0xffff}")
        # Keep two minutes of activity even when the detailed packet ring turns
        # over quickly. These are explicitly 100 ms aggregates, not fake packets.
        bucket = int(p["capture_s"] * 10)
        if not c["activity"] or c["activity"][-1]["bucket"] != bucket:
            c["activity"].append(dict(bucket=bucket, id=p["id"], time=p["capture_s"],
                                      end=p["capture_s"], count=0, channels={}))
        activity = c["activity"][-1]
        activity["count"] += 1
        activity["id"] = p["id"]
        activity["end"] = p["capture_s"]
        ch = str(p["channel"])
        activity["channels"][ch] = activity["channels"].get(ch, 0) + 1
        c["count"] += 1
        c["channels"].add(p["channel"])
        if c["observed"]:
            c["state"] = "Traffic observed"
        if b[0] & 3 != 3 or len(b) < 3 or not c["controls_trusted"]:
            return
        opcode, payload = b[2], b[2:]
        # ENC_REQ/RSP is still plaintext; stop parsing before encrypted controls.
        if (opcode == 3 and len(payload) == 23) or (opcode == 4 and len(payload) == 13) or (opcode in (5, 6) and len(payload) == 1):
            c["encrypted"] = True
            c["controls_trusted"] = False
            c["warning"] = "Encryption procedure observed; later control updates are unreadable"
            self._mark(c, point, kind="encryption", label="Encryption starting")
            # Future encrypted interval/map updates would invalidate predictions.
            c["timing_valid"] = False
        elif opcode == 2 and len(payload) == 2:
            c["ended"] = True
            c["state"] = "Terminate observed"
            self._mark(c, point, kind="terminate", label=f"LL_TERMINATE_IND · reason 0x{b[3]:02x}")
        elif opcode in (0, 1) and len(payload) == (12 if opcode == 0 else 8):
            if event is None:
                self._mark(c, point, kind="uncertain", label="Control update seen; instant cannot be located")
                return
            instant = int.from_bytes(b[-2:], "little")
            delta = (instant - event) & 0xffff
            absolute = event + delta
            # Same control PDU can be retransmitted, including after its instant.
            key = (payload.hex(), absolute)
            if key in c["seen_controls"] or any(k[0] == payload.hex() and abs(k[1] - event) < 32768 for k in c["seen_controls"]):
                return
            if delta >= 32768:
                self._mark(c, point, kind="uncertain", label="Control instant is in the past or ambiguous")
                return
            if opcode == 1:
                try:
                    used = channel_map(b[3:8])
                except ValueError:
                    return
                if len(c["maps"]) >= 128:
                    # Retain the current map and all future updates.
                    c["maps"] = [m for m in c["maps"] if m["event"] >= event] + [dict(event=event, used=self._used(c, event))]
                c["maps"].append(dict(event=absolute, used=used))
                c["maps"].sort(key=lambda m: m["event"])
                c["map_updates"] += 1
                self._mark(c, point, kind="map", label=f"Channel map → {len(used)} channels at event {instant}",
                             instant=instant, instant_index=absolute, used=used)
            else:
                window = b[3]
                offset = int.from_bytes(b[4:6], "little")
                interval = int.from_bytes(b[6:8], "little")
                latency = int.from_bytes(b[8:10], "little")
                timeout = int.from_bytes(b[10:12], "little")
                if not (6 <= interval <= 3200 and 0 <= latency <= 499 and
                        10 <= timeout <= 3200 and timeout * 8 > 2 * (latency + 1) * interval and
                        1 <= window <= min(8, interval - 1) and offset <= interval):
                    return
                if c["pending_timing"]:
                    c["timing_valid"] = False
                    c["warning"] = "Conflicting connection interval updates"
                    return
                c["pending_timing"] = dict(event=absolute, interval_ms=interval * 1.25,
                                           offset_s=offset * .00125, window_s=window * .00125,
                                           latency=latency, timeout_ms=timeout * 10)
                self._mark(c, point, kind="timing", label=f"Interval → {interval * 1.25} ms at event {instant}")
            c["seen_controls"].append(key)

    def snapshot(self, after):
        rows = []
        for c in reversed(self.connections.values()):
            row = {k: c[k] for k in ("id", "access_address", "crc_init", "observed", "start", "last", "central", "peripheral", "interval_ms", "timeout_ms", "latency", "hop", "algorithms", "used", "state", "ended", "encrypted", "warning", "last_event", "map_updates", "count")}
            row["channels"] = sorted(c["channels"])
            row["quiet"] = self.now - c["last"] > c["timeout_ms"] / 1000
            row["events"] = [e for e in c["events"] if e["id"] > after]
            row["markers"] = [e for e in c["markers"] if e["id"] > after]
            row["activity"] = [{**b, "channels": dict(b["channels"])} for b in c["activity"] if b["id"] > after]
            row["history_truncated"] = len(c["events"]) == MAX_POINTS
            row["pending_maps"] = [m for m in c["maps"] if m["event"] > (c["last_event"] or 0)]
            rows.append(row)
        return dict(connections=rows, capture_s=self.now, gaps=[dict(g) for g in self.gaps])
