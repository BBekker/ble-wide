// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

// Eight alternating symbols, maintained independently at both sample phases.
// The DC-removed normalized correlation retains confidence and rejects a
// constant carrier. This is a candidate generator, never packet validation.
class PreambleCorrelator {
    struct Phase {
        std::array<float, 8> values{};
        double sum = 0, square = 0, alternating = 0;
        unsigned index = 0, count = 0;
    } phases[2];

  public:
    void reset() {
        for (auto &p : phases)
            p = Phase{};
    }
    double push(uint64_t sample, float value) {
        auto &p = phases[sample & 1];
        float old = p.values[p.index];
        p.values[p.index] = value;
        p.index = (p.index + 1) & 7;
        p.sum += double(value) - old;
        p.square += double(value) * value - double(old) * old;
        p.alternating = value - p.alternating - old;
        if (p.count < 8) {
            p.count++;
            if (p.count < 8)
                return 0;
        }
        double variance = 8 * p.square - p.sum * p.sum;
        double correlation = p.alternating * p.alternating;
        if (variance < .05 || correlation < .85 * variance)
            return 0;
        return correlation / variance;
    }
    struct Sync {
        uint32_t aa = 0;
        double timing = 0, offset = 0, score = 0;
        bool first_bit = false;
    };
    template <class Sample>
    static void synchronize(uint64_t end, Sample sample, std::array<Sync, 3> &fits) {
        // Include the full preamble and access address plus interpolation guards.
        // end is the candidate's final preamble discriminator sample.
        std::array<float, 88> angle;
        auto previous = sample(end - 22);
        for (int k = 0; k < 88; k++) {
            auto x = sample(end - 21 + k);
            angle[k] = std::atan2(previous[0] * x[1] - previous[1] * x[0],
                                  previous[0] * x[0] + previous[1] * x[1]);
            previous = x;
        }
        auto interpolate = [&](double t) {
            int i = int(t);
            double f = t - i;
            return angle[i] * (1 - f) + angle[i + 1] * f;
        };
        auto symbol = [&](double t) { return .5 * (interpolate(t - .5) + interpolate(t + .5)); };
        for (unsigned boundary = 0; boundary < 3; boundary++) {
            constexpr unsigned back[] = {2, 0, 4};
            auto &best = fits[boundary];
            double shift = 4 - back[boundary];
            for (int phase = -4; phase <= 4; phase++) {
                double timing = phase * .25, sum[2]{}, sq[2]{};
                // Interior six symbols avoid packet onset and the AA boundary.
                for (int b = 1; b < 7; b++) {
                    double x = symbol(3 + 2 * b + timing + shift);
                    sum[b & 1] += x;
                    sq[b & 1] += x * x;
                }
                double low = sum[0] / 3, high = sum[1] / 3;
                double separation = high - low, offset = (high + low) * .5;
                double variance =
                    std::max(0., (sq[0] + sq[1]) / 6 - (low * low + high * high) * .5);
                double score = separation * separation / (variance + .02);
                if (std::abs(separation) < .4 || std::abs(separation) > 2.8 ||
                    std::abs(offset) > .65 || score < 8)
                    continue;
                if (score <= best.score)
                    continue;
                uint32_t aa = 0;
                for (int b = 0; b < 32; b++)
                    aa |= uint32_t(symbol(19 + 2 * b + timing + shift) > offset) << b;
                best = {aa, timing, offset, score, separation < 0};
            }
        }
    }
};
