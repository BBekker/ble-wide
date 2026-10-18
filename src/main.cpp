// SPDX-License-Identifier: MIT
#include "iq8.h"
#include "w1700k_stream.h"
#include "channelizer/channelizer.h"
#include "pipeline/capture-clock.h"
#include "decoder/decoder.h"
#include "decoder/streaming-demod.h"
#include "pipeline/pipeline.h"
#include "pipeline/decode-pool.h"
#include "pipeline/waterfall.h"
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif
static volatile sig_atomic_t stopped = 0;
static void stop(int) { stopped = 1; }
int run_main(int argc, char **argv) {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
  const char *file = nullptr, *stream = "/w1700k-iq4";
  unsigned repeat = 1;
  unsigned decode_threads = 4;
  std::string backend = "cpu";
  bool raw14 = false, planar = true;
  bool adaptive = false, preamble = false, streaming = true;
  unsigned quantize = 0;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--demod") && i+1<argc) {
      std::string mode=argv[++i];
      if(mode!="legacy"&&mode!="adaptive"&&mode!="preamble"&&mode!="streaming")return 2;
      adaptive=mode!="legacy" && mode!="streaming";
      streaming=mode=="streaming";
      preamble=mode=="preamble";
    } else if (!strcmp(argv[i], "--decode-threads") && i + 1 < argc) {
      char *end = nullptr;
      unsigned long n = strtoul(argv[++i], &end, 10);
      if (!*argv[i] || *end || n < 1 || n > 40) {
        fprintf(stderr, "--decode-threads must be 1..40\n");
        return 2;
      }
      decode_threads = unsigned(n);
    } else if (!strcmp(argv[i], "--bin-layout") && i + 1 < argc) {
      std::string layout = argv[++i];
      if (layout != "planar" && layout != "interleaved") return 2;
      planar = layout == "planar";
    } else if (!strcmp(argv[i], "--raw14"))
      raw14 = true;
    else if (!strcmp(argv[i], "--iq4") || !strcmp(argv[i], "--iq8")) {
      raw14 = true;
      quantize = !strcmp(argv[i], "--iq8") ? 8 : 4;
    } else if (!strcmp(argv[i], "--file") && i + 1 < argc)
      file = argv[++i];
    else if (!strcmp(argv[i], "--stream") && i + 1 < argc)
      stream = argv[++i];
    else if (!strcmp(argv[i], "--backend") && i + 1 < argc)
      backend = argv[++i];
    else if (!strcmp(argv[i], "--repeat") && i + 1 < argc)
      repeat = atoi(argv[++i]);
    else {
      fprintf(stderr,
              "usage: %s [--file raw-int16-iq] [--repeat N] [--stream /name] "
              "[--backend cpu|metal|auto] [--decode-threads 1..40] [--bin-layout planar|interleaved] [--demod legacy|adaptive|preamble|streaming]\n",
              argv[0]);
      return 2;
    }
  }
  signal(SIGINT, stop);
  signal(SIGTERM, stop);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  Registry registry;
  std::vector<std::array<float,2>> recovery_history(adaptive && decode_threads == 1 ? Decoder::history_samples*40:0);
  std::vector<std::unique_ptr<Decoder>> decoders;
  std::vector<std::unique_ptr<StreamingDecoder>> forward;
  std::vector<DecoderStats*> stats;
  uint64_t total = 0, lost = 0, run = 0, next = 0, packets = 0, resets = 0;
  double busy = 0;
  CaptureClock capture;
  Waterfall waterfall;
  uint64_t epoch = 0;
  auto begin = std::chrono::steady_clock::now(), report = begin,
       last_progress = begin;
  uint64_t frontier = 0;
  bool in_gap = false;
  auto emit = [&](const Packet &p) {
    if (p.crc_valid) packets++;
    printf("{\"type\":\"packet\",\"channel\":%d,\"mhz\":%d,\"sample\":%llu,"
           "\"access_address\":\"%08x\",\"crc\":\"%06x\",\"crc_init\":\"%06x\","
           "\"context\":\"%s\",\"capture_s\":%.9f,\"epoch\":%llu,\"signal_dbfs\":%.2f,\"pdu\":\"",
           p.channel, Channelizer::mhz(p.channel), (unsigned long long)p.sample,
           p.aa, p.crc, p.crc_init, p.context.c_str(), capture.packet(p.sample),
           (unsigned long long)epoch, p.signal_dbfs);
    for (auto b : p.pdu)
      printf("%02x", b);
    printf("\",\"demod\":\"%s\"", p.demod);
    if (strcmp(p.demod, "hard"))
      printf(",\"frequency_offset_hz\":%.1f", p.frequency_offset_hz);
    if (std::isfinite(p.noise_dbfs))
      printf(",\"noise_dbfs\":%.2f", p.noise_dbfs);
    else
      printf(",\"noise_dbfs\":null");
    printf(",\"crc_valid\":%s", p.crc_valid ? "true" : "false");
    if (!p.crc_valid) printf(",\"expected_crc\":\"%06x\"", p.expected_crc);
    puts("}");
  };
  std::array<std::vector<Packet>, 40> output;
  auto collect = [&](const Packet &p) { output[p.channel].push_back(p); };
  for (int ch = 0; ch < 40; ch++) {
    if (streaming) {
      forward.emplace_back(new StreamingDecoder(ch, registry, collect, true));
      stats.push_back(forward.back().get());
    } else {
      decoders.emplace_back(new Decoder(ch, registry, collect, adaptive,
          recovery_history.empty()?nullptr:recovery_history.data()+ch,recovery_history.empty()?1:40,preamble));
      stats.push_back(decoders.back().get());
    }
  }
  DecodePool pool(decode_threads, [&](unsigned ch, const float *bins, size_t frames) {
    if (!frames) return;
    const float *channel = bins + (planar ? 2 * ch * frames : 2 * ch);
    const size_t stride = planar ? 2 : 80;
    if (streaming) {
      forward[ch]->run_block(channel, frames, stride);
      return;
    }
    for (size_t k = 0; k < frames; k++) {
      const float *p = channel + k * stride;
      decoders[ch]->run(p[0], p[1]);
    }
  });
  double decode_wall = 0;
  Pipeline bank(
      [&](const float *batch, size_t frames) {
        auto start = std::chrono::steady_clock::now();
        if (decode_threads == 1) {
          // Retain the original sample-major order as a reference mode.
          for (size_t k = 0; k < frames; k++) {
            for (int ch = 0; ch < 40; ch++) {
              const float *p = planar ? batch + 2 * (ch * frames + k) : batch + 80 * k + 2 * ch;
              if (streaming) forward[ch]->run(p[0], p[1]);
              else decoders[ch]->run(p[0], p[1]);
            }
          }
        } else pool.run(batch, frames);
        // No worker touches the clock, JSON stream, or waterfall. All packet
        // fragments are serialized here after workers finish the batch.
        std::vector<Packet> ready;
        for (auto &channel : output) {
          for (auto &packet : channel) ready.push_back(std::move(packet));
          channel.clear();
        }
        std::stable_sort(ready.begin(), ready.end(), [](const Packet &a, const Packet &b) {
          return a.sample != b.sample ? a.sample < b.sample : a.channel < b.channel;
        });
        for (const auto &packet : ready) emit(packet);
        waterfall.batch(batch, frames, planar, capture.frame_time(),
                        [&](double start, double end, const auto &db) {
          printf("{\"type\":\"spectrum\",\"capture_s\":%.9f,\"end_s\":%.9f,\"epoch\":%llu,\"db\":[",
                 start, end, (unsigned long long)epoch);
          for (int ch = 0; ch < 40; ch++) printf("%s%.1f", ch ? "," : "", db[ch]);
          puts("]}");
        });
        capture.frame(frames);
        decode_wall += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      },
      backend, planar);
  auto flush = [&] {
    auto t = std::chrono::steady_clock::now();
    bank.flush();
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
                .count();
  };
  auto reset = [&] {
    flush();
    bank.reset();
    for (auto &d : decoders)
      d->reset();
    for (auto &d : forward) d->reset();
    waterfall.reset();
    capture.segment();
    resets++;
  };
  auto start_capture = [&] {
    reset();
    epoch++;
    printf("{\"type\":\"capture_start\",\"capture_s\":%.9f,\"epoch\":%llu}\n",
           capture.now(), (unsigned long long)epoch);
  };
  auto missing = [&](uint64_t n) {
    if (!in_gap) reset();
    in_gap = true;
    printf("{\"type\":\"gap\",\"capture_s\":%.9f,\"duration_s\":%.9f}\n",
           capture.now(), n / 160e6);
    lost += n;
    capture.missing(n);
  };
  auto process = [&](const int16_t *iq, size_t n) {
    auto t = std::chrono::steady_clock::now();
    capture.input(n);
    bank.run(iq, n);
    total += n;
    busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
                .count();
  };
  auto status = [&](uint64_t lag, bool alive) {
    double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
            .count();
    uint64_t attempts=0,recovered=0,pre_candidates=0,pre_syncs=0,pre_packets=0;
    for(auto *d:stats){attempts+=d->recovery_attempts;recovered+=d->recovered;pre_candidates+=d->preamble_candidates;pre_syncs+=d->preamble_syncs;pre_packets+=d->preamble_packets;}
    uint64_t payload_starts=0,payload_symbols=0,trellis_updates=0;
    for(auto &d:forward){payload_starts+=d->payload_starts;payload_symbols+=d->payload_symbols;trellis_updates+=d->trellis_updates;}
    printf("{\"type\":\"status\",\"samples\":%llu,\"packets\":%llu,\"lost_"
           "samples\":%llu,\"resets\":%llu,\"lag_ms\":%.3f,\"elapsed_s\":%.6f,"
           "\"processing_s\":%.6f,\"capacity_msps\":%.3f,\"source_alive\":%s,"
           "\"backend\":\"%s\",\"demodulator\":\"%s\",\"recovery_attempts\":%llu,\"recovered_packets\":%llu,\"preamble_candidates\":%llu,\"preamble_syncs\":%llu,\"preamble_packets\":%llu,\"filter_cpu_s\":%.6f,\"decode_cpu_s\":%.6f,"
           "\"payload_starts\":%llu,\"payload_symbols\":%llu,\"trellis_updates\":%llu,\"gpu_s\":%.6f,\"bin_layout\":\"%s\",\"decode_threads\":%u,\"decode_wall_s\":%.6f,\"capture_s\":%.9f,\"epoch\":%llu,\"channels\":[",
           (unsigned long long)total, (unsigned long long)packets,
           (unsigned long long)lost, (unsigned long long)resets,
           lag * 2048 / 160000.0, elapsed, busy, busy ? total / busy / 1e6 : 0,
           alive ? "true" : "false", bank.backend().c_str(), streaming?"streaming":preamble?"preamble":adaptive?"adaptive":"legacy",
           (unsigned long long)attempts,(unsigned long long)recovered,
           (unsigned long long)pre_candidates,(unsigned long long)pre_syncs,(unsigned long long)pre_packets,
           bank.filter_cpu_seconds(), bank.decode_cpu_seconds() + pool.extra_cpu_seconds(),
           (unsigned long long)payload_starts,(unsigned long long)payload_symbols,(unsigned long long)trellis_updates,
           bank.gpu_seconds(), planar ? "planar" : "interleaved", decode_threads, decode_wall, capture.now(), (unsigned long long)epoch);
    for (int ch = 0; ch < 40; ch++)
      printf("%s{\"channel\":%d,\"mhz\":%d,\"packets\":%llu,\"crc_fail\":%llu,"
             "\"candidates\":%llu}",
             ch ? "," : "", ch, Channelizer::mhz(ch),
             (unsigned long long)stats[ch]->valid,
             (unsigned long long)stats[ch]->crc_fail,
             (unsigned long long)stats[ch]->candidates);
    puts("]}");
    report = std::chrono::steady_clock::now();
  };
  alignas(64) int16_t block[4096];
  if (file) {
    for (unsigned j = 0; j < repeat && !stopped; j++) {
      FILE *f = fopen(file, "rb");
      if (!f) {
        perror(file);
        return 1;
      }
      start_capture();
      registry.clear();
      size_t n;
      while (!stopped && (n = fread(block, 4, 2048, f))) {
        if (raw14) {
          uint32_t raw[2048];
          uint8_t packed[4096];
          memcpy(raw, block, n * 4);
          if (quantize) {
            unsigned shift =
                quantize == 8 ? iq8_scale(raw, n) : iq4_scale(raw, n);
            if (quantize == 8)
              iq8_pack(raw, packed, n, shift);
            else
              iq4_pack(raw, packed, n, shift);
            for (size_t k = 0; k < n; k++)
              raw[k] = quantize == 8 ? iq8_unpack(packed + 2 * k, shift)
                                     : iq4_unpack(packed[k], shift);
          }
          for (size_t k = 0; k < n; k++) {
            block[2 * k] = iq4_signed14(raw[k] >> 16);
            block[2 * k + 1] = iq4_signed14(raw[k]);
          }
        }
        process(block, n);
      }
      flush();
      if (ferror(f)) {
        perror("read");
        fclose(f);
        return 1;
      }
      fclose(f);
    }
    status(0, false);
    return 0;
  }
  size_t bytes = 0;
  w1700k_stream_header *h = nullptr;
  while (!stopped) {
    if (!h) {
      h = w1700k_stream_open(stream, 0, &bytes, nullptr);
      if (!h) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
      }
      run = next = 0;
    }
    uint64_t r, newest;
    w1700k_stream_position(h, &r, &newest);
    if (newest != frontier || r != run) {
      last_progress = std::chrono::steady_clock::now();
      frontier = newest;
    }
    bool active = r && std::chrono::steady_clock::now() - last_progress <
                           std::chrono::seconds(1);
    if (r != run) {
      start_capture();
      registry.clear();
      run = r;
      next = newest;
    }
    if (newest > next + h->ring_slices) {
      missing((newest - next - h->ring_slices) * 2048);
      next = newest - h->ring_slices;
    }
    unsigned count = 0;
    while (next < newest && count++ < 256 && !stopped) {
      auto *tagp = w1700k_stream_tags(h) + (next & (h->ring_slices - 1));
      uint64_t tag = w1700k_stream_load(tagp),
               expected = w1700k_stream_tag(run, next);
      if ((tag & ~W1700K_STREAM_TAG_KIND) != expected ||
          (tag & W1700K_STREAM_TAG_LOST)) {
        missing(2048);
        next++;
        continue;
      }
      if (tag & W1700K_STREAM_TAG_EVENT) {
        // A firmware event replaces a sample slice; its RF duration is unknown.
        reset();
        printf("{\"type\":\"gap\",\"capture_s\":%.9f,\"duration_s\":0}\n", capture.now());
        next++;
        continue;
      }
      memcpy(block, w1700k_stream_slice(h, next), sizeof(block));
      if (w1700k_stream_load(tagp) != tag) {
        missing(2048);
        next++;
        continue;
      }
      in_gap = false;
      process(block, 2048);
      next++;
    }
    if (std::chrono::steady_clock::now() - report > std::chrono::milliseconds(250))
      status(newest > next ? newest - next : 0,
             active && w1700k_stream_alive(h));
    if (!w1700k_stream_alive(h)) {
      munmap(h, bytes);
      h = nullptr;
      reset();
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } else if (next == newest)
      std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
  flush();
  status(0, false);
  if (h)
    munmap(h, bytes);
  return 0;
}

int main(int argc, char **argv) {
  try {
    return run_main(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "BLE receiver: %s\n", e.what());
    return 1;
  }
}
