// SPDX-License-Identifier: MIT
#pragma once
#include "decoder/decoder.h"
#include "decoder/packet-noise.h"
#include "decoder/streaming-vector.h"
#ifdef STREAMING_AUDIT
#include <cassert>
#endif

// LE 1M at 2 Msps: synchronization search, then exactly one payload decoder.
// The 128-sample ring is only a bounded synchronization/interpolation window.
// CRC never selects a timing, retries a packet, or reads payload samples again.
class StreamingDecoder : public DecoderStats {
    struct Frame {
        uint64_t start = 0, energy_sample = 0;
        double offset = 0, energy_start = 0, noise_dbfs = NAN;
        uint32_t aa = 0, init = 0, crc = 0;
        unsigned bits = 0, target = 0, whitening = 0;
        bool known = false;
        std::array<uint8_t, 260> bytes{};
    } frame;
    struct Correlator {
        std::array<float, 8> values{};
        double sum = 0, square = 0, alternating = 0;
        unsigned index = 0, count = 0;
    } coarse[2];
    Registry &registry;
    std::function<void(const Packet &)> emit;
    int channel;
    bool report_crc_failures;
    uint64_t samples = 0, since_reset = 0, last_start = UINT64_MAX;
    uint32_t last_aa = 0, last_crc = 0;
    float prev_i = 0, prev_q = 0;
    double energy = 0, power = 0;
    PacketNoise noise;
    std::array<float, 128> fm{}, dx{}, dy{};
    std::array<uint64_t, 128> fm_tag{};
    std::array<double, 128> energy_prefix{};
    uint64_t decision_sample = 0;
    uint64_t raw_word[2]{}, smooth_word[2]{}, corrected_word[2]{};
    double coarse_offset = 0, coarse_score = 0;
    uint64_t coarse_end = 0, pending_end = 0, blocked_until = 0;
    uint64_t reply_start = UINT64_MAX, reply_due = UINT64_MAX;
    uint32_t reply_aa = 0;
    struct SyncCandidate {
        uint64_t end;
        uint32_t aa;
        bool timed = false;
    };
    std::array<SyncCandidate, 24> syncs{};
    unsigned sync_count = 0;
    bool pending = false, active = false;
    std::array<double, 4> metrics{};
    std::array<uint64_t, 4> paths{};
    unsigned trellis_steps = 0;
    double next_symbol = 0, period = 2, previous_symbol = 0, deviation = .6;

