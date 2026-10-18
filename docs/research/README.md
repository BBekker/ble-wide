> Historical notes from the router SDR working tree. Paths and commands below describe that layout; use the [current README](../../README.md) for this standalone application.

# All-channel LE 1M receiver

Decode all 40 BLE frequencies simultaneously on the Mac from the W1700K's
160 Msps IQ4 export. The local website is at `http://127.0.0.1:8076/`.
Live reception has produced CRC-valid advertisements on every channel,
including extended advertisements on secondary channels. This is not a claim
that every transmitted packet is recovered.

The streaming decoder is the default in the receiver, web server, and Mac launcher.
Legacy/adaptive modes remain explicit comparison options.

### Further vector optimization (2026-10-17)

Synchronization now keeps all timing-fit accumulators in NEON registers and
compares four synchronized address bits per vector. The background and noise-floor
filters update together in a double-precision vector, preserving their sequential
sample updates. The display-only noise measurement reduces its 64-sample window
at packet acquisition instead of maintaining a running sum on every sample.
That reduction changes summation order, but not the samples included or the
filters used for acquisition thresholds. Phase calculation remains lazy and each
payload is decoded once.

Against the previously deployed profile-guided build, three alternating trials
on each of four held-out recordings measured **12.5–15.1% less decoder CPU time**
and **11.4–14.9% more replay capacity**. Both builds used Metal, four workers and
IQ8; live capture was paused for the comparison. All 1,434 packet records across
40 captures matched, including CRC failures and printed signal/noise/CFO values.
The saved 49- and 64-packet connection datasets also matched. The deployed build
received 5,320 packets during a 30-second check with no lost samples or new resets.

Prepared-history, raw-address batching and payload-metric prototypes did not
improve total decoder performance and were excluded. The scalar preamble search
remains in place. Measurements, retained changes and validation are recorded in
[streaming-search-vector-results.json](streaming-search-vector-results.json).

### Profile-guided builds

The local receiver can use LLVM execution profiles to optimize branch layout and
inlining without changing decoder arithmetic, thresholds or packet coverage.
Train on representative recordings, then benchmark and check packet recovery on
different recordings:

```sh
tools/macos/run python3 tools/ble/profile-decoder.py --backend metal captures/train-*.raw
```

The default input format is packed radio words quantized to IQ8 during replay;
`--format iq16` instead accepts interleaved int16 I/Q. Training stores compiler
counters in `build/decoder.profdata` and rebuilds the receiver. Ordinary builds
use that local profile when present; builds without it work normally. Regenerate
the profile after decoder changes. Use `make -C host/ble-wide -B PROFILE=` for
an unprofiled comparison and `make -C host/ble-wide -B` to restore profiling.
The training process records input hashes and compiler version locally. It does
not modify recordings or package packet payloads into the profile.

The follow-up tuning experiments and held-out results are recorded in
[streaming-tuning-results.json](streaming-tuning-results.json). Changes that
increased CPU work or lacked a consistent gain were excluded from the final build.
With live capture paused, three paired trials on each of two held-out recordings
measured **3.0–3.3% less decoder CPU time** and **2.4–2.6% more replay capacity**.
All 895 valid and 539 CRC-failed packet records matched across 40 captures,
including 36 not used for training; both saved connection datasets also matched.

### Accelerate and ARM vector processing (2026-10-17)

On ARM64, the streaming worker prepares differential I/Q products and
double-precision sample power with four-sample NEON vectors in 64-sample blocks.
Synchronization fits two timing phases at once using double-precision vector
fused multiply-adds. The nine timing hypotheses, sample order within each fit,
thresholds and single payload decode are unchanged. Phase discrimination remains
lazy; the vector preparation does not compute angles for every idle sample.
Scalar sample processing remains available for interleaved input and the
single-worker reference path.

On macOS, the planar waterfall uses Accelerate's `vDSP_svesq` to sum I/Q power.
Only display power uses that float reduction; packet signal/noise measurements
retain their double-precision history. The waterfall test allows less than
0.0001 dB difference from the scalar result, below its 0.1 dB output precision.
Other platforms retain portable arithmetic. Metal still performs channelization.

Against the preceding optimized streaming receiver, three alternating trials
on the same M3 Pro replay measured median decoder CPU time **2.732 → 2.359 s**
(14% less) and processing capacity **270.4 → 335.3 Msps** (24% higher).
The live receiver ran concurrently; these are replay measurements with competing
load, not isolated whole-system CPU or battery measurements. GPU code is unchanged.

All 895 valid and 539 CRC-failed packet records match across 40 saved captures.
The two saved connection datasets also match, at 49 and 64 packets. Arithmetic,
scalar/batch boundaries, resets, CRC failures, packet measurements, all-channel
CPU/Metal and existing sensitivity/reply tests pass. Build imports and disassembly
confirm the Accelerate call and NEON instructions. See
[streaming-vector-results.json](streaming-vector-results.json) for measurements.

```sh
tools/macos/run make -C host/ble-wide all check-streaming-vector check-waterfall
```

### Streaming efficiency (2026-10-17)

The pipeline reuses up to six input/output jobs after their decoder callback
finishes. Metal writes each channel's contiguous output directly, removing the
transpose kernel and its scratch buffer. Synchronization reuses precomputed
pulse shapes and observed sample moments; identical detector words are checked
once. Exact and one-bit-neighbor address resolution shares a single lookup.
Packet thresholds, timing search coverage, and payload decoding are unchanged.

Three alternating before/after replay trials on the M3 Pro, with Metal and four
decoder workers, measured these medians for the same 335,544,320 input samples:

| Measurement | Before | After |
| --- | ---: | ---: |
| Processing capacity | 229.4 Msps | 282.6 Msps |
| Decoder CPU time | 2.975 s | 2.694 s |
| Channelizer CPU time | 0.191 s | 0.102 s |
| GPU time | 0.533 s | 0.522 s |

The live receiver was running during these trials, so these are short replay
measurements with competing load, not isolated battery or whole-system CPU
measurements. GPU timing varied; its small measured change is not strong evidence
of a GPU speedup. Both versions recovered the same 895 valid and 539 CRC-failed
packets across 40 saved captures, matching packet bytes, channel, sample and CRC.
The two saved connection datasets also match exactly: 49 and 64 packets.
Metal layout equivalence, CPU/Metal comparison, synthetic reception, reply
synchronization, packet-noise, parallel worker and address-lookup checks pass.
Results are in [streaming-efficiency-results.json](streaming-efficiency-results.json).

## Run

From the repository root, with the existing Mac receiver configuration and
normal router firmware installed:

