#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read-only CPU/network measurement, including work outside the receiver process.

CPU percentages use 100% = one logical core. System CPU includes kernel time
already charged to processes: do not add the system and process columns.
Unrelated desktop activity also contributes to system totals. Compare repeated
equal-traffic runs and a stopped-capture baseline before attributing costs.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.request


def process_times():
    names = {"w1700k-stream", "w1700k-ble-wide", "com.realtek.dext.RTL815xEthernet"}
    result = {}
    output = subprocess.check_output(["ps", "-axo", "pid=,time=,comm="], text=True)
    for line in output.splitlines():
        pid, duration, path = line.strip().split(None, 2)
        name = Path(path).name
        if name in names:
            seconds = sum(float(v) * 60**k for k, v in enumerate(reversed(duration.split(":"))))
            result[int(pid)] = {"name": name, "cpu_s": seconds}
    return result


def network_counts(interface):
    output = subprocess.check_output(["netstat", "-ibn", "-I", interface], text=True)
    row = next(line.split() for line in output.splitlines() if "<Link#" in line)
    return dict(packets=int(row[4]), errors=int(row[5]), bytes=int(row[6]))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--interface", default="en10")
    parser.add_argument("--viewer", default="http://127.0.0.1:8076/api")
    parser.add_argument("--no-viewer", action="store_true", help="measure with capture stopped")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if sys.platform != "darwin" or args.seconds <= 0:
        parser.error("requires macOS and a positive measurement duration")

    lib = ctypes.CDLL(None)
    lib.mach_host_self.restype = ctypes.c_uint
    lib.host_statistics.argtypes = [ctypes.c_uint, ctypes.c_int,
                                   ctypes.POINTER(ctypes.c_uint), ctypes.POINTER(ctypes.c_uint)]
    host = lib.mach_host_self()

    def ticks():
        # mach/host_info.h: HOST_CPU_LOAD_INFO = 3; four natural_t tick counters.
        values = (ctypes.c_uint * 4)()
        count = ctypes.c_uint(4)
        if lib.host_statistics(host, 3, values, ctypes.byref(count)) != 0:
            raise RuntimeError("host_statistics failed")
        return list(values)

    def snapshot():
        status = {}
        if not args.no_viewer:
            with urllib.request.urlopen(args.viewer, timeout=5) as response:
                status = json.load(response)["status"]
        return dict(processes=process_times(), ticks=ticks(), network=network_counts(args.interface),
                    status=status, time=time.monotonic())

    start = snapshot()
    time.sleep(args.seconds)
    end = snapshot()
    elapsed = end["time"] - start["time"]
    delta_ticks = [(b - a) % (1 << 32) for a, b in zip(start["ticks"], end["ticks"])]
    common = start["processes"].keys() & end["processes"].keys()
    result = {
        "wall_s": elapsed,
        "percent_units": "100% = one logical core; system and process columns overlap",
        "system_cpu_percent": {name: (os.cpu_count() or 1) * 100 * value / sum(delta_ticks)
                               for name, value in zip(("user", "system", "idle", "nice"), delta_ticks)},
        "process_cpu_percent": [dict(pid=pid, name=end["processes"][pid]["name"],
                                     percent=100 * (end["processes"][pid]["cpu_s"] -
                                                    start["processes"][pid]["cpu_s"]) / elapsed)
                                for pid in sorted(common)],
        "processes_changed": sorted(start["processes"].keys() ^ end["processes"].keys()),
        "network_per_second": {k: (end["network"][k] - start["network"][k]) / elapsed
                               for k in start["network"]},
        "receiver_delta": {k: end["status"][k] - start["status"][k]
                           for k in ("samples", "packets", "lost_samples", "resets", "decode_cpu_s", "gpu_s")
                           if k in start["status"] and k in end["status"]},
        "receiver_final": {k: end["status"][k]
                           for k in ("source_alive", "lag_ms", "backend", "demodulator") if k in end["status"]},
    }
    output = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(output)
    print(output, end="")


if __name__ == "__main__":
    main()
