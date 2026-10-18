// SPDX-License-Identifier: MIT
// w1700k-stream: receives the W1700K's UDP export and serves it to every consumer on this host
// through shared memory (include/w1700k_stream.h): in order, unpacked to complex int16.
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include "ingest.h"
#include "publish.h"

namespace {


std::atomic<bool> stop_requested{false};
void on_signal(int) { stop_requested = true; }

void usage()
{
    std::fprintf(stderr,
                 "usage: w1700k-stream [options]\n"
                 "  --bind IP         the stream's destination address on this host (default 10.77.0.10)\n"
                 "  --device PATH     read the router's SDR DMA ring directly (Linux only)\n"
                 "  --usb-ring PATH   receive RTL8157 USB transfers from the separate privileged helper\n"
                 "  --receive-mib N   input ring size, a power of two (default 256)\n"
                 "  --port N          its UDP port (default 7700)\n"
                 "  --ports N         consecutive UDP ports, one receiver per port (default 1)\n"
                 "  --rx-threads N    UDP sockets and receive threads (default 8)\n"
                 "  --ring-mib N      the shared sample ring, a power of two (default 512: 0.42 s at 320 Msps)\n"
                 "  --name NAME       the shared-memory object (default /w1700k-stream)\n"
                 "  --report S        a JSON status line every S seconds (default 0: none)\n");
}

unsigned positive(const std::string &option, const std::string &text, unsigned long max)
{
    size_t used = 0;
    unsigned long v = 0;
    try {
        v = std::stoul(text, &used);
    } catch (const std::logic_error &) {
        used = 0;
    }
    if (!used || used != text.size() || v == 0 || v > max)
        throw std::runtime_error(option + " needs an integer 1.." + std::to_string(max) + ", not \"" + text + "\"");
    return unsigned(v);
}

}  // namespace

int main(int argc, char **argv)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    std::string bind = "10.77.0.10", name = W1700K_STREAM_NAME, usb_ring;
    unsigned ports = 1, port = 7700, rx_threads = 8, ring_mib = 512, receive_mib = 256, report = 0;
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[++i];
            };
            if (arg == "--bind") bind = value();
            else if (arg == "--usb-ring") usb_ring = value();
            else if (arg == "--device") {
                bind = value();
                if (bind.rfind("/dev/", 0) != 0) throw std::runtime_error("--device needs a /dev/ path");
            }
            else if (arg == "--receive-mib") receive_mib = positive(arg, value(), 1u << 16);
            else if (arg == "--port") port = positive(arg, value(), 65535);
            else if (arg == "--ports") ports = positive(arg, value(), 64);
            else if (arg == "--rx-threads") rx_threads = positive(arg, value(), 64);
            else if (arg == "--ring-mib") ring_mib = positive(arg, value(), 1u << 16);
            else if (arg == "--name") name = value();
            else if (arg == "--report") report = positive(arg, value(), 3600);
            else if (arg == "-h" || arg == "--help") {
                usage();
                return 0;
            } else {
                throw std::runtime_error("unknown option " + arg);
            }
        }
        if (ring_mib & (ring_mib - 1)) throw std::runtime_error("--ring-mib must be a power of two");
        if (receive_mib & (receive_mib - 1)) throw std::runtime_error("--receive-mib must be a power of two");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "w1700k-stream: %s\n", e.what());
        usage();
        return 2;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    try {
        SharedRing shared(name, ring_mib * 128);
        SampleRing received(receive_mib * 128);
        Ingest ingest(received, bind, port, rx_threads, ports, usb_ring);
        Publisher publisher(received, ingest, shared.header());
        publisher.start();
        ingest.start();
        shared.ready();
        std::fprintf(stderr, "w1700k-stream: serving %s:%u as %s (%u MiB)\n", bind.c_str(), port, name.c_str(), ring_mib);
        auto last = std::chrono::steady_clock::now();
        while (!stop_requested) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!report || std::chrono::steady_clock::now() - last < std::chrono::seconds(report)) continue;
            last = std::chrono::steady_clock::now();
            const w1700k_stream_header *h = shared.header();
            uint64_t run, newest;
            w1700k_stream_position(h, &run, &newest);
            std::printf("{\"run\":%" PRIu64 ",\"newest\":%" PRIu64 ",\"datagrams\":%" PRIu64 ",\"slices\":%" PRIu64
                        ",\"packed\":%" PRIu64 ",\"events\":%" PRIu64 ",\"lost\":%" PRIu64 ",\"overruns\":%" PRIu64 ",\"late\":%" PRIu64
                        ",\"duplicates\":%" PRIu64 ",\"bad\":%" PRIu64 ",\"socket_drops\":%" PRIu64
                        ",\"spare_words\":%" PRIu64 ",\"stale_slices\":%" PRIu64 ",\"stale_lines\":%" PRIu64
                        "}\n",
                        run, newest, h->datagrams, h->slices, h->packed, h->events, h->lost, h->overruns, h->late, h->duplicates,
                        h->bad, h->socket_drops, h->spare_words, h->stale_slices, h->stale_lines);
            std::fflush(stdout);
        }
    } catch (const std::exception &e) {
        std::fprintf(stderr, "w1700k-stream: %s\n", e.what());
        return 1;
    }
    return 0;
}