```sh
tools/macos/run make -C host/stream
tools/macos/run make -C host/ble-wide
make -C tools/iq4 build/iq4-send
tools/macos/run python3 tools/iq4/start-mac.py --interface eth2 --ble --seconds 86400
```

This router's currently connected 2.5 GbE port is `eth2` (LAN2), configured at
10.77.0.1/24 with MTU 9216. The Mac is 10.77.0.10/24 with MTU 9000.
`--interface eth1` selects the WAN port for the link check instead. The option
checks the interface; it does not move IP addresses or change router routing.
These network changes are runtime configuration and need restoring after a
router reboot. Bringing eth2 up initially took a long time but eventually
completed; a reboot was not required.

The launcher stops the old BLE capture service, loads normal radio firmware,
and starts the sender, Mac stream receiver, decoder, and website. Use
`--reuse-radio` only when normal firmware is already loaded and capture is
stopped. The current launcher relies on the existing `/work/ble/config.json`
and service bundle; a router reboot may require restoring that RAM bundle.
The router capture runs independently of SSH, so a brief control-connection
disconnect does not terminate it. The launcher polls its log and exit status;
it tolerates up to one minute without contact. Network send failures are
counted as gaps. Ctrl-C stops capture and the local children. Duration is bounded to 1–86400
seconds; this is not a boot service. The old router-side BLE viewer is not
restarted automatically.

The website keeps the latest 10,000 packets in memory and renders the latest
200 matching packets. It offers frequency selection, advertisement/data
selection, text search, raw PDU/CRC inspection, and a display pause. Pausing
only pauses the packet list; reception continues. Operational logs are in
`tools/iq4/build/`; normal live operation does not persist advertisement payloads.

## Processing

1. The radio captures complex 160 Msps centered at 2441 MHz, with the 160 MHz
   analog filter. Two router CPU workers pack signed four-bit I and Q with
   one scale per 2048-sample block. They run on CPUs 1 and 2, leaving CPU 0
   available for network interrupts and CPU 3 for the capture pump.
2. IQ41 UDP carries 1.28 Gbit/s of sample payload over the 2.5 GbE link.
   `w1700k-stream` restores signed int16 samples in its shared ring, preserving
   completion numbers and marking missing blocks. On macOS the UDP thread
   has interactive scheduling priority so decoding does not starve reception.
3. `channelizer.h` translates by −1 MHz and applies an 80-way polyphase
   filter bank: a 1280-tap Blackman-windowed sinc prototype, 0.65 MHz cutoff,
   16 taps per phase, and an 80-point FFT. All BLE centers land on 2 MHz
   bins, each producing 2 Msps complex samples. The unused bins are discarded.
4. With the CPU channelizer, three Mac workers process successive 163,840-sample chunks. Each receives
   the preceding 1280 samples as FIR history; warmup output is discarded.
   Results are consumed in capture order. This retains filter and decoder
   continuity across worker boundaries without sending raw samples to a DSP.
5. Packet decoding uses a separate pool of four CPU workers by default. Each
   channel has its own ordered sample history and decoder state. The adaptive
   and single-pass demodulators are described below.

The queue is bounded to six chunks. A lost shared-ring block flushes preceding
work and resets filter/demodulator history; it is never silently joined to
subsequent samples. A new radio run also clears connection contexts. Attaching
to an existing live stream starts at its current frontier, not old backlog.

## CRC context and coverage

Advertising packets use the standard advertising access address and CRC seed.
CONNECT_IND / AUX_CONNECT_REQ packets teach the connection access address and
CRCInit for subsequent LE 1M data packets. For an unknown access address, the
receiver reverses the CRC to infer a seed and requires three separate packets
with the same seed before showing it. The website labels these as **inferred**,
separately from an observed connection setup. One candidate alone is not
independent CRC evidence. Known-context packets must pass their CRC.

Implemented: primary advertisements and scan responses, LE 1M extended
advertising PDUs on secondary channels, ordinary LE 1M data PDUs with observed
or inferred CRC context. The first two inferred candidates are not displayed.

Not implemented: LE 2M, LE Coded, direction-finding CTE headers, isochronous
packet formats, extended-advertising chain reassembly, SyncInfo context
learning, connection event/timing reconstruction, or payload decryption.
Extended PDUs are displayed individually as raw bytes. Noise, collisions,
frequency error, four-bit quantization, and input gaps can all prevent recovery.
The packet `sample` is the decoder's surviving-sample counter at 2 Msps, not
an absolute radio timestamp; the website time is arrival at the viewer.

## Validation and limitations

```sh
tools/macos/run python3 -m unittest discover -s tests -p test_ble_wide.py
tools/macos/run python3 -m unittest discover -s tests -p test_stream.py
tools/macos/run python3 -m unittest discover -s tests -p test_iq4.py
```

The wideband test places 40 simultaneous modulated packets on all BLE centers,
straddling a parallel-worker boundary, and recovers all 40 through both raw14
and IQ4 paths. It rewhitens the published BLE test vector for every channel to
exercise frequency mapping and whitening; it is a synthetic channelizer test,
not an assertion that legacy primary PDUs are transmitted on secondary channels.
Decoder tests cover CRC inversion, invalid-CRC rejection, three-packet seed
confirmation, duplicate timing hypotheses, reset, and connection-setup learning.

Six identical saved 160 Msps captures yielded 142 CRC-valid IQ4 packets with
the 16-tap-per-phase filter versus 128 with an eight-tap version. The stronger
filter is retained. This is a short ambient comparison, not a sensitivity test.

Live aggregate measurements are in `results.json`; local raw captures and
packet logs remain under ignored `build/` directories. The Mac initially had
UDP socket-buffer drops under load. Moving the router packers away from CPU 0
removed their throughput deficit, and prioritizing Mac reception improved
steady operation, but loss-free reception under arbitrary Mac load is not
proven. The website shows known missing input as sample time, separately from
backlog. Filter settling after a gap can also lose packets. macOS lacks the
Linux per-socket `SO_RXQ_OVFL` counter, so `socket_drops: 0` in the common stream
report does **not** establish no socket drops; use sequence gaps and the change
in `netstat -s -p udp` full-buffer drops.

Protocol reference: [Bluetooth Core, Vol 6, Part B](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/low-energy-controller/link-layer-specification.html),
packet formats and sections 3.1–3.2 for CRC and whitening.

`processing_s` measures foreground processing/wait time, while filter workers
can also run during input waits. The derived `capacity_msps` is diagnostic,
not a guaranteed sustained-throughput benchmark. Live sequence loss and
backlog are the end-to-end checks.

