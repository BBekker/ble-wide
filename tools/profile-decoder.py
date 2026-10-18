#!/usr/bin/env python3
"""Train LLVM's decoder optimization profile using local I/Q recordings.

Run through tools/run on macOS. Supply representative training captures;
reserve different recordings for performance and packet-recovery validation.
The profile contains compiler counters, not decoded packet payloads.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DECODER = ROOT


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("captures", type=Path, nargs="+")
    parser.add_argument("--format", choices=["iq8", "iq4", "raw14", "iq16"], default="iq8")
    parser.add_argument("--backend", choices=["auto", "metal", "cpu"], default="auto")
    parser.add_argument("--repeat", type=int, default=3)
    args = parser.parse_args()
    if args.repeat < 1:
        parser.error("--repeat must be positive")
    captures = [path.resolve(strict=True) for path in args.captures]
    if any(not path.is_file() or path.stat().st_size == 0 for path in captures):
        parser.error("captures must be nonempty files")
    compiler = os.environ.get("CXX", "clang++")
    profdata = shutil.which("llvm-profdata")
    if not profdata:
        parser.error("llvm-profdata is missing; use tools/run on macOS")
    compiler_version = subprocess.check_output([compiler, "--version"], text=True)
    if "clang" not in compiler_version.lower():
        parser.error("profile-guided builds require Clang (set CXX=clang++)")
    build = DECODER / "build"
    build.mkdir(exist_ok=True)
    (build / "w1700k-ble-wide-instrumented").unlink(missing_ok=True)
    subprocess.run(["make", "-C", str(DECODER), f"CXX={compiler}", "PROFILE=",
                    "build/w1700k-ble-wide-instrumented"], check=True)
    records = []
    with tempfile.TemporaryDirectory(prefix="profile-", dir=build) as directory:
        directory = Path(directory)
        profiles = []
        for index, capture in enumerate(captures):
            profile = directory / f"capture-{index}.profraw"
            profiles.append(profile)
            command = [str(build / "w1700k-ble-wide-instrumented"), "--file", str(capture),
                       "--demod", "streaming", "--backend", args.backend,
                       "--decode-threads", "4", "--repeat", str(args.repeat)]
            if args.format != "iq16":
                command.append("--" + args.format)
            subprocess.run(command, env={**os.environ, "LLVM_PROFILE_FILE": str(profile)},
                           stdout=subprocess.DEVNULL, check=True)
            with capture.open("rb") as source:
                digest = hashlib.file_digest(source, "sha256").hexdigest()
            records.append({"path": str(capture), "bytes": capture.stat().st_size,
                            "sha256": digest})
            print(f"Profiled {capture.name}", flush=True)
        merged = directory / "decoder.profdata"
        subprocess.run([profdata, "merge", *map(str, profiles), "-o", str(merged)], check=True)
        merged.replace(build / "decoder.profdata")
    metadata = {"compiler": compiler_version, "format": args.format, "backend": args.backend,
                "repeat": args.repeat, "captures": records}
    (build / "decoder-profile.json").write_text(json.dumps(metadata, indent=2) + "\n")
    subprocess.run(["make", "-C", str(DECODER), f"CXX={compiler}", "all"], check=True)
    print("Built the streaming receiver with the local profile. Validate on held-out captures.")


if __name__ == "__main__":
    main()
