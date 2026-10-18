// SPDX-License-Identifier: MIT
#define W1700K_PORTABLE_FFT
#include "channelizer/channelizer.h"
#include "decoder/decoder.h"
#include <emscripten.h>
#include <cstdio>
#include <memory>

namespace {
constexpr unsigned chunk = 163840, prefix = 1280;
alignas(64) int16_t iq[(chunk + prefix) * 2];
alignas(64) float bins[chunk]; // frames * 40 channels * complex float
std::unique_ptr<Channelizer> bank;
Registry registry;
std::vector<std::unique_ptr<Decoder>> decoders;
uint64_t samples = 0, packets = 0;
std::string status;
EM_JS(void, packet_js, (int ch, double sample, uint32_t aa, uint32_t crc, uint32_t init,
                       const char *context, const char *pdu), {
    globalThis.blePacket({type:'packet', channel:ch, sample, access_address:aa.toString(16).padStart(8,'0'),
        crc:crc.toString(16).padStart(6,'0'), crc_init:init.toString(16).padStart(6,'0'),
        context:UTF8ToString(context), pdu:UTF8ToString(pdu)});
});
}
extern "C" {
EMSCRIPTEN_KEEPALIVE int16_t *input_ptr() { return iq; }
EMSCRIPTEN_KEEPALIVE float *bins_ptr() { return bins; }
EMSCRIPTEN_KEEPALIVE unsigned channelize(unsigned n) {
    if (n > chunk || n % 80) return 0;
    if (!bank) bank = std::make_unique<Channelizer>();
    bank->reset();
    unsigned frame = 0, at = 0;
    bank->run(iq, n + prefix, [&](const fftwf_complex *b) {
        if (frame++ < prefix / 80) return;
        for (int ch = 0; ch < 40; ch++) {
            unsigned k = Channelizer::bin(ch);
            bins[at++] = b[k][0]; bins[at++] = b[k][1];
        }
    });
    return at;
}
EMSCRIPTEN_KEEPALIVE void decoder_init() {
    decoders.clear(); registry.clear(); samples = packets = 0;
    for (int ch = 0; ch < 40; ch++)
        decoders.emplace_back(new Decoder(ch, registry, [](const Packet &p) {
            char hex[521];
            static const char digits[] = "0123456789abcdef";
            unsigned at = 0;
            for (auto b : p.pdu) { hex[at++] = digits[b >> 4]; hex[at++] = digits[b & 15]; }
            hex[at] = 0; packets++;
            packet_js(p.channel, double(p.sample), p.aa, p.crc, p.crc_init, p.context.c_str(), hex);
        }));
}
EMSCRIPTEN_KEEPALIVE void decoder_reset(int clear) {
    for (auto &d : decoders) d->reset();
    if (clear) registry.clear();
}
EMSCRIPTEN_KEEPALIVE void decode(unsigned count) {
    if (count > chunk || count % 80 || decoders.size() != 40) return;
    for (unsigned k = 0; k < count; k += 80)
        for (unsigned ch = 0; ch < 40; ch++)
            decoders[ch]->run(bins[k + ch * 2], bins[k + ch * 2 + 1]);
    samples += uint64_t(count / 80) * 80;
}
EMSCRIPTEN_KEEPALIVE const char *decoder_status() {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "{\"samples\":%.0f,\"packets\":%.0f,\"channels\":[", double(samples), double(packets));
    status = buf;
    for (unsigned ch = 0; ch < decoders.size(); ch++) {
        std::snprintf(buf, sizeof(buf), "%s{\"channel\":%u,\"mhz\":%d,\"packets\":%.0f,\"crc_fail\":%.0f,\"candidates\":%.0f}",
            ch ? "," : "", ch, Channelizer::mhz(ch), double(decoders[ch]->valid),
            double(decoders[ch]->crc_fail), double(decoders[ch]->candidates));
        status += buf;
    }
    status += "]}";
    return status.c_str();
}
}
