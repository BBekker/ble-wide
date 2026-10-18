// SPDX-License-Identifier: MIT
#include "demod-fixture.h"
#include "streaming-demod.h"

static unsigned receive(const std::vector<std::array<float, 2>> &iq,
                        const std::vector<uint8_t> &expected, uint32_t aa = 0x8e89bed6,
                        int channel = 37, int gap = -1) {
    Registry registry;
    if (aa != 0x8e89bed6)
        registry.remember(aa, {0x654321, 3, 0, true});
    unsigned found = 0, now = 0;
    StreamingDecoder d(channel, registry, [&](const Packet &p) {
        assert(p.aa == aa && p.pdu == expected);
        assert(std::string(p.demod).find("streaming") == 0);
        // Publication at the actual CRC, without a maximum-packet-length wait.
        assert(now < 4000 + (8 + 32 + (expected.size() + 3) * 8) * 2 + 40);
        assert(p.sample >= 4017 && p.sample <= 4022);
        found++;
    });
    for (auto x : iq) {
        if (int(now) == gap)
            d.reset();
        d.run(x[0], x[1]);
        now++;
    }
    assert(found <= 1);
    assert(d.recovery_attempts == 0);
    return found;
}
int main() {
    // Diagnostic publication retains a failed frame but never accepts it or
    // changes the learned connection. Default callers still receive only valid packets.
    for (bool report : {false, true}) {
        Registry r;
        const uint32_t aa = 0x12345679;
        r.remember(aa, {0x654321, 3, 0, true});
        unsigned emitted = 0;
        const std::vector<uint8_t> bytes = {3, 2, 2, 0}; // damaged terminate indication
        StreamingDecoder d(0, r, [&](const Packet &p) {
            assert(!p.crc_valid && p.expected_crc != p.crc);
            assert(p.pdu == bytes && p.aa == aa && std::isfinite(p.signal_dbfs));
            ++emitted;
        }, report);
        for (auto x : waveform(bytes, 0, .25, 0, 19, true, aa, 0x654321, 0))
            d.run(x[0], x[1]);
        assert(emitted == unsigned(report) && d.crc_fail == 1 && d.valid == 0);
        assert(d.payload_starts == 1 && d.recovery_attempts == 0);
        assert(r.known(aa)->init == 0x654321);
    }
#ifdef STREAMING_AUDIT
    for(int k=0;k<=100000;k++) {
        double a=-3.14159265358979323846 + k*(6.283185307179586/100000);
        float x=std::cos(a), y=std::sin(a);
        assert(std::abs(StreamingDecoder::test_angle(y,x)-std::atan2(y,x))<.00002);
    }
    assert(StreamingDecoder::test_angle(0,0)==0);
#endif
    // Exactly one decoder invocation and one dewhitening/CRC step per payload
    // bit, including packets with deliberately invalid CRC. No retry is hidden
    // behind a successful final emission or an output deduplication check.
    for(bool corrupt:{false,true}) {
        Registry r; unsigned emitted=0;
        std::vector<uint8_t> bytes={0,6,1,2,3,4,5,6};
        StreamingDecoder d(37,r,[&](const Packet &p){assert(p.pdu==bytes);emitted++;});
        auto iq=waveform(bytes,100000,.37,0,123,corrupt);
        for(auto x:iq)d.run(x[0],x[1]);
        assert(d.payload_starts==1);
        assert(d.payload_symbols==(bytes.size()+3)*8);
        assert(d.trellis_updates==d.payload_symbols+7);
        assert(d.discriminator_evaluations<=iq.size());
        assert(d.recovery_attempts==0 && d.crc_fail==unsigned(corrupt));
        assert(emitted==unsigned(!corrupt));
    }

    // Packet power must not leak into the frozen pre-sync background value.
    {
        Registry r; unsigned emitted = 0;
        StreamingDecoder d(37, r, [&](const Packet &p) {
            assert(std::isfinite(p.noise_dbfs));
            const double expected = 10 * std::log10(200. / (8192. * 8192.));
            assert(std::abs(p.noise_dbfs - expected) < 2);
            emitted++;
        });
        for (auto x : waveform({0, 6, 1, 2, 3, 4, 5, 6}, 0, .3, 10, 77))
            d.run(x[0], x[1]);
        assert(emitted == 1);
    }

    // Weak address samples must not dominate timing/CFO selection. These
    // seeded cases include a gain step within the AA, followed by one payload.
    for (unsigned case_id = 0; case_id < 3; ++case_id) {
        const uint32_t aa = 0x50657b98, crc_init = reverse24(0x4d4e41);
        const unsigned timing = case_id ? 0 : 5;
        const unsigned step_at = case_id ? 4032 : 4056;
        const double step_db = case_id * 6.;
        const std::vector<uint8_t> bytes = {5, 0};
        auto iq = waveform(bytes, 0, timing / 10., 0, 1, false, aa, crc_init, 16);
        std::mt19937 rng(999 + timing + step_at);
        std::normal_distribution<float> noise(0, 100 / std::sqrt(2 * std::pow(10, .6)));
        Registry r;
        r.remember(aa, {crc_init, 3, 0, true});
        unsigned emitted = 0;
        StreamingDecoder d(16, r, [&](const Packet &p) {
            assert(p.aa == aa && p.pdu == bytes);
            ++emitted;
        });
        for (size_t k = 0; k < iq.size(); ++k) {
            double scale = k < step_at ? std::pow(10, -step_db / 20) : 1;
            float i = iq[k][0] * scale + noise(rng);
            float q = iq[k][1] * scale + noise(rng);
            d.run(i, q);
        }
        assert(emitted == 1 && d.payload_starts == 1 && d.recovery_attempts == 0);
    }

    std::vector<uint8_t> pdu = {0x40, 20};
    for (int k = 0; k < 20; k++)
        pdu.push_back(uint8_t(17 * k + 23));
    unsigned found = 0;
    for (double cfo : {-140000., 0., 140000.})
        for (double phase : {0., .25, .5, .75})
            for (double noise : {0., 20., 35., 50.})
                for (unsigned seed : {19u, 83u, 277u}) {
                    auto iq = waveform(pdu, cfo, phase, noise, seed);
                    found += receive(iq, pdu);
                }
    std::cout << "Streaming noisy advertisements: " << found << "/144\n";
    unsigned clean = 0;
    for (uint32_t aa : {0x8e89bed6u, 0x12345679u})
        for (double cfo : {-140000., 0., 140000.})
            for (double timing : {0., .25, .5, .75}) {
                bool adv = aa == 0x8e89bed6;
                auto bytes = adv ? pdu : std::vector<uint8_t>{2, 3, 1, 2, 3};
                auto iq = waveform(bytes, cfo, timing, 0, 19, false, aa, adv ? 0xaaaaaa : 0x654321,
                                   adv ? 37 : 0);
                clean += receive(iq, bytes, aa, adv ? 37 : 0);
            }
    std::cout << "Streaming clean: " << clean << "/24" << std::endl;
    assert(clean == 24);
    for (unsigned seed = 0; seed < 20; seed++)
        assert(receive(waveform(pdu, 100000., .5, 20., seed, true), pdu) == 0);
    auto gap = waveform(pdu, 0, .5, 0, 19);
    assert(receive(gap, pdu, 0x8e89bed6, 37, 4100) == 0);
    // Every whitening channel and both access-address polarities, including
    // packet state reset between independent captures.
    for (int ch = 0; ch < 40; ch++) {
        std::vector<uint8_t> bytes = {2, 3, 1, 2, uint8_t(ch)};
        assert(receive(waveform(bytes, 70000., .25, 0., 19, false, 0x12345679, 0x654321, ch), bytes,
                       0x12345679, ch) == 1);
    }
    for (uint32_t aa : {0x12345678u, 0x50657397u}) {
    Registry unknown;
    unsigned confirmed = 0;
    StreamingDecoder blind(0, unknown, [&](const Packet &p) {
        assert(p.aa == aa && p.crc_init == reverse24(0x654321));
        assert(p.context == "repeated_crc_seed");
        confirmed++;
    });
    for (int repetition = 0; repetition < 3; repetition++) {
        std::vector<uint8_t> bytes = {2, 3, uint8_t(repetition), 2, 3};
        auto iq = waveform(bytes, 50000., .5, 0., 19, false, aa, 0x654321, 0);
        for (auto x : iq)
            blind.run(x[0], x[1]);
        assert(confirmed == (repetition == 2 ? 1u : 0u));
    }
    }
    unsigned known_data = 0;
    std::vector<uint8_t> data = {2, 40};
    for (int k = 0; k < 40; k++)
        data.push_back(uint8_t(31 * k + 7));
    for (unsigned seed = 0; seed < 20; seed++) {
        known_data +=
            receive(waveform(data, 120000., .25, 25., seed, false, 0x12345678, 0x654321, 0), data,
                    0x12345678, 0);
        assert(receive(waveform(data, 100000., .5, 20., seed, true, 0x12345678, 0x654321, 0), data,
                       0x12345678, 0) == 0);
    }
    std::cout << "Streaming noisy known data: " << known_data << "/20" << std::endl;
    std::vector<uint8_t> long_data = {2, 251};
    for (int k = 0; k < 251; k++)
        long_data.push_back(uint8_t(k * 73 + 19));
    unsigned long_found = 0;
    for (double skew : {-90., 0., 90.})
        long_found +=
            receive(waveform(long_data, 50000., .5, 5., 31, false, 0x12345678, 0x654321, 0, skew),
                    long_data, 0x12345678, 0);
    std::cout << "Streaming long: " << long_found << "/3" << std::endl;
    assert(long_found == 3);
    Registry registry;
    unsigned false_packets = 0;
    StreamingDecoder noise_decoder(38, registry, [&](const Packet &) { false_packets++; });
    std::mt19937 rng(155);
    std::normal_distribution<float> noise(0, 100);
    for (int k = 0; k < 2000000; k++)
        noise_decoder.run(noise(rng), noise(rng));
    assert(false_packets == 0);
}
