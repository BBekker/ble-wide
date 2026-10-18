// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

// Packet-local frequency discriminator and fractional timing search. A
// preamble fit can supply timing/offset before an address is known. The caller
// validates CRCs or applies repeated-seed inference; no payload bits are repaired.
class SoftDemod {
  public:
    struct Result {
        std::vector<uint8_t> bytes;
        uint32_t crc = 0;
        double offset_hz = 0, timing = 0, energy = 0;
    };
    template <class Sample, class Check>
    [[gnu::noinline]] static bool recover(uint64_t last_aa, uint32_t aa, int channel, uint32_t seed,
                                          unsigned max_bytes, Sample sample, Check check,
                                          Result &result, double preamble_timing = NAN,
                                          double preamble_offset = 0, bool known = true) {
        // Index 66 is the final AA bit's hard-decision sample. Keep enough
        // context for interpolation around the first AA bit and packet end.
        const int count = 70 + int(max_bytes) * 16;
        std::array<float, 70 + 260 * 16> angle, energy;
        if (max_bytes > 260)
            return false;
        const uint64_t origin = last_aa - 66;
        auto previous = sample(origin - 1);
        int computed = 0;
        auto ensure = [&](int end) {
            while (computed <= end && computed < count) {
                auto x = sample(origin + computed);
                float cross = previous[0] * x[1] - previous[1] * x[0];
                float dot = previous[0] * x[0] + previous[1] * x[1];
                angle[computed] = std::atan2(cross, dot);
                energy[computed] = x[0] * x[0] + x[1] * x[1];
                previous = x;
                computed++;
            }
        };
        auto interpolate = [&](double t) {
            int k = int(std::floor(t));
            if (k < 0 || k + 1 >= count)
                return 0.0;
            ensure(k + 1);
            double f = t - k;
            return angle[k] * (1 - f) + angle[k + 1] * f;
        };
        // A one-symbol rectangular integration of the FM discriminator.
        auto symbol = [&](double t) { return .5 * (interpolate(t - .5) + interpolate(t + .5)); };
        struct Fit {
            double timing, offset, score;
        };
        std::vector<Fit> fits;
        if (std::isfinite(preamble_timing))
            fits.push_back({preamble_timing, preamble_offset, 1e9});
        for (int phase = -4; known && phase <= 4; phase++) {
            double timing = phase * .25, sum[2]{}, sq[2]{};
            unsigned n[2]{};
            for (int bit = 0; bit < 32; bit++) {
                unsigned b = (aa >> bit) & 1;
                double x = symbol(4 + 2 * bit + timing);
                sum[b] += x;
                sq[b] += x * x;
                n[b]++;
            }
            if (!n[0] || !n[1])
                continue;
            double low = sum[0] / n[0], high = sum[1] / n[1], offset = .5 * (high + low);
            double variance = (sq[0] - sum[0] * low + sq[1] - sum[1] * high) / 32;
            if (high - low < .45 || high - low > 2.8 || std::abs(offset) > .65)
                continue;
            unsigned errors = 0;
            for (int bit = 0; bit < 32; bit++)
                errors += (symbol(4 + 2 * bit + timing) > offset) != bool((aa >> bit) & 1);
            if (errors > 2)
                continue;
            fits.push_back({timing, offset, (high - low) * (high - low) / (variance + .02)});
        }
        std::sort(fits.begin(), fits.end(), [](auto a, auto b) { return a.score > b.score; });
        // Timing/offset come from the preamble or known AA, never from CRC bits.
        // Trying nearby timings makes packet recovery less dependent on one
        // noisy synchronization estimate. Clock skew is bounded to +/-100 ppm.
        for (auto fit : fits)
            for (double ppm : {0., -100., 100.}) {
                if (!known && ppm != 0)
                    continue;
                std::vector<uint8_t> bytes;
                unsigned whitening = unsigned(channel) | 64, target = 2;
                for (unsigned byte = 0; byte < target; byte++) {
                    unsigned value = 0;
                    for (unsigned bit = 0; bit < 8; bit++) {
                        unsigned position = byte * 8 + bit;
                        double t = 68 + 2 * position + fit.timing + 2 * position * ppm * 1e-6;
                        unsigned b = (symbol(t) > fit.offset) ^ (whitening & 1);
                        whitening = (whitening >> 1) ^ ((whitening & 1) ? 0x44 : 0);
                        value |= b << bit;
                    }
                    bytes.push_back(uint8_t(value));
                    if (byte == 1) {
                        unsigned len = bytes[1], type = bytes[0] & 15;
                        bool good;
                        if (aa == 0x8e89bed6)
                            good = type == 7
                                       ? len >= 1
                                       : ((type == 0 || type == 2 || type == 4 || type == 6)
                                              ? len >= 6 && len <= 37
                                          : (type == 1 || type == 3) ? len == 12
                                                                     : type == 5 && len == 34);
                        else
                            good = (bytes[0] & 3) != 0 && !(bytes[0] & 0xe0) && len <= 251;
                        if (!good || len + 5 > max_bytes)
                            break;
                        target = len + 5;
                    }
                }
                if (bytes.size() != target || target <= 2)
                    continue;
                size_t n = bytes.size() - 3;
                uint32_t crc =
                    bytes[n] | uint32_t(bytes[n + 1]) << 8 | uint32_t(bytes[n + 2]) << 16;
                if (!check(bytes.data(), n, seed, crc))
                    continue;
                result.bytes.assign(bytes.begin(), bytes.begin() + n);
                result.crc = crc;
                result.offset_hz = fit.offset * 2e6 / (2 * 3.14159265358979323846);
                result.timing = fit.timing;
                result.energy = 0;
                ensure(68 + target * 16 - 1);
                for (unsigned k = 68; k < 68 + target * 16; k++)
                    result.energy += energy[k];
                result.energy /= target * 16;
                return true;
            }
        return false;
    }
};
