# BLE Wide

Wideband Bluetooth LE 1M reception across all 40 channels, with a live browser viewer for advertisements, connections, packet contents, CRC failures, signal/noise measurements, and a waterfall.

This repository contains the complete computer-side application:

- `receiver/`: UDP reception, ordering, loss accounting, sample conversion, and a shared-memory IQ ring (`w1700k-stream`).
- C++/Metal sources at the root: channelization and streaming packet decoding (`w1700k-ble-wide`). Metal runs the channelizer on macOS; CPU workers decode independent channels.
- `server.py`, `connections.py`, and `web/`: the viewer, connection tracking, and browser-side protocol decoding.
- `tools/` and `tests/`: replay benchmarks, local optimization profiles, and regression tests.

Router firmware, the radio exporter, installation, and hardware research remain in [w1700k-sdr](https://github.com/BBekker/w1700k-sdr). Building, replaying recordings, and receiving an already-running UDP export do not require that checkout.

## Build and test

Requires a C++17 compiler, FFTW single precision, pkg-config, make, and Python 3.10 or newer. Node.js is needed for the browser regression tests. Profile training additionally requires Python 3.11 or newer and LLVM.

On macOS, install dependencies with Homebrew:

```sh
brew install fftw pkg-config llvm node
./tools/run make -j4
./tools/run make check
```

`tools/run` selects available Homebrew tools for that command without changing your shell settings. On Linux, install the corresponding FFTW development package and build tools, then use `make` and `make check`. Linux builds use the CPU channelizer; macOS builds also support Metal and Accelerate. Linux portability is retained, but this extraction was validated on macOS.

Build outputs and locally generated compiler profiles are ignored by Git. A regular build needs no profile. Use `make PROFILE=` to disable a local profile.

## Live reception

With the router already exporting to this computer, start the viewer:

```sh
python3 server.py --bits 8 --backend metal --decode-threads 4
```

Open [the viewer](http://127.0.0.1:8076/). The viewer starts and stops its sample receiver and decoder. On macOS it detects the adapter owning `--bind` (default `10.77.0.10`) and prefers direct USB for the tested RTL8157 revision `0x3000` in vendor configuration 1. Detection does not require privileges. USB capture uses one macOS administrator authorization per viewer run, with no installed service or security-setting changes. Receiver details show the selected transport and startup/fallback reason. Unsupported hardware uses UDP; a failed or declined USB attempt falls back to UDP without prompting again.

Use `--transport udp` to force ordinary networking or `--transport usb` to require USB and report an error instead of falling back. `--stream /name` explicitly attaches to a receiver managed elsewhere and never claims its adapter. Direct USB temporarily removes normal IP networking on that adapter, so use the updated router launcher below or an independently running exporter.

Use `--backend cpu` on Linux. Streaming decoding is the default. The example matches the current 160 Msps, 8-bit export over two UDP ports with jumbo frames; the router exporter and host network must already be configured accordingly. `--bits` describes the source stream to the viewer; the receiver detects the wire format and publishes normalized int16 IQ. It does not configure the router.

For coordinated router setup/start/stop, run the hardware launcher from the separate SDR checkout:

```sh
tools/macos/run python3 tools/iq4/start-mac.py --ble-repo ../ble-wide \
  --interface eth2 --bits 8 --blocks 2 --ports 2 --ble \
  --backend metal --decode-threads 4 --reuse-radio --seconds 86400
```

The router launcher accepts `--ble-repo`, or `W1700K_BLE_REPO`. Its default is a sibling `ble-wide` checkout. Credentials and router configuration stay in the SDR checkout.

## Replay and development

```sh
./build/w1700k-ble-wide --file /path/capture.raw --iq8 --demod streaming --backend metal --decode-threads 4 > /tmp/events.jsonl
python3 server.py --events /tmp/events.jsonl
```

For replay, `--iq8` and `--iq4` simulate quantization of recordings containing native 32-bit radio words; they do not mean packed on-wire bytes. `--raw14` reads those words without quantization. With none of those flags, input is interleaved int16 IQ. Supply capture paths explicitly to the tools; recordings are not shipped.

The shared-memory ABI is in [`include/w1700k_stream.h`](include/w1700k_stream.h); receiver details are in [`receiver/README.md`](receiver/README.md). The two executables share the IQ ring, avoiding packet/network handling in decoder workers. Both belong to this application.

Historical [research notes and measurements](docs/research/README.md) retain the paths and observations from their original checkout. They are provenance, not current launch instructions. `browser/` preserves an incomplete browser/WASM experiment and is not part of the supported viewer or default build.

Local recordings and older build experiments may be kept under ignored `local-data/`; none are uploaded. See [`NOTICE`](NOTICE) for source provenance and licensing.

The [direct RTL8157 USB helper](receiver/usb/README.md) is built automatically when compatible hardware is detected. Its hardware support is narrow, and the normal UDP path remains available on every supported platform. Shutdown logs are retained in `build/live-receiver/`.
