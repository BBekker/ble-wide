// SPDX-License-Identifier: MIT
// Transport only: copy tagged int16 I/Q to stdout. No filtering or decoding.
#include "../include/w1700k_stream.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
constexpr size_t chunk = 163840;
constexpr unsigned reset = 1, new_run = 2, eof = 4;
volatile sig_atomic_t stopped = 0;
void stop(int) { stopped = 1; }
struct Header {
    uint32_t magic = 0x57454c42, flags = 0;
    uint64_t run = 0, first = 0, lost = 0;
    uint32_t samples = 0, id = 0;
};
static_assert(sizeof(Header) == 40);
std::vector<int16_t> batch;
Header header;
uint64_t cursor = 0;
void send(unsigned flags = 0) {
    size_t samples = batch.size() / 2;
    if (samples % 80) std::abort();
    header.samples = uint32_t(samples);
    header.flags |= flags;
    uint32_t bytes = sizeof(header) + uint32_t(batch.size() * 2);
    if (std::fwrite(&bytes, 4, 1, stdout) != 1 ||
        std::fwrite(&header, sizeof(header), 1, stdout) != 1 ||
        (samples && std::fwrite(batch.data(), 4, samples, stdout) != samples) || std::fflush(stdout))
        std::exit(0); // browser closed its pipe
    header.id++;
    header.flags = 0;
    batch.clear();
}
void gap(uint64_t n) {
    // Never join samples across a missing DMA completion. Discard an unfinished
    // chunk as well; that loss is included in the next frame's sample position.
    header.lost += n + batch.size() / 2;
    cursor += n;
    batch.clear();
    header.flags |= reset;
}
void append(const int16_t *iq, size_t n) {
    while (n) {
        if (batch.empty()) header.first = cursor;
        size_t take = std::min(n, chunk - batch.size() / 2);
        batch.insert(batch.end(), iq, iq + take * 2);
        cursor += take; iq += take * 2; n -= take;
        if (batch.size() / 2 == chunk) send();
    }
}
}
int main(int argc, char **argv) {
    const char *stream = "/w1700k-iq4", *file = nullptr;
    bool raw14 = false, pace = false;
    unsigned repeat = 1;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--stream") && i + 1 < argc) stream = argv[++i];
        else if (!std::strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!std::strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--raw14")) raw14 = true;
        else if (!std::strcmp(argv[i], "--pace")) pace = true;
        else { std::fprintf(stderr, "usage: sample-relay [--stream /name | --file iq16 [--raw14] [--repeat N] [--pace]]\n"); return 2; }
    }
    signal(SIGINT, stop); signal(SIGTERM, stop); signal(SIGPIPE, SIG_IGN);
    batch.reserve(chunk * 2);
    std::array<int16_t, 4096> block;
    if (file) {
        for (unsigned j = 0; j < repeat && !stopped; j++) {
            FILE *f = std::fopen(file, "rb");
            if (!f) { std::perror(file); return 1; }
            header.run++; header.flags |= reset | new_run; cursor = 0;
            const auto start = std::chrono::steady_clock::now();
            size_t n;
            while (!stopped && (n = std::fread(block.data(), 4, 2048, f))) {
                if (raw14) for (size_t k = 0; k < n; k++) {
                    uint32_t w; std::memcpy(&w, &block[2*k], 4);
                    block[2*k] = int16_t(((w >> 16 & 16383) ^ 8192) - 8192);
                    block[2*k+1] = int16_t(((w & 16383) ^ 8192) - 8192);
                }
                append(block.data(), n);
                if (pace) std::this_thread::sleep_until(start + std::chrono::nanoseconds(cursor * 1000000000ull / 160000000));
            }
            if (std::ferror(f)) { std::perror(file); std::fclose(f); return 1; }
            std::fclose(f);
            // A partial final FFT frame cannot produce a complete channel sample.
            batch.resize(batch.size() / 160 * 160);
            if (!batch.empty()) send();
        }
        send(eof);
        return 0;
    }
    w1700k_stream_header *h = nullptr;
    size_t bytes = 0;
    uint64_t run = 0, next = 0;
    while (!stopped) {
        if (!h) {
            h = w1700k_stream_open(stream, 0, &bytes, nullptr);
            if (!h) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); continue; }
            run = 0;
        }
        uint64_t r, newest;
        w1700k_stream_position(h, &r, &newest);
        if (r != run) {
            batch.clear(); cursor = 0; header.run = r; header.lost = 0;
            header.flags = reset | new_run; run = r; next = newest;
        }
        if (newest > next + h->ring_slices) {
            uint64_t skip = newest - next - h->ring_slices;
            gap(skip * 2048); next += skip;
        }
        for (unsigned k = 0; next < newest && k < 256 && !stopped; k++, next++) {
            auto *p = w1700k_stream_tags(h) + (next & (h->ring_slices - 1));
            uint64_t tag = w1700k_stream_load(p), expected = w1700k_stream_tag(run, next);
            if ((tag & ~W1700K_STREAM_TAG_KIND) != expected || (tag & W1700K_STREAM_TAG_LOST)) { gap(2048); continue; }
            if (tag & W1700K_STREAM_TAG_EVENT) continue;
            std::memcpy(block.data(), w1700k_stream_slice(h, next), 8192);
            if (w1700k_stream_load(p) != tag) { gap(2048); continue; }
            append(block.data(), 2048);
        }
        if (!w1700k_stream_alive(h)) {
            munmap(h, bytes); h = nullptr; gap(0);
        } else if (next == newest) std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    if (h) munmap(h, bytes);
}