    static bool valid_address(uint32_t aa) {
        if (__builtin_popcount(aa ^ 0x8e89bed6u) <= 1)
            return false;
        unsigned run = 1, transitions = 0, top = 0;
        for (int k = 1; k < 32; k++) {
            bool same = ((aa >> k) & 1) == ((aa >> (k - 1)) & 1);
            run = same ? run + 1 : 1;
            if (run > 6)
                return false;
            if (!same) {
                transitions++;
                if (k >= 27)
                    top++;
            }
        }
        return transitions <= 24 && top >= 2 &&
               !((aa & 255) == ((aa >> 8) & 255) && (aa & 255) == ((aa >> 16) & 255) &&
                 (aa & 255) == (aa >> 24));
    }
    bool consume(Frame &f, unsigned bit) {
        payload_symbols++;
        bit ^= f.whitening & 1;
        f.whitening = (f.whitening >> 1) ^ ((f.whitening & 1) ? 0x44 : 0);
        unsigned pos = f.bits++ - 32;
        if (!(pos & 7))
            f.bytes[pos / 8] = 0;
        f.bytes[pos / 8] |= uint8_t(bit << (pos & 7));
        if (pos < 16 || pos < (unsigned(f.bytes[1]) + 2) * 8)
            f.crc = crc_step(f.crc, bit);
        if (pos == 15) {
            unsigned len = f.bytes[1], type = f.bytes[0] & 15;
            bool good = f.aa == 0x8e89bed6 ? (type == 7 ? len >= 1
                                              : (type == 0 || type == 2 || type == 4 || type == 6)
                                                  ? len >= 6 && len <= 37
                                              : (type == 1 || type == 3) ? len == 12
                                                                         : type == 5 && len == 34)
                                           : (f.bytes[0] & 3) && !(f.bytes[0] & 0xe0) && len <= 251;
            if (!good) {
                return false;
            }
            f.target = (len + 5) * 8;
        }
        if (pos + 1 != f.target)
            return true;
        unsigned n = f.bytes[1] + 2;
        uint32_t crc = f.bytes[n] | uint32_t(f.bytes[n + 1]) << 8 | uint32_t(f.bytes[n + 2]) << 16;
        if (last_start != UINT64_MAX && f.start <= last_start + 4 && f.start + 4 >= last_start &&
            last_aa == f.aa && last_crc == crc)
            return false;
        bool crc_valid = true;
        if (f.known) {
            if (f.crc != crc) {
                crc_fail++;
                crc_valid = false;
                if (!report_crc_failures)
                    return false;
            }
        } else {
            // Reverse decoded bytes, never samples. Independent packets must
            // agree on the CRC seed before an unknown connection is published.
            f.init = recover_init(f.bytes.data(), n, crc);
            candidates++;
            registry.hint_address(f.aa, f.start);
            if (!registry.confirm(f.aa, f.init, f.start))
                return false;
        }
        auto c = registry.known(f.aa);
        Packet p{channel,
                 f.start,
                 f.aa,
                 crc,
                 reverse24(f.init),
                 f.aa == 0x8e89bed6 ? "advertising"
                 : c && c->observed ? "connection_request"
                                    : "repeated_crc_seed",
                 std::vector<uint8_t>(f.bytes.begin(), f.bytes.begin() + n)};
        p.demod = "streaming";
        p.noise_dbfs = f.noise_dbfs;
        p.frequency_offset_hz = f.offset * (1e6 / 3.14159265358979323846);
        p.signal_dbfs =
            10 *
            std::log10(std::max(1e-12, (energy_prefix[decision_sample & 127] - f.energy_start) /
                                           (decision_sample - f.energy_sample) / (8192. * 8192.)));
        p.crc_valid = crc_valid;
        p.expected_crc = f.crc;
        if (!crc_valid) {
            // Diagnostic output only: do not learn context, schedule replies,
            // or count this damaged frame as a successful decode.
            emit(p);
            return false;
        }
        last_start = f.start;
        last_aa = f.aa;
        last_crc = crc;
        if (f.aa != 0x8e89bed6) {
            // LE 1M, default 150 us inter-frame space. This is a bounded
            // synchronization hint, never evidence that a reply was received.
            // Ordinary acquisition still handles other timings/addresses.
            reply_start = f.start + (n + 8) * 16 + 300;
            reply_aa = f.aa;
            reply_due = reply_start + 67;
        }
        valid++;
        preamble_packets++;
        registry.connection(p);
        emit(p);
        return false;
    }

    // Odd minimax polynomial on [0,1], with quadrant reduction. Tested against
    // atan2; its error is tiny relative to discriminator noise. One division.
    static float angle(float y, float x) {
        float ax = std::abs(x), ay = std::abs(y), hi = std::max(ax, ay);
        if (hi == 0)
            return 0;
        float z = std::min(ax, ay) / hi, z2 = z * z;
        float a = z * (.9998660f +
                       z2 * (-.3302995f + z2 * (.1801410f + z2 * (-.0851330f + z2 * .0208351f))));
        if (ay > ax)
            a = 1.57079632679f - a;
        if (x < 0)
            a = 3.14159265359f - a;
        return y < 0 ? -a : a;
    }
#ifdef STREAMING_AUDIT
  public:
    static float test_angle(float y, float x) { return angle(y, x); }

