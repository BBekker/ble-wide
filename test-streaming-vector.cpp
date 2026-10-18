// SPDX-License-Identifier: MIT
#include "demod-fixture.h"
#include "streaming-demod.h"
#include <tuple>

int main() {
    // Vector differential products must retain the scalar ARM FMA order,
    // including near cancellation, silence, short blocks and the scalar tail.
    std::mt19937 rng(91);
#if defined(__aarch64__)
    // Compare the vector address decisions with the actual scalar symbol
    // interpolation formula, including exact-threshold ties at every phase.
    for (unsigned trial = 0; trial < 10000; ++trial) {
        float angles[68];
        for (auto &x : angles) x = float(int(rng() % 628319) - 314159) / 100000;
        if (trial % 7 == 0) std::fill(std::begin(angles), std::end(angles), 0);
        uint32_t aa = rng();
        auto interpolate = [&](double at) {
            unsigned n = unsigned(at);
            float fraction = float(at - n), a = angles[n];
            return fraction == 0 ? a : a + fraction * (angles[n+1] - a);
        };
        auto symbol = [&](double at) { return .5 * (interpolate(at-.5) + interpolate(at+.5)); };
        for (int phase = -4; phase <= 4; ++phase) {
            double end = 64 + phase * .25;
            double offset = trial & 1 ? symbol(end-62+2*(trial%32))
                                      : (int(rng()%130001)-65000)/100000.;
            unsigned expected = 0;
            for (unsigned k = 0; k < 32; ++k)
                expected += (symbol(end-62+2*k) > offset) != bool((aa>>k)&1);
            assert(StreamingVector::address_errors(angles,phase,offset,aa) == expected);
        }
    }
#endif
    for (unsigned trial = 0; trial < 10000; ++trial) {
        float iq[128];
        for (auto &x : iq)
            x = std::ldexp(float(int(rng() % 200000) - 100000), -int(rng() % 30));
        if (trial % 7 == 0) std::fill(std::begin(iq), std::end(iq), 0);
        float pi = 13.7f, pq = -27.3f;
        StreamingVector v;
        size_t n = 1 + rng() % StreamingVector::capacity;
        v.prepare(iq, n, pi, pq);
        for (size_t k = 0; k < n; ++k) {
            float i = iq[2*k], q = iq[2*k+1];
            assert(v.energy[k] == double(i)*i + double(q)*q);
#if defined(__aarch64__)
            assert(v.real[k] == std::fma(pi, i, pq*q));
            assert(v.imag[k] == std::fma(pi, q, -(pq*i)));
#else
            assert(v.real[k] == pi*i + pq*q);
            assert(v.imag[k] == pi*q - pq*i);
#endif
            pi = i; pq = q;
        }
    }
    using Key = std::tuple<uint64_t, uint32_t, std::vector<uint8_t>, uint32_t,
                           bool, double, double, double>;
    for (unsigned seed = 0; seed < 12; ++seed) {
        bool data = seed & 1;
        uint32_t aa = data ? 0x12345679 : 0x8e89bed6;
        auto pdu = data ? std::vector<uint8_t>{2, 4, 1, 2, 3, 4}
                        : std::vector<uint8_t>{0, 6, 1, 2, 3, 4, 5, 6};
        auto iq = waveform(pdu, 60000, seed / 12., seed * 3., seed,
                           seed % 3 == 0, aa, data ? 0x654321 : 0xaaaaaa, data ? 0 : 37);
        for (size_t stride : {size_t(2), size_t(80)})
            for (size_t chunk : {size_t(1), size_t(3), size_t(63), size_t(64), size_t(65), size_t(4096)}) {
                Registry a, b;
                if (data) { a.remember(aa, {0x654321,3,0,true}); b.remember(aa, {0x654321,3,0,true}); }
                std::vector<Key> expected, got;
                auto collect = [](std::vector<Key> &out, const Packet &p) {
                    out.emplace_back(p.sample, p.aa, p.pdu, p.crc, p.crc_valid,
                                     p.signal_dbfs, std::isnan(p.noise_dbfs) ? INFINITY : p.noise_dbfs, p.frequency_offset_hz);
                };
                StreamingDecoder scalar(data ? 0 : 37, a, [&](const Packet &p) { collect(expected,p); }, true);
                StreamingDecoder vector(data ? 0 : 37, b, [&](const Packet &p) { collect(got,p); }, true);
                std::vector<float> buffer(iq.size() * stride);
                for (size_t k = 0; k < iq.size(); ++k) {
                    buffer[k*stride] = iq[k][0]; buffer[k*stride+1] = iq[k][1];
                }
                for (unsigned epoch = 0; epoch < 2; ++epoch) {
                    scalar.reset(); vector.reset();
                    for (auto x : iq) scalar.run(x[0], x[1]);
                    for (size_t at = 0; at < iq.size(); at += chunk)
                        vector.run_block(buffer.data() + at*stride, std::min(chunk, iq.size()-at), stride);
                    assert(expected == got);
                    assert(scalar.payload_starts == vector.payload_starts);
                    assert(scalar.crc_fail == vector.crc_fail);
                    assert(scalar.discriminator_evaluations == vector.discriminator_evaluations);
                }
            }
    }
    std::cout << "vector arithmetic, packet bytes, measurements, tails, strides and reset passed\n";
}
