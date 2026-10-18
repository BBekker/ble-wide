// SPDX-License-Identifier: MIT
#include "demod-fixture.h"
static unsigned run(const std::vector<std::array<float, 2>> &iq, bool adaptive,
                    const std::vector<uint8_t> &expected, bool known_data = false,
                    bool correlate = true, bool hard = true) {
    Registry registry;
    if (known_data)
        registry.remember(0x12345678, {0x654321, 3, 0, true});
    unsigned found = 0;
    Decoder d(
        known_data ? 0 : 37, registry,
        [&](const Packet &p) {
            assert(p.pdu == expected);
            found++;
        },
        adaptive, nullptr, 1, correlate, hard);
    for (auto x : iq)
        d.run(x[0], x[1]);
    assert(found <= 1);
    return found;
}
int main() {
    std::vector<uint8_t> pdu = {0x40, 20};
    for (int k = 0; k < 20; k++)
        pdu.push_back(uint8_t(17 * k + 23));
    unsigned old = 0, improved = 0, preamble_only = 0, previous = 0, trials = 0;
    std::vector<std::array<float, 2>> rescued;
    for (double cfo : {-140000., 0., 140000.})
        for (double phase : {0., .25, .5, .75})
            for (double noise : {0., 20., 35., 50.})
                for (unsigned seed : {19u, 83u, 277u}) {
                    auto iq = waveform(pdu, cfo, phase, noise, seed);
                    unsigned a = run(iq, false, pdu), b = run(iq, true, pdu);
                    preamble_only += run(iq, true, pdu, false, true, false);
                    previous += run(iq, true, pdu, false, false);
                    assert(b >= a);
                    if (b > a && rescued.empty())
                        rescued = iq;
                    old += a;
                    improved += b;
                    trials++;
                }
    assert(improved > old);
    assert(improved >= previous);
    assert(preamble_only > 0);
    // Acquisition must work without ANY hard-AA search, at both preamble
    // polarities and fractional offsets. This catches a one-symbol sync slip.
    for (uint32_t aa : {0x8e89bed6u, 0x12345679u})
        for (double cfo : {-140000., 0., 140000.})
            for (double timing : {0., .25, .5, .75}) {
                bool adv = aa == 0x8e89bed6;
                std::vector<uint8_t> bytes = adv ? pdu : std::vector<uint8_t>{2, 3, 1, 2, 3};
                auto iq = waveform(bytes, cfo, timing, 0, 19, false, aa, adv ? 0xaaaaaa : 0x654321,
                                   adv ? 37 : 0);
                Registry r;
                if (!adv)
                    r.remember(aa, {0x654321, 3, 0, true});
                unsigned packets = 0;
                Decoder d(
                    adv ? 37 : 0, r,
                    [&](const Packet &p) {
                        assert(p.pdu == bytes && p.aa == aa);
                        assert(std::string(p.demod) == "preamble");
                        assert(std::abs(p.frequency_offset_hz - cfo) < 1000);
                        packets++;
                    },
                    true, nullptr, 1, true, false);
                for (auto x : iq)
                    d.run(x[0], x[1]);
                assert(packets == 1);
            }
    // A constant carrier (including a carrier offset) is not an alternating
    // preamble. Reset must discard partial correlation windows.
    PreambleCorrelator correlator;
    for (uint64_t n = 0; n < 10000; n++)
        assert(correlator.push(n, .4f) == 0);
    correlator.reset();
    for (uint64_t n = 0; n < 12; n++)
        assert(correlator.push(n, (n / 2) & 1 ? 1.f : -1.f) == 0);
    correlator.reset();
    for (uint64_t n = 0; n < 12; n++)
        assert(correlator.push(n, (n / 2) & 1 ? 1.f : -1.f) == 0);

    assert(!rescued.empty());
    Registry gap_registry;
    unsigned across_gap = 0;
    Decoder interrupted(
        37, gap_registry, [&](const Packet &) { across_gap++; }, true, nullptr, 1, true);
    for (size_t k = 0; k < rescued.size(); k++) {
        if (k == 5000)
            interrupted.reset();
        interrupted.run(rescued[k][0], rescued[k][1]);
    }
    assert(across_gap == 0); // A discontinuity cancels queued recovery.

    // Deliberately wrong CRCs must remain rejected, including recovered paths.
    for (unsigned seed = 0; seed < 20; seed++) {
        auto iq = waveform(pdu, 100000., .5, 20., seed, true);
        assert(run(iq, true, pdu) == 0);
        assert(run(iq, true, pdu, false, true, false) == 0);
    }
    // The same recovery is available to connections with an established seed.
    std::vector<uint8_t> data = {2, 40};
    for (int k = 0; k < 40; k++)
        data.push_back(uint8_t(31 * k + 7));
    unsigned data_old = 0, data_new = 0;
    for (unsigned seed = 0; seed < 20; seed++) {
        auto iq = waveform(data, 120000., .25, 25., seed, false, 0x12345678, 0x654321, 0);
        data_old += run(iq, false, data, true);
        data_new += run(iq, true, data, true);
    }
    assert(data_new >= data_old && data_new > 0);
    for (unsigned seed = 0; seed < 20; seed++) {
        auto iq = waveform(data, 100000., .5, 20., seed, true, 0x12345678, 0x654321, 0);
        assert(run(iq, true, data, true, true, false) == 0);
    }
    // Unknown addresses need three consistent CRC-seed observations, even
    // when the preamble is perfect. Vary data to exercise independent CRCs.
    Registry unknown;
    unsigned unknown_packets = 0;
    Decoder blind(
        0, unknown,
        [&](const Packet &p) {
            assert(p.aa == 0x12345678 && p.crc_init == reverse24(0x654321));
            assert(p.context == "repeated_crc_seed");
            unknown_packets++;
        },
        true, nullptr, 1, true, false);
    for (int repetition = 0; repetition < 3; repetition++) {
        std::vector<uint8_t> bytes = {2, 3, uint8_t(repetition), 2, 3};
        auto iq = waveform(bytes, 50000., .5, 0., 19, false, 0x12345678, 0x654321, 0);
        for (auto x : iq)
            blind.run(x[0], x[1]);
        assert(unknown_packets == (repetition == 2 ? 1u : 0u));
    }
    std::vector<uint8_t> long_data = {2, 251};
    for (int k = 0; k < 251; k++)
        long_data.push_back(uint8_t(k * 73 + 19));
    for (double skew : {-90., 0., 90.}) {
        auto iq = waveform(long_data, 50000., .5, 5., 31, false, 0x12345678, 0x654321, 0, skew);
        assert(run(iq, true, long_data, true) == 1);
    }
    std::mt19937 rng(155);
    std::normal_distribution<float> noise(0, 100);
    Registry registry;
    unsigned false_packets = 0;
    Decoder d(38, registry, [&](const Packet &) { false_packets++; }, true, nullptr, 1, true);
    for (int k = 0; k < 2000000; k++)
        d.run(noise(rng), noise(rng));
    assert(false_packets == 0);
    std::cout << "GFSK trials " << trials << ": legacy " << old << ", adaptive " << improved
              << "; previous " << previous << "; preamble only " << preamble_only << "; known data "
              << data_old << " -> " << data_new << "; invalid CRC/noise/duplicate checks passed\n";
}
