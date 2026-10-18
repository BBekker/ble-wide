#pragma once
// SPDX-License-Identifier: MIT
// Deterministic GFSK/AWGN fixture, generated independently of receiver timing.
#include "decoder/decoder.h"
#include <cassert>
#include <iostream>
#include <random>

static std::vector<std::array<float, 2>> waveform(const std::vector<uint8_t> &pdu, double offset_hz,
                                                  double timing, double noise, unsigned seed,
                                                  bool corrupt = false, uint32_t aa = 0x8e89bed6,
                                                  uint32_t crc_seed = 0xaaaaaa, int ch = 37,
                                                  double skew_ppm = 0) {
    constexpr double pi = 3.14159265358979323846;
    std::vector<int> bits;
    for (int k = 0; k < 8; k++)
        bits.push_back(((((aa & 1) ? 0x55 : 0xaa) >> k) & 1) ? 1 : -1);
    for (int k = 0; k < 32; k++)
        bits.push_back(((aa >> k) & 1) ? 1 : -1);
    auto bytes = pdu;
    uint32_t crc = crc_of(bytes.data(), bytes.size(), crc_seed) ^ (corrupt ? 1u : 0u);
    for (int k = 0; k < 3; k++)
        bytes.push_back((crc >> (k * 8)) & 255);
    unsigned w = unsigned(ch) | 64;
    for (auto b : bytes)
        for (int k = 0; k < 8; k++) {
            bits.push_back((((b >> k) & 1) ^ (w & 1)) ? 1 : -1);
            w = (w >> 1) ^ ((w & 1) ? 0x44 : 0);
        }
    // 32 samples/symbol; BT=0.5 Gaussian pulse shaping over +/-2 symbols.
    std::array<double, 129> h{};
    double sum = 0;
    for (int k = 0; k < 129; k++) {
        double t = (k - 64) / 32.;
        h[k] = std::exp(-2 * pi * pi * .25 * t * t / std::log(2.));
        sum += h[k];
    }
    for (auto &v : h)
        v /= sum;
    std::vector<std::array<double, 2>> high((bits.size() + 4) * 32);
    double phase = .123;
    for (size_t k = 0; k < high.size(); k++) {
        double f = 0;
        for (int j = 0; j < 129; j++) {
            int n = int(k) + j - 64 - 64;
            if (n >= 0 && n < int(bits.size() * 32))
                f += h[j] * bits[n / 32];
        }
        phase += 2 * pi * (offset_hz + 250000 * f) / 32e6;
        high[k] = {100 * cos(phase), 100 * sin(phase)};
    }
    std::mt19937 rng(seed);
    std::normal_distribution<float> normal(0, float(noise));
    std::vector<std::array<float, 2>> out(4000 + high.size() / 16 + 4600);
    for (auto &v : out)
        v = {normal(rng), normal(rng)};
    for (size_t n = 0; n < high.size() / 16 - 2; n++) {
        double k = n * 16. * (1 + skew_ppm * 1e-6) + timing * 16.;
        int at = int(k);
        double f = k - at;
        for (int q = 0; q < 2; q++)
            out[4000 + n][q] += float(high[at][q] * (1 - f) + high[at + 1][q] * f);
    }
    return out;
}
