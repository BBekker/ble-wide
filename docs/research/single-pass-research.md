# Single-pass BLE LE 1M receiver

The implementation is `streaming-demod.h`, selected with `--demod streaming`.
It replaces the earlier eight-timing streaming experiment. There is one active
payload decoder per RF channel. A failed header or CRC ends that attempt;
there is no alternate timing, clock-skew retry, or whole-packet replay.

## Prior art and the choices made here

- Hosseini and Perrins, **Timing, Carrier, and Frame Synchronization of
  Burst-Mode CPM**, IEEE Transactions on Communications, 2013,
  [author manuscript](https://arxiv.org/pdf/1310.0757),
  DOI 10.1109/TCOMM.2013.111613.130667. This develops data-aided feedforward
  synchronization for CPM bursts. It supports separating acquisition from
  payload detection. Its optimized preamble and reported performance do not
  transfer directly to BLE's fixed eight-bit preamble; this implementation
  instead uses the known access address to refine timing and frequency.
- Lampe, Schober and Jain, **Noncoherent Sequence Detection Receiver for
  Bluetooth Systems**, IEEE JSAC, 2005,
  [author manuscript](https://people.ece.ubc.ca/lampe/Preprints/2004-JSAC-NSD.pdf),
  DOI 10.1109/JSAC.2005.853791. This demonstrates why exploiting GFSK's memory
  can outperform a per-symbol discriminator with a small trellis. It concerns
  Bluetooth Classic and uses a different two-state noncoherent detector. Our
  four-state discriminator-domain detector is an independently implemented
  approximation, not a reproduction of its receiver or its sensitivity figures.
- [US7239675B2, **GFSK receiver**](https://patents.google.com/patent/US7239675B2/en).
  Discusses synchronization using differential samples and a known sync word,
  followed by detection that considers multi-symbol GFSK waveforms. This
  informed using pulse-shaped templates rather than treating adjacent bits
  as independent flat frequency levels.
- [US10015029B2, **Bluetooth signal receiving method and device using improved
  symbol timing offset compensation**](https://patents.google.com/patent/US10015029B2/en).
  Explains short-preamble timing uncertainty and timing tracking through the
  rest of a BLE packet, including selective pattern-directed tracking. The
  implementation uses a bounded Gardner transition-error loop; it does not
  implement that patent's exact selective-pattern architecture or claim its
  measured improvement.
- [WO2021250042A1, **Frequency offset estimation**](https://patents.google.com/patent/WO2021250042A1/en).
  Describes data-aided timing/frequency estimation with a correlation template
  and subsequent frequency refinement. It also illustrates that synchronization
  can select parameters before message decoding. Our estimator is a linear
  fit of differential phase to a Gaussian pulse template, not its complex
  double-correlator and matched-filter bank.

## Receiver structure

1. Maintain the differential complex product once per arriving sample. Raw
   address shifts are inexpensive and run continuously. A power-gated,
   normalized alternating-preamble correlator provides a coarse carrier offset
   for an additional acquisition search. The phase angle is computed lazily
   and cached, at most once for each input sample.
2. Keep only a 128-sample (64 microsecond) circular synchronization/filter
   window. A bounded 20-sample acquisition interval resolves preamble/address
   boundary ambiguity before allocating a payload state. Known addresses have
   priority over speculative blind addresses. This interval fits the eight
   alternating preamble symbols plus interpolation guards.
3. Fit the known address's Gaussian-shaped differential waveform to the same
   fixed observations at nine fractional delays. Jointly estimate DC/carrier
   offset and pulse amplitude. Comparing fixed observations avoids favoring
   a delay merely because interpolation reduced the noise variance. For an
   unknown address, rank frame boundaries using the known preamble, not the
   self-consistency of guessed address bits. No payload or CRC participates
   in this choice. A bounded, expiring address hint from a previously
   completed candidate can break a blind-acquisition tie. It grants no CRC
   trust; publication still requires three packets agreeing on the seed.
4. Select one address/timing/offset estimate. A bounded Gardner loop adjusts
   the symbol period and phase forward. There is no bank of payload timings.
5. Advance one four-state Viterbi detector per symbol. Its state describes
   adjacent transmitted bits, not different packet timings. Gaussian pulse
   coefficients include the effect of fractional interpolation. Eight branch
   comparisons update four survivor metrics; fixed-length decision histories
   produce one committed bit per symbol after an eight-stage warmup. This is
   approximate sequence detection: the model truncates pulse memory and does
   not model the full correlated discriminator-noise distribution.
6. Dewhiten each committed bit once, parse one header, and update one CRC.
   Finish at the actual indicated packet length plus the bounded decision
   delay. Unknown connections retain the three-distinct-packet CRC-seed
   confirmation rule. A CRC failure never changes the timing or re-runs bits.

The rolling filter window and overlapping interpolation taps reuse neighboring
samples in the normal filtering sense. They are not a packet recording: there
is no length-dependent sample history, backward payload traversal, CRC-directed
search, or delayed maximum-length recovery. The single payload state stores
only decoded bytes, four path metrics and bounded survivor histories.

## Verification and limits

`test-streaming.cpp` audits sample-window bounds and cached phase evaluation,
asserts one payload start and exactly one dewhitening/CRC step per transmitted
payload bit for clean and deliberately corrupt packets, and checks the fixed
Viterbi warmup. It also checks fractional timing, both preamble polarities,
carrier offsets, long packets with clock skew, gaps, all whitening channels,
blind CRC-seed confirmation, CRC rejection and two million noise samples.
`test-sensitivity.cpp` compares both receivers on exactly the same generated
GFSK/AWGN samples. The 14 dB fixtures assert full recovery, catching premature
blind-address acquisition. CPU/Metal integration covers 40 simultaneous
channels and processing-chunk boundaries. Sanitizer builds check memory and
undefined behavior.

Measurements are in `streaming-results.json`. The original multipath experiment
is preserved separately in `streaming-multipath-results.json`. Recorded capture
windows are short and independent, not a continuous connection-following test.
Synthetic SNR is explicitly defined at the decoder's 2-Msps input; it is not
calibrated RF sensitivity. Added packets do not prove that every baseline
packet is retained. Report connection and advertisement recovery separately.

Only one overlapping packet can be active on a channel. False acquisition can
occupy that state until header/CRC rejection. Blind framing is less certain
than recognition of a known access address. The current timing loop tracks
clock error; carrier offset is estimated during acquisition and held for the
packet. LE 2M, LE Coded and encrypted payload interpretation are outside this
receiver change.