## Mac GPU channelizer

On macOS the native build includes a Metal backend. The web server and
`tools/iq4/start-mac.py --ble` default to `--backend auto`: try Metal, with a
logged CPU fallback if device or shader initialization fails. Use
`--backend metal` to require the GPU, or `--backend cpu` for comparison. The
standalone decoder retains CPU as its default. The website reports the actual
backend from decoder status, rather than the requested setting.

The GPU performs the 1 MHz frequency translation, all 1280 FIR taps (80 phases
by 16 taps), and the positive-sign 80-point channel transform. The transform
factors into 5-point and 16-point stages and emits only the 40 BLE bins.
Demodulation, access-address detection, CRC/context handling and web serving
remain on the CPU. The radio still streams 160 Msps IQ81 over two UDP ports.

Each GPU job contains 655360 new samples (4.096 ms) plus 1280 samples of FIR
history. Chunks align with the 160-sample oscillator period. A single worker
reuses shared Metal input/output buffers and waits for command completion
before reusing or reading them. A bounded queue overlaps GPU channelization
with CPU packet decoding and consumes jobs in sample order. This adds up to
roughly 25 ms of normal batch buffering; network loss still resets both FIR
and packet state. There are explicit CPU copies into and out of shared GPU
buffers; this does not claim zero-copy reception. Output is channel-contiguous
by default, as described below.

The shader source is embedded at build time and compiled by Metal once at
startup, using the system runtime. Xcode's offline `metal` compiler is not
required. CPU-only builds on other operating systems do not link Metal.

```sh
tools/macos/run make -C host/ble-wide
tools/macos/run make -C host/ble-wide check-metal
tools/macos/run python3 -m unittest discover -s tests -p test_ble_wide.py
tools/macos/run python3 tools/iq4/start-mac.py --interface eth2 --bits 8 --ble --backend metal --reuse-radio --seconds 86400
```

`filter_cpu_s` measures worker CPU time (including buffer copies/submission),
`decode_cpu_s` sums CPU time across all decoder workers (including output on
the calling thread), `decode_wall_s` measures elapsed batch decoding/output
time, `decode_threads` reports the configured worker count, and
`gpu_s` sums Metal command GPU-end minus GPU-start timestamps. The last value
is elapsed command execution time, not a device-wide utilization or power
measurement. Process CPU measurements include additional scheduling, input,
serialization and other overhead beyond the two stage counters.

`test-metal.cpp` compares CPU/GPU complex output for noise, impulses, silence,
partial chunks and reused buffers. The all-channel test additionally verifies
identical packet start positions and payloads across a GPU chunk boundary.
Private live recordings used for comparison remain in ignored build output.

### Measurements on this Mac (2026-10-15)

Over sequential live intervals, BLE-process CPU fell from **259.3%** to
**65.9%** (100% = one core). The UDP receiver fell from 62.6% to 52.8%, so
those two processes together used **63.1% less CPU time per second**.
The 60-second GPU interval processed about 160 Msps with zero reported
missing input and no backlog at its endpoint. This is a bounded observation,
not a guarantee of lossless reception or lower power consumption.

A separate paired one-second recording yielded exactly the same **243**
packets, including channel, start position, access address and PDU, through
CPU and Metal. FIR/transform numerical tests differed by at most 0.0000916
in component amplitude on the radio's signed-14-bit scale. Full aggregate
measurements and caveats are in [metal-results.json](metal-results.json).