  private:
#endif
    float sample_angle(uint64_t n) {
#ifdef STREAMING_AUDIT
        assert(n <= samples && samples - n < 128 && samples - n <= since_reset);
#endif
        if (fm_tag[n & 127] != n + 1) {
            fm[n & 127] = angle(dy[n & 127], dx[n & 127]);
            discriminator_evaluations++;
            fm_tag[n & 127] = n + 1;
        }
        return fm[n & 127];
    }
    float interpolate(double t) {
        auto n = uint64_t(t);
        float f = float(t - n);
        float a = sample_angle(n);
        return f == 0 ? a : a + f * (sample_angle(n + 1) - a);
    }
    double symbol(double t) { return .5 * (interpolate(t - .5) + interpolate(t + .5)); }
    [[gnu::always_inline]] void correlate(float value) {
        auto &c = coarse[samples & 1];
        float old = c.count < 8 ? 0 : c.values[c.index];
        c.values[c.index] = value;
        c.index = (c.index + 1) & 7;
        c.sum += double(value) - old;
        c.square += double(value) * value - double(old) * old;
        c.alternating = value - c.alternating - old;
        if (c.count < 8 && ++c.count < 8)
            return;
        double variance = 8 * c.square - c.sum * c.sum;
        double corr = c.alternating * c.alternating;
        if (variance < .3 || corr < .65 * variance || std::abs(c.sum / 8) > .65)
            return;
        double score = corr / (variance + .02);
        if (samples >= coarse_end || score > coarse_score) {
            coarse_offset = c.sum / 8;
            coarse_score = score;
            coarse_end = samples + 80;
            preamble_candidates++;
        }
    }
    [[gnu::always_inline]] void detect(uint64_t word, uint64_t end) {
        if (active || end < blocked_until || since_reset < 82)
            return;
        uint32_t aa = uint32_t(word >> 8);
        if (__builtin_popcount(aa ^ 0x8e89bed6u) <= 2)
            aa = 0x8e89bed6;
        // The low-bit filter is only a lookup hint. A high-bit AA error has
        // the same low bits and still needs exact lookup/neighbor correction.
        else {
            uint32_t resolved = registry.resolve_address(aa);
            if (resolved)
                aa = resolved;
            else if (uint8_t(word) != ((aa & 1) ? 0x55 : 0xaa) || power < 4 * noise.floor() ||
                     !valid_address(aa))
                return;
        }
        if (!pending) {
            pending = true;
            pending_end = end;
            sync_count = 0;
        }
        for (unsigned k = 0; k < sync_count; k++)
            if (syncs[k].aa == aa && syncs[k].end == end)
                return;
        if (sync_count < 12)
            syncs[sync_count++] = {end, aa};
        else if (aa == 0x8e89bed6 || registry.known(aa)) {
            // Reserve acquisition capacity for a known address even when an
            // extended alternating run produced earlier blind candidates.
            for (auto &candidate : syncs)
                if (candidate.aa != 0x8e89bed6 && !registry.known(candidate.aa)) {
                    candidate = {end, aa};
                    break;
                }
        }
    }
    static double pulse(double x) {
        constexpr double c = 2.668223128318498; // sqrt(2)*pi*BT/sqrt(log(2)), BT=.5
        double sum = 0;
        for (int j = 0; j <= 16; j++) {
            double u = x - .25 + j / 32.;
            double p = .5 * (std::erf(c * (u + .5)) - std::erf(c * (u - .5)));
            sum += p * (j == 0 || j == 16 ? 1 : j & 1 ? 4 : 2);
        }
        return sum / 48;
    }
    using BranchTaps = std::array<std::array<std::array<double, 3>, 2>, 32>;
    static const BranchTaps &branch_taps() {
        static const BranchTaps table = [] {
            BranchTaps out{};
            for (unsigned k = 0; k < 32; k++)
                for (int j = 0; j < 2; j++)
                    for (int d = -1; d <= 1; d++) {
                        double t = k / 32., at = t + j - .5, n = std::floor(at), f = at - n;
                        out[k][j][d + 1] =
                            (1 - f) * pulse((n - t) / 2 - d) + f * pulse((n + 1 - t) / 2 - d);
                    }
            return out;
        }();
        return table;
    }
    using Templates = std::array<std::array<std::array<double, 5>, 2>, 9>;
    static const Templates &templates() {
        static const Templates table = [] {
            Templates out{};
            for (int phase = -4; phase <= 4; phase++)
                for (int sub = 0; sub < 2; sub++)
                    for (int d = -2; d <= 2; d++)
                        out[phase + 4][sub][d + 2] = pulse((sub - phase * .25) * .5 - d);
            return out;
        }();
        return table;
    }
    using SyncShapes = std::array<std::array<std::array<double, 10>, 32>, 2>;
    static const SyncShapes &sync_shapes() {
        // A five-bit neighborhood has only 32 possible pulse sums. Preserve
        // the original addition order, but compute them once, not at every fit.
        static const SyncShapes shapes = [] {
            SyncShapes out{};
            const auto &taps = templates();
            for (unsigned phase = 0; phase < 9; ++phase)
                for (unsigned sub = 0; sub < 2; ++sub)
                    for (unsigned bits = 0; bits < 32; ++bits)
                        for (unsigned d = 0; d < 5; ++d)
                            out[sub][bits][phase] += ((bits >> d) & 1 ? 1. : -1.) * taps[phase][sub][d];
            return out;
        }();
        return shapes;
    }
    void acquire() {
        fit_calls++;
        pending = false;
        blocked_until = pending_end + 5;
        uint32_t best_aa = 0;
        bool any_known = false;
        for (unsigned k = 0; k < sync_count; k++)
            any_known |= syncs[k].aa == 0x8e89bed6 || registry.known(syncs[k].aa);
        // Fit only synchronization symbols. No header, payload or CRC enters
        // the choice of timing. Fit DC and pulse amplitude jointly so an
        // unbalanced address does not bias the carrier offset.
        double best = 0, best_t = 0, best_offset = 0, best_deviation = 0;
        for (unsigned candidate = 0; candidate < sync_count; candidate++) {
            uint32_t aa = syncs[candidate].aa;
            bool known = aa == 0x8e89bed6 || registry.known(aa);
            if (any_known && !known)
                continue;
            // A shifted/noisy reply can resemble an unrelated blind address
            // before its real AA has finished. Wait for the scheduled sync
            // evidence rather than starting a speculative payload over it.
            if (!known && reply_start != UINT64_MAX &&
                syncs[candidate].end + 64 >= reply_start &&
                samples < reply_start + 67)
                continue;
            // Use amplitude as a reliability weight for established data links.
            // Cache per observation: every timing fits the same 56 samples.
            std::array<double, 56> weights;
            weights.fill(1.0);
            double weight_sum = 56;
            if (known && aa != 0x8e89bed6) {
                weight_sum = 0;
                for (unsigned n = 0; n < weights.size(); ++n) {
                    unsigned at = (syncs[candidate].end - 58 + n) & 127;
                    weights[n] = std::sqrt(std::hypot(dx[at], dy[at]) + 1e-9);
                    weight_sum += weights[n];
                }
            }
            // Observations and their weighted moments are invariant across
            // timing hypotheses. Each phase still sees the same input samples.
            std::array<double, 56> observed;
            double sy = 0, syy = 0;
            for (unsigned n = 0; n < observed.size(); ++n) {
                double y = sample_angle(syncs[candidate].end - 58 + n);
                observed[n] = y;
                sy += weights[n] * y;
                syy += weights[n] * y * y;
            }
            // Fit two timing phases per NEON vector, keeping each phase's
            // observation order. The tenth lane is padding, not a new timing.
            std::array<double, 10> sums{}, squares{}, products{};
            const auto &shapes = sync_shapes();
#if defined(__aarch64__)
            // Keep all ten timing lanes in registers. Each observation and
            // weight is loaded once, and each lane retains its original FMA
            // order across the 56 observations.
            float64x2_t sx[5]{}, sxx[5]{}, sxy[5]{};
            for (unsigned n = 0; n < 56; ++n) {
                const double *shape = shapes[n & 1][(aa >> (n / 2)) & 31].data();
                const double weight = weights[n], y = observed[n];
#if defined(__clang__)
#pragma clang loop unroll(full)
#elif defined(__GNUC__)
#pragma GCC unroll 5
#endif
                for (unsigned lane = 0; lane < 5; ++lane) {
                    auto x = vld1q_f64(shape + 2 * lane);
                    auto wx = vmulq_n_f64(x, weight);
                    sx[lane] = vfmaq_n_f64(sx[lane], x, weight);
                    sxx[lane] = vfmaq_f64(sxx[lane], wx, x);
                    sxy[lane] = vfmaq_n_f64(sxy[lane], wx, y);
                }
            }
            for (unsigned lane = 0; lane < 5; ++lane) {
                vst1q_f64(sums.data() + 2 * lane, sx[lane]);
                vst1q_f64(squares.data() + 2 * lane, sxx[lane]);
                vst1q_f64(products.data() + 2 * lane, sxy[lane]);
            }
#else
            for (unsigned lane = 0; lane < 10; ++lane)
                for (unsigned n = 0; n < 56; ++n) {
                    double x = shapes[n & 1][(aa >> (n / 2)) & 31][lane];
                    double wx = weights[n] * x;
                    sums[lane] += wx;
                    squares[lane] += wx * x;
                    products[lane] += wx * observed[n];
                }
#endif
#if defined(__aarch64__)
            std::array<float, 68> symbol_angles{};
            bool angles_ready = false;
#endif
            for (int phase = -4; phase <= 4; phase++) {
                double t = double(syncs[candidate].end) + phase * .25;
                if (t + .5 > samples)
                    continue;
                // Compare a shifted GFSK template against the SAME fixed input
                // samples at every timing. Interpolating observations for the fit
                // would change their noise variance and favor the wrong phase.
                double sx = sums[phase + 4], sxx = squares[phase + 4], sxy = products[phase + 4];
                double xx = sxx - sx * sx / weight_sum, xy = sxy - sx * sy / weight_sum;
                double gain = xy / xx, offset = (sy - gain * sx) / weight_sum, span = 2 * gain;
                if (span < .45 || span > 2.8 || std::abs(offset) > .65)
                    continue;
                unsigned errors = 0;
#if defined(__aarch64__)
                if (!angles_ready) {
                    const uint64_t begin = syncs[candidate].end - 64;
                    const unsigned count = unsigned(std::min(uint64_t(67), samples - begin + 1));
                    for (unsigned k = 0; k < count; ++k) symbol_angles[k] = sample_angle(begin + k);
                    angles_ready = true;
                }
                errors = StreamingVector::address_errors(symbol_angles.data(), phase, offset, aa);
#else
                for (unsigned k = 0; k < 32; k++)
                    errors += (symbol(t - 62 + 2 * k) > offset) != bool((aa >> k) & 1);
#endif
                // Only a timed hint from a CRC-valid predecessor gets the
                // larger sync error budget. Payload/CRC validation is unchanged.
                if (errors > (syncs[candidate].timed ? 6u : known ? 2u : 0u))
                    continue;
                double explained = xy * xy / xx;
                double variance = std::max(0., syy - sy * sy / weight_sum - explained);
                double score = explained / (variance + .02 * weight_sum);
                if (!known) {
                    double ps = 0, pq = 0, pc = 0;
                    for (unsigned k = 0; k < 8; k++) {
                        double x = symbol(t - 78 + 2 * k) - offset;
                        double b = ((k & 1) ^ (aa & 1)) ? 1 : -1;
                        pc += x * b;
                        ps += x;
                        pq += x * x;
                    }
                    if (pc <= 0 || pc * pc < .8 * (8 * pq - ps * ps) || score < 3)
                        continue;
                    // For a blind address, only the known preamble may rank
                    // competing frame boundaries. Fitting guessed address bits
                    // otherwise rewards an earlier, shifted address by chance.
                    score = pc * pc / (8 * pq + .02);
                    // Packet history can disambiguate an alternating run that
                    // extends into an unknown address. This small prior only
                    // ranks acquisition; CRC-seed confirmation is unchanged.
                    if (registry.recently_observed(aa, samples)) score += .1;
                }
                if (score > best) {
                    best = score;
                    best_t = t;
                    best_offset = offset;
                    best_deviation = span / 2;
                    best_aa = aa;
                }
            }
        }
        if (best == 0)
            return;
        uint32_t aa = best_aa;
        auto ctx = aa == 0x8e89bed6 ? std::nullopt : registry.known(aa);
        bool known = aa == 0x8e89bed6 || ctx;
        frame.start = uint64_t(std::llround(best_t - 63));
        frame.offset = best_offset;
        // Snapshot the last 32 us of committed non-packet power at sync time.
        // Delayed learning keeps this packet's preamble/address out of it.
        double noise_power = noise.measured_power();
        frame.noise_dbfs = std::isfinite(noise_power) && noise_power > 0
                               ? 10 * std::log10(noise_power / (8192. * 8192.)) : NAN;
        frame.bits = 32;
        frame.target = 0;
        frame.aa = aa;
        frame.known = known;
        frame.init = aa == 0x8e89bed6 ? 0xaaaaaa : ctx ? ctx->init : 0;
        frame.crc = frame.init;
        frame.whitening = unsigned(channel) | 64;
        frame.energy_sample = uint64_t(std::floor(best_t + 1));
        frame.energy_start = energy_prefix[frame.energy_sample & 127];
        next_symbol = best_t + 2;
        period = 2;
        previous_symbol = symbol(best_t) - best_offset;
        deviation = best_deviation;
        metrics.fill(1e12);
        paths = {};
        trellis_steps = 0;
        unsigned last = (aa >> 31) & 1;
        metrics[last * 2] = metrics[last * 2 + 1] = 0;
        paths[last * 2] = 0;
        paths[last * 2 + 1] = 1;
        active = true;
        // Captured peripherals emit a settling carrier about 50 us before the
        // address. Reserve 64 us before AA so that carrier is not idle noise.
        noise.packet(frame.start > 128 ? frame.start - 128 : 0, samples);
        payload_starts++;
        preamble_syncs++;
    }
    [[gnu::noinline]] void prepare_reply_sync() {
        if (!active) {
            // Search a narrow AA timing neighborhood, using only sync
            // samples. Select one fit before any payload decoding.
            unsigned kept = 0;
            if (pending)
                for (unsigned n = 0; n < sync_count; ++n)
                    if (syncs[n].aa == 0x8e89bed6 || registry.known(syncs[n].aa))
                        syncs[kept++] = syncs[n];
            pending = true;
            pending_end = samples;
            sync_count = kept;
            for (int d = -4; d <= 4; ++d)
                syncs[sync_count++] = {uint64_t(int64_t(reply_start + 63) + d), reply_aa, true};
        }
        reply_start = reply_due = UINT64_MAX;
    }

