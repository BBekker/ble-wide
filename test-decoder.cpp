// SPDX-License-Identifier: MIT
#include "decoder.h"
#include <cassert>
#include <iostream>
static void send(Decoder &d, int ch, uint32_t aa, const std::vector<uint8_t> &pdu, uint32_t init,
                 bool corrupt = false, double amplitude = 100) {
    for (int k = 0; k < 200; k++)
        d.run(0, 0);
    std::vector<unsigned> bits;
    for (int k = 0; k < 8; k++)
        bits.push_back((((aa & 1) ? 0x55 : 0xaa) >> k) & 1);
    for (int k = 0; k < 32; k++)
        bits.push_back((aa >> k) & 1);
    auto bytes = pdu;
    auto crc = crc_of(pdu.data(), pdu.size(), init);
    if (corrupt)
        crc ^= 1;
    for (int k = 0; k < 3; k++)
        bytes.push_back((crc >> (8 * k)) & 255);
    unsigned w = ch | 64;
    for (auto byte : bytes)
        for (int k = 0; k < 8; k++) {
            bits.push_back(((byte >> k) & 1) ^ (w & 1));
            w = (w >> 1) ^ ((w & 1) ? 0x44 : 0);
        }
    double angle = 0;
    for (auto b : bits)
        for (int k = 0; k < 2; k++) {
            angle += (b ? 1 : -1) * 3.14159265358979323846 / 4;
            d.run(amplitude * cos(angle), amplitude * sin(angle));
        }
    for (int k = 0; k < 20; k++)
        d.run(0, 0);
}
int main() {
    const std::vector<uint8_t> data = {1, 3, 11, 22, 33};
    for (uint32_t init : {0u, 1u, 0xaaaaaau, 0xffffffu, 0x123456u})
        assert(recover_init(data.data(), data.size(), crc_of(data.data(), data.size(), init)) ==
               init);
    for (int ch = 0; ch < 40; ch++) {
        Registry r;
        std::vector<Packet> packets;
        Decoder d(ch, r, [&](const Packet &p) { packets.push_back(p); });
        send(d, ch, 0x8e89bed6, {7, 1, 0}, 0xaaaaaa);
        assert(packets.size() == 1);
        assert(std::abs(packets.back().signal_dbfs - 20 * std::log10(100.0 / 8192)) < .001);
        send(d, ch, 0x8e89bed6, {7, 1, 0}, 0xaaaaaa, true);
        assert(packets.size() == 1);
        send(d, ch, 0x12345678, data, 0x654321);
        assert(packets.size() == 1);
        // A bad frame with the same AA must not erase prior seed votes.
        send(d, ch, 0x12345678, data, 0x654321, true);
        assert(packets.size() == 1);
        send(d, ch, 0x12345678, data, 0x654321);
        assert(packets.size() == 1);
        send(d, ch, 0x12345678, data, 0x654321);
        assert(packets.size() == 2);
        assert(packets.back().context == "repeated_crc_seed");
        send(d, ch, 0x12345678, data, 0x654321, true);
        assert(packets.size() == 2);
        send(d, ch, 0x12345678, data, 0x654321);
        assert(packets.size() == 3);
        d.reset();
        send(d, ch, 0x12345678, data, 0x654321);
        assert(packets.size() == 4);
    }
    Registry r;
    unsigned packets = 0;
    Decoder adv(37, r, [&](const Packet &) { packets++; });
    std::vector<uint8_t> connect(36);
    connect[0] = 5;
    connect[1] = 34;
    uint32_t aa = 0x12345678, init = 0x654321;
    for (int k = 0; k < 4; k++)
        connect[14 + k] = (aa >> (8 * k)) & 255;
    for (int k = 0; k < 3; k++)
        connect[18 + k] = (init >> (8 * k)) & 255;
    send(adv, 37, 0x8e89bed6, connect, 0xaaaaaa);
    assert(packets == 1);
    assert(r.known(aa) && r.known(aa)->observed);
    Decoder d(0, r, [&](const Packet &p) {
        assert(p.context == "connection_request");
        packets++;
    });
    send(d, 0, aa, data, reverse24(init));
    assert(packets == 2);
    Registry signal_registry;
    std::vector<double> levels;
    Decoder signal_decoder(37, signal_registry, [&](const Packet &p) { levels.push_back(p.signal_dbfs); });
    send(signal_decoder, 37, 0x8e89bed6, {7, 1, 0}, 0xaaaaaa, false, 100);
    send(signal_decoder, 37, 0x8e89bed6, {7, 1, 0}, 0xaaaaaa, false, 50);
    assert(levels.size() == 2 && std::abs(levels[0] - levels[1] - 20 * std::log10(2)) < .001);
    signal_decoder.reset();
    send(signal_decoder, 37, 0x8e89bed6, {7, 1, 0}, 0xaaaaaa, false, 50);
    assert(levels.size() == 3 && std::abs(levels[1] - levels[2]) < .001);
    std::cout << "all 40 whitening channels, CRC rejection, seed inference and "
                 "CONNECT_IND passed\n";
}