Metal runtime compilation uses Apple's
[newLibraryWithSource API](https://developer.apple.com/documentation/metal/mtldevice/makelibrary(source:options:)?language=objc).

## Connection timelines

The viewer now keeps one timeline row per connection. A primary-channel
`CONNECT_IND` supplies the two device addresses, data access address, CRC seed,
connection interval, latency, supervision timeout, hop increment and channel
map. An access address seen without its setup gets a separate “midstream” row;
it does not get invented device addresses or a connection start event.

`connections.py` associates decoded packets across all 37 data channels using
access address and CRC seed. It implements CSA #1 and CSA #2 and locates
connection events by intersecting the initial transmit window, elapsed capture
time and observed channel. Event numbers are labeled **inferred**: BLE data
PDUs do not carry them. When the advertiser's ChSel bit was not observed, both
algorithms remain candidates until channel observations distinguish them.

Plaintext `LL_CHANNEL_MAP_IND` changes are queued for their 16-bit instant,
with wraparound and retransmission handling. The new map is used starting at
that connection event; CSA #1's unmapped-channel progression is not reset.
The timeline distinguishes the update's receipt from the first captured packet
using its new map. Selecting a row shows the last tracked map and pending maps.
All frequencies are already decoded concurrently; this tracking does not retune
the radio or discard packets that disagree with an inferred schedule.

The native decoder adds `capture_s` and `epoch` fields plus `capture_start` and
`gap` events. Its capture clock includes lost raw samples and partial PFB frames
discarded during resets. `sample` retains its original decoder-counter meaning.
The common FIR/demodulation delay is not an absolute RF time calibration; it
cancels for interval comparisons. A firmware event has unknown RF duration and
invalidates timing rather than pretending a known duration. Old JSONL without
this clock can still display traffic, but does not enable event inference.

Limits are explicit in the UI:

- Once an encryption procedure is observed, later control PDUs are not parsed.
  Packet association and channel observations continue; encrypted map changes
  require keys and are not recovered here. Midstream payloads are never assumed
  to be plaintext control messages.
- An input gap invalidates control/timing confidence because an interval,
  encryption or map update could have been missed. Captured packets still appear.
- Plaintext connection-interval updates use the old interval up to their instant,
  then the announced new transmit window and interval. Anchor timing remains an
  interval of possibilities until captured packets constrain it. Conflicting or
  ambiguous updates are flagged instead of choosing an event number arbitrarily.
  AUX_CONNECT_REQ, LE 2M and Coded PHY are not tracked as primary LE 1M setups.
- Quiet means no recent packets were captured, not that termination was proven.
  `LL_TERMINATE_IND`, a new setup reusing an access address and a capture restart
  are shown separately.

The redesigned viewer gives the connection timelines and FFT waterfall the main
workspace, with a shared time window/pause control and a side inspector. Selecting
a connection highlights its packets in the waterfall. The packet log is collapsed
by default and uses virtual scrolling when opened.

Selecting a connection also opens its packet view, including its retained setup
packet when available. Consecutive empty data PDUs (LLID 1, zero payload bytes)
are collapsed into expandable runs; control/data packets, capture gaps and epoch
changes break runs. Expanded groups retain every individual header and signal
reading. Newest packets appear first, with optional follow mode; inspecting a
packet stops scrolling while capture continues. Click any packet for its complete
payload and PDU, CRC, SN/NESN/MD flags and capture timestamp. Encrypted bytes are
displayed as ciphertext. History remains bounded by the shared 50,000-packet ring;
the view reports how many earlier connection packets have expired.

The native decoder emits `signal_dbfs` for every decoded packet. It averages
channel-filtered I²+Q² across every 2 Msps sample of that packet's PDU and CRC,
using a cumulative energy difference rather than another per-packet sample loop.
Normalization matches the waterfall's 8192-amplitude reference. The UI labels
this relative RSSI/signal measurement in dBFS, not antenna-calibrated dBm. Empty
group rows show their signal range; expansion shows each reading separately.

`web/packet-model.js` also decodes data-channel PDUs entirely in the browser.
Packet rows show protocol/message summaries alongside raw hex; opening a packet
shows LL header fields, supported LL control parameters, L2CAP length/CID, and
ATT fields (MTU, errors, handle/UUID discovery, reads, writes, prepared writes,
notifications and indications). LE signaling includes connection parameters,
disconnection and credit-channel setup. Unsupported opcodes/channels retain raw
bytes and an explicit unsupported label. Length checks reject truncated fields.
No C++ or receiver processing is involved in this protocol interpretation.

Known encrypted payloads stay opaque. Missing setup or capture gaps make the
encryption state uncertain; “Assume plaintext” allows a clearly labeled manual
interpretation for those connections, without overriding observed encryption.
The browser uses the encryption marker's capture time so earlier plaintext
packets remain inspectable. It does not reassemble L2CAP fragments: reliable
transmitter direction and fragment continuity are not available in this feed.
Fragments are identified, including declared versus captured L2CAP length, and
are never combined across the two directions. Application attribute values stay
hex (and printable text when applicable); proprietary value formats are not guessed.

Drag a timeline lane horizontally to pan. Scroll to zoom around the pointer;
horizontal trackpad scrolling pans, and pinch gestures zoom. The waterfall uses
the same window, with vertical dragging along its time axis. Navigation freezes
the view while reception continues; double-click or choose Live to follow again.
Zoom spans 1 ms to 120 seconds. Focus either plot for arrow-key panning, +/- zoom
and Home to return live. Presets and the history slider remain available.

`/events` pushes server-sent event batches every 50 ms. Every decoded packet is
included; plotting runs at up to 30 frames per second independently of delivery.
Reconnect cursors contain a server-session ID plus packet/spectrum sequence IDs.
The browser retains the latest 50,000 packets and explicitly labels retention;
there is no 200-row filter or packet sampling. Connection rows draw individual
packet marks at every zoom level. Pixel overlap at wide zoom is unavoidable.
The server retains 128 connections, 1,024 recent detailed marks and 128 control
markers per connection, while the separate packet ring supplies the complete
retained packet stream. Its older 100 ms activity summaries remain in the API.

`waterfall.h` accumulates complex power from every output sample of the existing
80-point polyphase FFT channelizer. It reports the 40 BLE bins at 2 MHz spacing,
averaged into 20 ms rows. It neither runs another FFT nor pretends these bins have
finer frequency resolution. The level is relative to an 8192-amplitude complex
input (dBFS), not calibrated dBm. Reset discards partial rows rather than blending
across missing input. Native status updates are 250 ms apart; the browser moves
the capture-time display between updates and freezes it on a stalled feed.

The waterfall retains up to 6,000 rows (two minutes). Packet outlines use capture
time and frequency; minimum-sized outlines make subpixel packets visible, so
outline height is not a precise airtime measurement. Color distinguishes
advertising/data packets and the selected connection. Input gaps are shaded,
and missing spectrum is left blank rather than interpolated. The browser pauses
painting when hidden, retains bounded data, and never uploads captures.
Each screen row integrates the linear power of every FFT row overlapping its
time interval, weighted by overlap duration. This avoids the flicker caused by
skipping FFT rows at wide time scales; it does not interpolate across gaps.

Waterfall Auto levels is enabled by default. Both color-scale endpoints follow
the last two seconds of FFT bins at the displayed capture time: the lower level
uses the fifth percentile minus 3 dB, and the upper level the 99th percentile
plus 3 dB, with at least 18 dB between them. Missing input and digital silence
do not drive the estimate. Updates run at 5 Hz with a 3-second lower-level time
constant; the upper level rises with a 0.5-second time constant and falls with a
4-second one. Pausing holds the scale; navigating history refits it. Turn Auto
levels off to adjust Low and High separately (minimum 5 dB separation). These
are display limits only; receiver gain and per-packet signal readings do not
change.

Validation:

```sh
tools/macos/run python3 -m unittest discover -s tests -p test_ble_connections.py
tools/macos/run python3 -m unittest discover -s tests -p test_ble_viewer_stream.py
tools/macos/run python3 -m unittest discover -s tests -p test_ble_wide.py
node tests/test_ble_plot_math.cjs
node tests/test_ble_packet_model.cjs
node tests/test_ble_pdu_decode.cjs
```

The connection tests cover setup, both channel-selection algorithms, Bluetooth's
published CSA #2 vectors, map-update instants, 16-bit wraparound, retransmissions,
missed events, encryption, interval updates, termination, address reuse, capture
loss/restart, bounded history and the loss-aware native clock. The all-channel
radio-signal tests continue to compare CPU/Metal packet positions and payloads.
Protocol reference: [Core Vol 6 Part B, connection events and channel selection](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/low-energy-controller/link-layer-specification.html);
independent vectors: [Core Vol 6 Part C §3](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-62/out/en/low-energy-controller/sample-data.html).
Browser protocol references: [Core Vol 3 Part A, L2CAP](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host/logical-link-control-and-adaptation-protocol-specification.html)
and [Core Vol 3 Part F, ATT](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/Core-61/out/en/host/attribute-protocol--att-.html).

### Automatic device names

The browser scans every delivered legacy advertisement and scan response for
Shortened Local Name (AD 0x08) and Complete Local Name (AD 0x09). The Nearby devices
panel merges observations by advertiser address **and address type**, prefers a
complete name, and keeps that name when later packets omit it. It shows source,
latest signal, and last-seen time; names also label connection lanes, endpoint
addresses, packet rows, and waterfall tooltips. Ambiguous public/random address
matches are not used to label connections.

This is passive observation of the captured traffic, not an active scan. A device
that never sends a local name stays unnamed. Extended advertising chains are not
followed by the current receiver. Random address rotation is not resolved into a
physical identity. The browser retains up to 10,000 recently observed addresses
for the current page/capture session, even after packets expire; reloading starts
again from available packet history. The directory displays up to 300 matching
addresses at a time and supports name/address search.

Format reference: [Bluetooth local name advertising data](https://www.bluetooth.com/wp-content/uploads/Files/Specification/HTML/CSS_v12/out/en/supplement-to-the-bluetooth-core-specification/data-types-specification.html).
Run `node tests/test_ble_devices.cjs` for name extraction and registry tests.

### Adaptive packet recovery

The optional `--demod adaptive` retains the original hard-decision decoder and adds
packet-local recovery for advertising packets and connections with a known CRC
seed. `--demod legacy` selects the original decisions for paired comparisons.
This is LE 1M receive processing; the browser still handles payload interpretation.

Recovery uses a bounded I/Q history and these steps:

- Look for the known advertising access address, or a known connection address.
  One damaged address bit can trigger recovery when the address is unambiguous;
  a known exact connection address can also tolerate one damaged preamble bit.
  Additional acquisition hypotheses at ±100 kHz run during power bursts.
- If the hard decoder did not already deliver the packet, calculate a full
  `atan2` frequency discriminator for its buffered samples. No `atan2` runs in
  the continuous idle-channel loop.
- Estimate frequency offset and fractional timing from the known 32-bit access
  address. Search quarter-sample timing positions, combine samples over each
  symbol, and try bounded clock-skew hypotheses of 0 and ±100 ppm.
- Decode the header and payload with that estimate, remove whitening, and
  require the ordinary CRC to match the existing seed. This does not flip bits
  to satisfy a CRC, infer new seeds from recovery candidates, or decrypt data.

A recovered packet is tagged `demod: "adaptive"`, with an estimated
`frequency_offset_hz`; other packets are tagged `demod: "hard"`. Status includes
`demodulator`, `recovery_attempts`, and `recovered_packets`. Duplicate detections
are suppressed by access address, CRC, and sample time. Input gaps cancel all
pending recovery. Recovery adds approximately 2.1 ms of reporting latency, while
preserving the packet's original capture timestamp; a file's last 2.1 ms may
not have enough following samples to complete recovery.

History is stored separately from each decoder's hot state. The application
uses a frame-major shared ring, and cheap address membership filters avoid hash
lookups for most samples. Pending candidates and history are bounded. This is
packet-local timing estimation, not a continuously tracking timing PLL or a
maximum-likelihood sequence detector.

Measured on the same saved 8-bit inputs, 2026-10-16:

| Capture set | Legacy | Adaptive | Legacy packets lost |
| --- | ---: | ---: | ---: |
| 32 original windows, 3.355 s | 434 | 544 | 0 |
| 8 fresh holdout windows, 0.839 s | 113 | 134 | 0 |

The original set gained 110 packets, including 11 below −50 dBFS and one at
−63.61 dBFS. Most gains were advertising packets: 108 advertising and two
connection packets with inferred CRC contexts. The holdout gained 20 advertising
packets and one inferred-context connection packet. These are small ambient
capture sets, not a calibrated sensitivity measurement or proof of a universal
percentage improvement.

An independently generated BT=0.5 GFSK/AWGN fixture decoded 73/144 packets versus
41/144 with the legacy decoder, across offsets up to ±140 kHz, fractional timing,
and noise levels. Known-connection recovery decoded 4/20 difficult packets versus
0/20. Tests also cover corrupted CRC rejection, noise-only input, duplicate
suppression, recovery cancellation on gaps, long packets with clock skew, all
40 channels, and CPU/Metal chunk boundaries.

```sh
make -C host/ble-wide check-demod
tools/macos/run python3 -m unittest discover -s tests -p test_ble_wide.py
python3 tools/ble/compare-demod.py
```

The comparison tool uses system Python with NumPy and saved raw snapshots under
`tools/iq4/build/precision/`. It applies the same IQ8 quantizer and channelizer to
both demodulators. Aggregate measurements are in `demod-results.json`; raw
captures, per-packet comparisons, and live trial logs remain in ignored build
folders. The live receiver continues to use 8-bit I/Q over 5 GbE.

The final deployed Metal/8-bit run processed 38.51 seconds of live samples with
zero missing input samples, 5,116 packets, and 1,069 recovery-path packets. The
reported processing capacity was 169.59 Msps for the 160 Msps input; sampled
backlog stayed at or below 4.51 ms. This is a short validation, with limited CPU
headroom, not a guarantee of lossless operation under additional laptop load.

## Preamble-assisted acquisition (experimental)

`--demod preamble` adds a soft preamble-first acquisition path to the adaptive
receiver. At the time of this experiment the default was `--demod adaptive`: the extra acquisition work
exceeded the full-band real-time budget on this Mac. `server.py` and
`tools/iq4/start-mac.py` accept the same `--demod` option.

At 2 Msps, the detector maintains an eight-symbol alternating correlation for
each of the two sample phases. Its input is the signed differential cross
product divided by `abs(cross) + abs(dot)`, retaining soft discriminator
information instead of slicing immediately to bits. It subtracts the window's
mean and requires squared normalized correlation >= 0.85. Scanning is gated by
short-term channel power exceeding 1.5 times its slow background estimate.
This gate is a computational compromise and can miss weak or sustained bursts.

A candidate queues the following access address. Once those samples arrive,
full phase differences are calculated on the buffered IQ. The six interior
preamble symbols estimate carrier offset and fractional timing on a
quarter-sample grid. Three adjacent bit boundaries address the ambiguity of
an alternating pattern that continues into the address. The address is then
read using that timing and offset; its first bit must match preamble polarity.
Advertising addresses may have up to two acquisition-bit errors; known data
addresses use exact/unambiguous one-bit-neighbor matching. Payload bits are
never repaired to pass CRC.

The preamble estimate is tried first for packet demodulation. For established
addresses, the existing AA-based timing/offset fits remain fallbacks. Unknown
addresses use only their preamble fit and no clock-skew retry; they still need
three distinct observations agreeing on the inferred CRC seed before emitting
packets. Such output remains labeled `repeated_crc_seed`, not a connection
observed in CONNECT_IND. Existing hard acquisition is retained, because the
short preamble alone missed packets that the existing detector could receive.

Candidate and recovery queues are bounded, known-address work takes priority
over speculative unknown addresses, and capture gaps clear correlation and
pending state. Recovery computes phase differences only as needed instead of
processing the maximum 260-byte frame for every rejected header. Status JSON
includes `preamble_candidates`, `preamble_syncs`, and `preamble_packets`.
Packet `demod: "preamble"` denotes preamble-assisted recovery, which can also
use the AA-fit fallback.

Validation on identical IQ8 samples:

- Original 32 snapshots: adaptive 544, preamble-assisted 556; all 544 retained.
- Separate eight snapshots: adaptive 134, preamble-assisted 138; all 134 retained.
- Of the 16 additional packets, nine are advertisements and seven use repeated
  CRC-seed context. None of these additions is below -50 dBFS; this experiment
  does not establish better weak-signal sensitivity.
- Deterministic GFSK tests exercise both preamble polarities, fractional sample
  offsets, +/-140 kHz carrier offsets, acquisition with hard searches disabled,
  unknown-seed confirmation, invalid CRCs, noise, and reset handling. The
  preamble-only path receives 64/144 noisy advertisement trials versus 73/144
  with existing adaptive acquisition; the combined path retains all 73.
- A 40-repeat Metal replay of capture-002 (4.194304 seconds of signal) measured
  165.5 Msps adaptive versus 136.0 Msps preamble-assisted with the live receiver
  also running. This is a relative development benchmark, not an isolated
  sustained-live capacity guarantee. Preamble mode is not enabled in the live
  viewer because it did not reach the 160 Msps input rate.

Reproduce the paired comparison:

```sh
python3 tools/ble/compare-demod.py --baseline adaptive --candidate preamble \
  --output host/ble-wide/build/preamble-final
tools/macos/run make -C host/ble-wide check-demod
```

Aggregate results are in `preamble-results.json`; capture-level outputs and
benchmarks are local under `build/preamble-*`.

## Channel-contiguous GPU output

The default `--bin-layout planar` produces one buffer containing 40 contiguous
channel regions. Each region holds interleaved I/Q pairs for every sample in
the batch: `bins[2 * (channel * frames + time) + component]`. Decoder workers
read their region sequentially. The CPU channelizer produces the same layout.
The receiver's `--bin-layout interleaved` option retains the former time-major
layout for comparisons; `bin_layout` in status identifies the actual choice.

Metal stores the FIR/FFT results directly in the requested layout. This removes
the earlier tiled transpose kernel, its scratch buffer, and its extra GPU
read/write pass. There is one submission and completion wait per batch.
The output memcpy into the pipeline's owned job buffer remains,
which lets later GPU work overlap CPU decoding without reusing live data.

The waterfall also consumes batches directly, accumulating each channel in
sample order and retaining its 20 ms row boundaries across batches. It does
not reconstruct an interleaved buffer. Single-worker reference decoding still
visits channels in sample order, even with planar storage, to preserve the
serial connection-discovery baseline.

Numerical tests compare both GPU layouts bit-for-bit, including 31-, 32- and
33-frame transpose edges, partial jobs, empty output, reused buffers, silence,
impulses and noise. CPU versus GPU maximum absolute component error remains
`9.15527344e-05`. Integration tests cover both layouts, both channelizer
backends, both decoders, and 1/4/40 decoding workers with packets crossing batch
boundaries. A dedicated waterfall test checks exact level equality and row
boundary timing, multi-row batches and reset; it also passes ASan/UBSan.

Across the same 40 saved recordings, planar and interleaved output recover
exactly the same **678 adaptive** and **893 streaming** packets, including
sample positions, payloads, CRCs, RSSI and capture times. Waterfall JSON rows
also match exactly. No radio configuration or live receiver was changed.

```sh
tools/macos/run make -C host/ble-wide all check-metal check-waterfall
tools/macos/run host/ble/build/venv/bin/python -m unittest discover -s tests -p test_ble_wide.py
python3 tools/ble/benchmark-layout.py --demod streaming --decode-threads 4
```

Final paired 40-repeat Metal replays (median of three runs per layout):

| Decoder / workers | Interleaved Msps | Planar Msps | Interleaved CPU s | Planar CPU s |
| --- | ---: | ---: | ---: | ---: |
| streaming-4 | 265.0 | 363.3 | 5.282 | 3.742 |
| streaming-40 | 351.6 | 432.2 | 6.468 | 5.298 |
| adaptive-4 | 365.3 | 486.2 | 3.523 | 2.561 |

Every run processes 4.194304 seconds of RF input. Background load varied,
including one slow interleaved streaming run; the per-run results and hashes
are preserved in [layout-results.json](layout-results.json).

The benchmark freezes the executable and runs alternating layouts on the same
IQ8 recording. `gpu_s` includes both channelization and the GPU transpose in
planar mode. CPU time sums active decoder thread time; `processing_s` measures
elapsed pipeline time and excludes file I/O and IQ quantization. System load
varies, so these results are not a live lossless-throughput guarantee.

## Parallel packet decoding

The receiver, web server and Mac launcher accept `--decode-threads 1..40`.
The default is **4**. `--decode-threads 1` retains the original sample-major
execution order for comparison. This setting is independent of `--backend`:
Metal channelization runs separately, and the CPU channelizer keeps its three
filter workers.

`decode-pool.h` dispatches whole channel batches to persistent workers. The
calling thread participates, so four means three background workers plus the
caller. There is one decoder state per channel; a channel never has two batches
in flight. Workers finish before the next batch, gap reset or status snapshot.
No per-sample queue, copying of packet histories, or extra payload decode is
introduced. Adaptive recovery histories are private to each channel to avoid
workers writing neighboring elements of a shared cache line.

Advertising channels are processed first on the calling thread, allowing
`CONNECT_IND` to install context before data workers process the same batch.
Thus connection knowledge can be up to one batch ahead (4.096 ms with Metal).
Data channels 0–36 are then distributed dynamically. A mutex protects registry
updates and candidate lookups, while atomic membership filters keep ordinary
sample screening lock-free. Lookups return copied contexts, never pointers that
another worker could invalidate. Blind CRC confirmation counts distinct sample
timestamps even if workers submit them out of order. Acquisition hints still do
not validate a packet. Cross-channel discovery completion order can differ in
parallel mode; identical results on the checked recordings are not a promise
of identical blind-acquisition decisions on every possible input.

Each channel collects its own packet output. After the batch completes, the
caller merges packets in timestamp/channel order and writes JSON, advances the
capture clock, and updates the waterfall. This prevents interleaved JSON or
races in viewer state. Sorting is within a batch; the adaptive decoder can
still recover older packets in a later batch.

Validation compares one and four workers on 40 recordings: all **678 adaptive**
and **893 streaming** packets match exactly by channel, start sample, access
address, payload and CRC. Neither mode loses or adds packets. Synthetic tests
also cover all 40 channels at once, `CONNECT_IND` followed by data on all 37
data channels, batches of 173 and 8192 frames, two capture epochs, concurrent
registry updates, reordered CRC votes, and worker-exception recovery. Tests use
2, 4 and 40 workers, and pass ThreadSanitizer. The existing CPU/Metal channelizer
and viewer stream tests pass too.

```sh
tools/macos/run make -C host/ble-wide all check-decode-pool
python3 tools/ble/benchmark-threads.py --demod streaming
# Pass the same option through the normal launcher:
# tools/macos/run python3 tools/iq4/start-mac.py ... --decode-threads 4
```

On this Mac, paired 40-repeat Metal replays measured median processing
capacity of **213.6 → 382.5 Msps** for adaptive and **175.7 → 312.6 Msps** for
streaming when moving from one to four workers. Streaming batch wall time fell
from **3.465 to 1.734 seconds** for 4.194 seconds of RF input; summed decoder CPU
time rose from **3.461 to 4.724 seconds**. Forty workers reached **400.5 Msps**
streaming but used **6.171 CPU seconds**. These are parallel speedups, not a
reduction in total CPU work. macOS background load varied during the runs.
[Detailed measurements](threading-results.json) preserve the individual runs
and hashes. The receiver was not live during this validation.

The benchmark freezes the executable and records its hash, the capture hash,
worker counts, packet totals, CPU time and elapsed time. It runs worker counts
in forward and reverse order. Processing capacity excludes file I/O and IQ8
quantization; it is not a live packet-loss measurement. Four workers are the
default to retain headroom for the laptop's other work. Higher worker counts
are available, but each additional thread need not correspond to another
physical core or improve total CPU efficiency.

## Single-pass decoder

`--demod streaming` selects `streaming-demod.h`. It replaces the earlier
multi-timing experiment with one payload state per channel: synchronization,
one timing tracker, a four-state GFSK sequence detector, then incremental
dewhitening and CRC. A header or CRC failure ends the attempt. There is no
packet replay, retry at another timing, or maximum-length recovery wait.

Acquisition uses the alternating preamble and, when available, a known access
address. It chooses timing and frequency offset before payload decoding.
A 128-sample circular synchronization/filter window and fixed decision history
bound memory independently of packet length. The Viterbi detector processes
each symbol once and commits one bit per stage after an eight-stage warmup.
Its states model neighboring bits' effect on the Gaussian waveform. Unknown
addresses still require three distinct packets agreeing on the inferred CRC
seed; acquisition hints do not confer CRC trust.

The [research and architecture notes](single-pass-research.md) identify the
papers and patents consulted, explain where this implementation differs, and
describe limitations. [Measured results](streaming-results.json) include the
frozen executable hash, packet matching, signal-level breakdown, synthetic
noise sweep and repeated throughput measurements.

Identical IQ8 recordings, after the final acquisition changes:

| Dataset | Adaptive | Single pass | Retained | Missed | Added |
| --- | ---: | ---: | ---: | ---: | ---: |
| Original 32 snapshots | 544 | 737 | 526 | 18 | 211 |
| Separate eight snapshots | 134 | 156 | 129 | 5 | 27 |
| Total | 678 | 893 | 655 | 23 | 238 |

The gain is not uniform: advertisements increased from 654 to 877, but packets
from blindly discovered connections decreased from 24 to 16 (14 retained,
10 missed, two added). These short captures reset discovery context each time.
They do not establish continuous connection-following performance. These historical
results preceded the switch to streaming as the receiver, server and launcher default.

All 17 baseline packets below -50 dBFS were retained, with 19 additional
packets measured below that level. Of the 36 baseline packets between -50 and
-45 dBFS, 34 were retained. These are digital signal levels and observed
packet retention, not calibrated RF sensitivity or known transmission counts.

The paired synthetic GFSK/AWGN sweep has 80 packets per point (advertising and
known-address data), varied frequency/timing offsets, and independent noise
seeds. SNR is `100^2/(2*sigma^2)` at the 2-Msps decoder input:

| SNR | Adaptive | Single pass |
| --- | ---: | ---: |
| 6 dB | 0/80 | 27/80 |
| 8 dB | 9/80 | 69/80 |
| 10 dB | 50/80 | 78/80 |
| 12 dB | 76/80 | 80/80 |
| 14 dB | 80/80 | 80/80 |

Four alternating 40-repeat Metal replays measured median processing capacity
of **219.8 Msps adaptive versus 180.5 Msps single pass**. Decoder elapsed time
was **2.715 versus 3.376 seconds**, about 24% more. Thus the new decoder improves
recovery in this dataset but does not reduce total processing cost. The
4.194-second RF replay exceeds the nominal 160-Msps stream rate in processing
capacity, but excludes file I/O and IQ quantization and is not proof of lossless
live operation. The live receiver remained running; system load was uncontrolled.

Validation includes exact one-pass counters for valid and bad-CRC packets,
phase-cache and sample-window audits, 40 simultaneous CPU/Metal channels,
clock skew, gaps, unknown-seed confirmation, noise, and ASan/UBSan. The earlier
multipath experiment's results are preserved in
`streaming-multipath-results.json`; its numbers do not describe this version.

Reproduce (the comparison script uses the project's existing NumPy environment):

```sh
tools/macos/run make -C host/ble-wide all check-streaming check-demod check-sensitivity
host/ble/build/venv/bin/python tools/ble/compare-demod.py \
  --baseline adaptive --candidate streaming --backend metal \
  --output host/ble-wide/build/single-pass-complete
host/ble/build/venv/bin/python tools/ble/compare-demod.py \
  --baseline adaptive --candidate streaming --backend metal \
  --captures host/ble-wide/build/demod-holdout \
  --output host/ble-wide/build/single-pass-complete-holdout
python3 tools/ble/benchmark-demod.py \
  --output host/ble-wide/build/single-pass-benchmark-complete
```

Replay tools freeze the executable before starting and save its SHA-256.
Detailed logs remain local under the output directories above.

## Excluding received packets from noise learning

The streaming decoder's power gate previously learned from all samples,
including received packets. A strong central transmission raised the smoothed
background estimate for milliseconds, suppressing the more sensitive acquisition
path when a much weaker peripheral replied after 150 us. Merely freezing the
estimate during payload decoding would still admit the preamble and access
address before synchronization completed.

`packet-noise.h` delays noise learning by 256 samples (128 us). Recognized bursts
exclude the 64 us before the access address, covering a leading carrier observed
about 50 us before the address in peripheral replies. Acquisition finishes before
these powers are committed; the payload and a short trailing guard are excluded too. Both the background estimate used for smoothed acquisition and the
floor used for blind-address screening exclude the burst. Acquired frames are
excluded even when their CRC fails: an RF transmission is not idle noise.
Unrecognized transmissions can still contribute to the estimate. Noise changes
outside excluded intervals continue to adapt normally; resets clear the delay
ring and exclusion state.

Only scalar sample powers are delayed; there is no IQ replay, payload retry, or
additional timing candidate. The acquisition gate remains enabled. A constant
128-double ring per channel and a merged exclusion interval bound storage; a
merged interval can also omit a short idle gap between closely spaced candidate
bursts. Packet energy/RSSI and waterfall accumulation are unchanged.

At an 8 dB weak-packet SNR with a preceding packet 22 dB stronger, the paired
synthetic test now recovers **90/100** replies, matching **90/100** when received
alone; the previous receiver recovered **29/100** replies. Tests cover equal,
22 dB and 30 dB power differences at 6, 8, 10 and 12 dB SNR, with different
transmitter frequency offsets and fractional timing. All **893** packets in
40 saved recordings remain unchanged. Existing noise-only and CRC-rejection
tests pass, as do exact estimator-accounting, reset and ASan/UBSan tests.

[Results and paired timing measurements](packet-noise-results.json) preserve
individual outcomes. The paired timing check showed about 3% higher summed
decoder CPU time; the live receiver was also running during that check. This
fix was applied to the live streaming receiver after validation. A viewer
snapshot and previous logs were saved locally before its restart.

```sh
tools/macos/run make -C host/ble-wide all check-packet-noise check-streaming check-decode-pool
```

## Connection reply synchronization

After a CRC-valid LE 1M data packet, the streaming decoder uses its address,
airtime, and the default 150 us inter-frame space to schedule a short reply
synchronization search. Nine nearby address-end positions enter the existing
GFSK synchronization fit. These timed candidates allow up to six address-bit
errors; ordinary known-address candidates retain their two-error limit. One
synchronization fit is selected before a single payload decode. The observed
CRC seed must validate the resulting bytes; predicted replies never become
viewer packets by themselves.

The hint bypasses the power gate for this short search. It preserves other
observed address candidates, expires after one opportunity, and is cleared on
stream discontinuities. Blind address guesses near the expected response wait
for the reply address to finish before starting a payload. Other packet timings
remain eligible for ordinary acquisition. The address lookup also checks the
full address before deciding whether one-bit correction is needed: matching the
low-16 membership filter alone must not turn a damaged address into a separate
connection.

On the saved 2.097-second live capture, replies after 38 captured central packets
increased from 5 to 9. All 43 previously decoded connection packets remain, with
four additions. Across 40 older recordings, all 893 previous packets remain and
one is added. This is an improvement, not a complete explanation of the remaining
missing replies. See `reply-sync-results.json` for measurements and limitations.
The final three paired replays measured about 7% more decoder CPU; processing
capacity remained above the 160 Msps input rate on the test Mac.

`make -C host/ble-wide check-reply-sync` checks variable packet airtimes, different
response timings and addresses, one payload decode, absent/bad-CRC replies,
stream resets, and upper-address-bit errors that collide in the membership
filter. Captured IQ and replay tools remain local under `build/`.

## Per-packet noise display

Streaming packet events include `noise_dbfs`: mean channel power from the last
complete 64-sample (32 µs) contiguous window accepted by `PacketNoise`, frozen
when synchronization selects the packet. A 256-sample learning delay
keeps the leading carrier, preamble and address out of the window. Recognized packet intervals
and their guards reset the measurement window; a complete new window is required.
The display measurement does not change acquisition thresholds. Missing windows,
zero-power fixtures, and decoder modes without this measurement emit JSON `null`.

The connection packet table shows noise beside RSSI, including ranges for empty
packet groups. Packet details and plot tooltips also show it. This is nearby
background power, including interference or unrecognized transmissions, not a
calibrated thermal-noise measurement. Levels can change between that window and
the payload, so subtracting the two displayed readings is not an exact SNR.


## Missing peripheral replies: independent review

Known data-address synchronization now weights each phase observation by the
geometric mean amplitude of its adjacent IQ samples. This reduces the influence
of weak samples when fitting timing, carrier offset and modulation depth. The
56 weights are cached once per candidate; all timing hypotheses fit the same
observations. Advertising and unknown-address acquisition retain their prior
fit. There is still one payload decode, with no CRC-guided retry.

Two independent captures improve from 9 to 11 replies out of 38 opportunities,
and from 21 to 22 out of 42. Exact packet comparisons lose none. The combined
synchronization and leading-carrier noise changes recover 895 rather than 894
packets across 40 other recordings, with no losses. A 1,200-case synthetic
frequency/timing/gain-step sweep adds three decodes without losing any.

Independent mixing/FIR replay agrees with the GPU channelizer within 0.002 ADC
units. Widening its filter recovers some replies but loses others and weakens
alias rejection, so the filter remains unchanged. Fixed-gain hardware trials did
not resolve the channel-dependent misses; automatic gain was restored. Those
sequential trials included reconnects and are not a controlled sensitivity test.

Captured peripherals can emit a carrier about 42 us before the preamble. The
noise guard now covers it. In one checked reply this corrects the background
reading from −48.30 to −57.07 dBFS, matching nearby quiet samples. It does not by
itself improve packet recovery, and gain changes still prevent interpreting
RSSI minus background as an exact SNR. Missing replies remain an open problem.

[Detailed results](reply-investigation-results.json) include the newly recovered
packets and experiment locations. `check-streaming` includes noisy synchronization
gain-step fixtures; `check-packet-noise` verifies delayed leading-carrier exclusion.

## Displaying CRC failures

The streaming receiver publishes completed CRC-failed frames as diagnostic packet
events with `crc_valid:false` and `expected_crc`, alongside received `crc`, raw
PDU bytes, signal power and the pre-packet background estimate. Valid events use
`crc_valid:true`; older recordings without the field remain viewable as valid.
Successful-packet counters still count only validated packets. Synchronization
misses, rejected headers and unconfirmed unknown CRC seeds are not emitted as
CRC failures.

“Show CRC failures” controls the timeline, waterfall and both packet lists. It
starts enabled and is remembered in the browser. Failed frames are red, never
grouped into empty-packet runs, and show raw bytes without protocol interpretation.
They can be associated with an existing matching connection but never create or
update one, learn device names, apply control messages, or schedule decoder reply
hints. Visibility is a display filter: hiding failures does not stop capture.