    void track_and_consume() {
        trellis_updates++;
        double t = next_symbol;
        double value = symbol(next_symbol);
        double centered = value - frame.offset;
        double middle = symbol(next_symbol - 1) - frame.offset;
        // Bounded Gardner feedback: update once per symbol, never resample an
        // already-decided symbol. Only transitions carry timing information.
        double error = 0;
        if ((centered > 0) != (previous_symbol > 0))
            error = std::clamp(
                (previous_symbol - centered) * middle / (deviation * deviation + .01), -.5, .5);
        period = std::clamp(period + .00001 * error, 1.9996, 2.0004);
        next_symbol += period + .01 * error;
        previous_symbol = centered;
        // Four-state sequence detector for the Gaussian pulse's adjacent-bit
        // memory. One trellis update per symbol; a fixed eight-symbol decision
        // delay, no saved payload samples and no CRC-directed alternatives.
        double obs[2] = {interpolate(t - .5) - frame.offset, interpolate(t + .5) - frame.offset};
        std::array<double, 4> next;
        next.fill(1e30);
        std::array<uint64_t, 4> trace{};
        // Pulse coefficients at +/- a quarter-symbol, integrated over the
        // half-symbol differential interval (same BT=.5 acquisition model).
        const auto &h = branch_taps()[unsigned((t - std::floor(t)) * 32) & 31];
        for (unsigned state = 0; state < 4; state++)
            for (unsigned bit = 0; bit < 2; bit++) {
                double a = (state >> 1) ? 1 : -1, b = (state & 1) ? 1 : -1, c = bit ? 1 : -1;
                double metric = metrics[state];
                for (unsigned j = 0; j < 2; j++) {
                    double expected = deviation * (h[j][0] * a + h[j][1] * b + h[j][2] * c);
                    double err = obs[j] - expected;
                    metric += err * err;
                }
                unsigned dst = ((state & 1) << 1) | bit;
                if (metric < next[dst]) {
                    next[dst] = metric;
                    trace[dst] = (paths[state] << 1) | bit;
                }
            }
        unsigned best = unsigned(std::min_element(next.begin(), next.end()) - next.begin());
        double min = next[best];
        for (auto &m : next)
            m -= min;
        metrics = next;
        paths = trace;
        if (++trellis_steps >= 8) {
            bool bit = (paths[best] >> 8) & 1;
            decision_sample = uint64_t(std::floor(t - 14 + 1));
            active = consume(frame, bit);
        }
        if (!active) {
            // Search starts after this packet; failed CRCs cannot re-arm it.
            blocked_until = samples + 64;
            raw_word[0] = raw_word[1] = smooth_word[0] = smooth_word[1] = 0;
            corrected_word[0] = corrected_word[1] = 0;
        }
    }

