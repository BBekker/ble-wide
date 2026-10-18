# Working on BLE Wide

## Project layout

```text
airspace/               Python application package
  server.py             HTTP/SSE viewer service and CLI
  live_receiver.py      Receiver/helper lifecycle and transport selection
  connections.py        Connection tracking
  advertisements.py     Advertising data interpretation
  paths.py              Shared repository/resource paths
src/                    Native decoder application
  main.cpp              Decoder executable entry point
  channelizer/          CPU and Metal channelizers, GPU shader
  decoder/              Synchronization, demodulation, packet/noise processing
  pipeline/             Worker scheduling, capture clock, waterfall power
receiver/               Sample receiver and hardware transport
  src/                  UDP/USB ingest, ordering, shared-memory publication
  usb/                  macOS helper, RTL8157 initialization, framing and IPC
include/                Shared stream/wire interfaces and platform headers
web/                    Browser UI and client-side protocol decoding
mk/                     Shared build configuration
tests/
  cpp/                  Native decoder tests and synthetic signal fixtures
    usb/                USB framing/ring and PHY initialization tests
  python/               Viewer, connection, receiver and integration tests
  web/                  Browser-model tests, runnable with Node.js
tools/
  benchmarks/           Replay comparisons and host measurements
  usb/                  Temporary hardware-test session tooling and table import
  run                   Local build-tool environment selection
  profile-decoder.py    Optional profile-guided optimization training
  usb-session           Temporary USB research-session entry point
docs/
  assets/               Project artwork
  benchmarks/           Transport measurements and hardware validation results
  research/             Historical decoder experiments and provenance
experiments/browser/    Incomplete WASM/browser receiver prototype
```

Application code belongs in `airspace/`, not the root launch shims. Run the viewer with `python3 -m airspace` from the checkout. `python3 server.py` and `from live_receiver import detect_adapter` remain available for existing router launchers. Executables stay at `build/w1700k-ble-wide` and `receiver/build/w1700k-stream`, so the router exporter does not depend on the internal source layout.

Native private headers use component-qualified includes such as `decoder/streaming-demod.h`; public/shared interfaces live in `include/`. Keep device access in the receiver/helper and avoid introducing privileged code into the Python or web layers.

## Build and test

```sh
./tools/run make -j4              # decoder and sample receiver
./tools/run make check            # complete default suite
./tools/run make check-cpp        # decoder, channelizer and pipeline tests
./tools/run make check-python     # Python and receiver integration tests
./tools/run make check-web        # browser models; no browser required
./tools/run make check-usb        # framing/ring and simulated initialization
./tools/run make -C receiver usb-helper  # build/sign macOS hardware helper
```

Focused targets such as `check-streaming`, `check-metal` (macOS), and `check-packet-noise` remain available. USB tests in the default suite use synthetic data and simulated register operations; they do not take over an Ethernet adapter or request administrator privileges.

`make clean` removes compiled products while retaining local profiles, captures, and diagnostic logs. Profiles are specific to source/compiler builds; regenerate local profiles after substantial source moves, or use `PROFILE=` to verify an unprofiled build.

The experimental browser receiver is outside the default build and test suite. Its Emscripten and Node dependencies are separate from the supported Airspace web UI.

## Measurements and contributions

Keep reusable benchmarks in `tools/benchmarks/` and publish summarized, reproducible results in `docs/benchmarks/`. Existing files in `docs/research/` retain historical paths and observations; do not rewrite those as if they were measurements of current code. Record the hardware, source format, revision, duration, packet-quality results, and sample-loss counters alongside performance numbers.

Local builds, captures, compiler profiles, and hardware-session logs are ignored. Do not include credentials, private recordings, or temporary generated artifacts in commits. Preserve SPDX notices and update `NOTICE` when importing source or firmware tables.
