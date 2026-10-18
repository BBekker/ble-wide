// SPDX-License-Identifier: MIT
#include "demod-fixture.h"
#include "decoder/streaming-demod.h"

int main() {
    const uint32_t aa = 0x50657b98, seed = reverse24(0x4d4e41);
    // Airtime depends on payload size. A hint must not constrain ordinary
    // reception when the next packet uses another IFS or another address.
    for (unsigned len : {0u, 27u, 251u})
        for (int gap : {140, 150, 160})
            for (bool other_address : {false, true}) {
                std::vector<uint8_t> pdu{2, uint8_t(len)};
                for (unsigned n = 0; n < len; n++) pdu.push_back(uint8_t(n * 31));
                std::vector<uint8_t> reply{5, 0};
                auto a = waveform(pdu, -7000, .3, 0, 1, false, aa, seed, 16);
                uint32_t second = other_address ? 0x12345678 : aa;
                auto b = waveform(reply, -25000, .6, 0, 2, false, second, seed, 16);
                size_t shift = (pdu.size() + 8) * 16 + gap * 2;
                Registry r;
                r.remember(aa, {seed, 3, 0, true});
                r.remember(second, {seed, 3, 0, true});
                unsigned count = 0;
                StreamingDecoder d(16, r, [&](const Packet &p) {
                    assert(p.pdu == (count == 0 ? pdu : reply));
                    assert(p.aa == (count == 0 ? aa : second));
                    count++;
                });
                for (size_t n = 0; n < a.size() + shift; n++) {
                    float i = n < a.size() ? a[n][0] * 10 : 0;
                    float q = n < a.size() ? a[n][1] * 10 : 0;
                    if (n >= shift && n - shift < b.size()) {
                        i += b[n - shift][0]; q += b[n - shift][1];
                    }
                    d.run(i, q);
                }
                if (count != 2 || d.payload_starts != 2) std::cerr << "len " << len << " gap " << gap << " other " << other_address << " count " << count << " starts " << d.payload_starts << std::endl;
                assert(count == 2 && d.payload_starts == 2 && d.recovery_attempts == 0);
                assert(d.payload_symbols == (pdu.size() + reply.size() + 6) * 8);
            }
    // Corrupting one upper AA bit must not create an alias connection. The
    // low-16 membership filter collides deliberately with the real address.
    for (unsigned bit = 16; bit < 32; ++bit) {
        Registry r; r.remember(aa, {seed, 3, 0, true});
        unsigned count = 0;
        StreamingDecoder d(16, r, [&](const Packet &p) {
            assert(p.aa == aa && p.pdu == std::vector<uint8_t>({5, 0})); count++;
        });
        auto iq = waveform({5, 0}, -25000, .3, 0, 1, false,
                           aa ^ (1u << bit), seed, 16);
        for (auto x : iq) d.run(x[0], x[1]);
        assert(count == 1 && !r.known(aa ^ (1u << bit)));
    }
    // A previous valid packet must not manufacture a response: exercise
    // absent replies, wrong CRCs, and discontinuities before the reply.
    for (int mode = 0; mode < 3; mode++) {
        for (unsigned trial = 0; trial < 200; trial++) {
            auto a = waveform({1, 0}, -7000, .3, 0, 1, false, aa, seed, 16);
            auto b = waveform({5, 0}, -25000, (trial % 10) / 10., 0, 2,
                              true, aa, seed, 16);
            Registry r; r.remember(aa, {seed, 3, 0, true});
            unsigned count = 0;
            StreamingDecoder d(16, r, [&](const Packet &p) {
                assert(p.pdu == std::vector<uint8_t>({1, 0})); count++;
            });
            std::mt19937 rng(trial + 400);
            std::normal_distribution<float> noise(0, 25);
            for (size_t n = 0; n < a.size(); n++) {
                if (mode == 2 && n == 4300) d.reset();
                float i = noise(rng) + a[n][0] * 10;
                float q = noise(rng) + a[n][1] * 10;
                if (mode == 1 && n >= 460) {
                    i += b[n - 460][0]; q += b[n - 460][1];
                }
                d.run(i, q);
            }
            assert(count == 1 && d.recovery_attempts == 0);
        }
    }
    std::cout << "Reply sync: variable airtime, alternate timings/addresses, single decode, absent/corrupt replies and resets passed\n";
}