  public:
    uint64_t payload_starts = 0, payload_symbols = 0, fit_calls = 0;
    uint64_t trellis_updates = 0, discriminator_evaluations = 0;
    StreamingDecoder(int ch, Registry &r, std::function<void(const Packet &)> fn,
                     bool report_failures = false)
        : registry(r), emit(std::move(fn)), channel(ch), report_crc_failures(report_failures) {}
    void reset() {
        frame = {};
        coarse[0] = {};
        coarse[1] = {};
        prev_i = prev_q = 0;
        energy = power = 0;
        noise.reset();
        raw_word[0] = raw_word[1] = smooth_word[0] = smooth_word[1] = 0;
        corrected_word[0] = corrected_word[1] = 0;
        fm = {};
        dx = {};
        dy = {};
        fm_tag = {};
        energy_prefix = {};
        since_reset = 0;
        pending = active = false;
        coarse_end = blocked_until = 0;
        last_start = UINT64_MAX;
        reply_start = reply_due = UINT64_MAX;
    }
    // Prepare independent arithmetic in small vector batches; packet state and
    // noise learning still advance once, in sample order.
    void run_block(const float *iq, size_t n, size_t stride = 2) {
        if (stride != 2) {
            for (size_t k = 0; k < n; ++k) run(iq[k * stride], iq[k * stride + 1]);
            return;
        }
        StreamingVector work;
        while (n) {
            size_t take = std::min(n, StreamingVector::capacity);
            work.prepare(iq, take, prev_i, prev_q);
            for (size_t k = 0; k < take; ++k)
                advance(work.energy[k], work.real[k], work.imag[k]);
            prev_i = iq[2 * take - 2];
            prev_q = iq[2 * take - 1];
            iq += 2 * take;
            n -= take;
        }
    }
    [[gnu::always_inline]] void run(float i, float q) {
        advance(double(i) * i + double(q) * q,
                prev_i * i + prev_q * q, prev_i * q - prev_q * i);
        prev_i = i;
        prev_q = q;
    }
  private:
    [[gnu::always_inline]] void advance(double e, float dot, float cross) {
        energy += e;
        energy_prefix[samples & 127] = energy;
        power += .125 * (e - power);
        bool was_active = active;
        if (samples >= reply_due)
            prepare_reply_sync();
        dx[samples & 127] = dot;
        dy[samples & 127] = cross;
        fm_tag[samples & 127] = 0;
        unsigned k = samples & 1;
        if (active) {
            if (double(samples) >= next_symbol + 1.5)
                track_and_consume();
        } else {
            raw_word[k] = (raw_word[k] >> 1) | (uint64_t(cross > 0) << 39);
            detect(raw_word[k], samples);
            // The sync discriminator is computed only while acquiring a
            // burst. Its short history is cached; no phase increment is
            // evaluated twice, and no payload is replayed.
            if (since_reset >= 2 && (power > 1.5 * noise.background() || samples < coarse_end || pending)) {
                float smooth = .25f * (sample_angle(samples - 2) + 2 * sample_angle(samples - 1) +
                                       sample_angle(samples));
                correlate(smooth);
                smooth_word[k] = (smooth_word[k] >> 1) | (uint64_t(smooth > 0) << 39);
                corrected_word[k] =
                    (corrected_word[k] >> 1) |
                    (uint64_t(smooth > (samples < coarse_end ? coarse_offset : 0)) << 39);
                detect(smooth_word[k], samples ? samples - 1 : 0);
                // With zero/no effective CFO correction these words coincide.
                // The same word and end position cannot add another candidate.
                if (corrected_word[k] != smooth_word[k])
                    detect(corrected_word[k], samples ? samples - 1 : 0);
            } else {
                coarse[k] = {};
                smooth_word[k] = corrected_word[k] = 0;
            }
            // Allow the eight-symbol preamble ambiguity plus two samples
            // on either side to resolve before committing a payload state.
            if (pending && samples >= pending_end + 20)
                acquire();
        }
        noise.observe(e, samples, was_active || active);
        samples++;
        since_reset++;
    }
};
