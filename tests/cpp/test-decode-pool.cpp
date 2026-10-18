// SPDX-License-Identifier: MIT
#include "pipeline/decode-pool.h"
#include "demod-fixture.h"
#include "decoder/streaming-demod.h"
#include <memory>
#include <tuple>

using Key = std::tuple<int, uint64_t, uint32_t, std::vector<uint8_t>>;
static std::vector<Key> receive(bool streaming, unsigned threads,
                                size_t chunk) {
  Registry registry;
  std::array<std::vector<Key>, 40> output;
  std::vector<std::unique_ptr<Decoder>> adaptive;
  std::vector<std::unique_ptr<StreamingDecoder>> forward;
  auto emit = [&](const Packet &p) {
    output[p.channel].emplace_back(p.channel, p.sample, p.aa, p.pdu);
    if (p.channel < 37)
      assert(p.context == "connection_request");
  };
  for (int ch = 0; ch < 40; ch++) {
    if (streaming)
      forward.emplace_back(new StreamingDecoder(ch, registry, emit));
    else
      adaptive.emplace_back(new Decoder(ch, registry, emit));
  }
  constexpr size_t frames = 16000;
  std::vector<float> bins(frames * 80);
  const uint32_t aa = 0x12345679, seed = 0x654321;
  for (int ch = 0; ch < 40; ch++) {
    std::vector<uint8_t> pdu = ch < 37 ? std::vector<uint8_t>{2, 3, 11, 22, 33}
                                       : std::vector<uint8_t>{7, 1, 0};
    if (ch == 37) {
      pdu.resize(36);
      pdu[0] = 5;
      pdu[1] = 34;
      for (unsigned k = 0; k < 4; k++)
        pdu[14 + k] = aa >> (8 * k);
      for (unsigned k = 0; k < 3; k++)
        pdu[18 + k] = reverse24(seed) >> (8 * k);
    }
    auto iq = waveform(pdu, 20000, .3, 0, ch, false, ch < 37 ? aa : 0x8e89bed6,
                       ch < 37 ? seed : 0xaaaaaa, ch);
    size_t offset = ch < 37 ? 2000 : 0;
    for (size_t k = 0; k < iq.size() && k + offset < frames; k++) {
      bins[80 * (k + offset) + 2 * ch] = iq[k][0];
      bins[80 * (k + offset) + 2 * ch + 1] = iq[k][1];
    }
  }
  auto step = [&](unsigned ch, const float *p, size_t n) {
    for (size_t k = 0; k < n; k++) {
      if (streaming)
        forward[ch]->run(p[80 * k + 2 * ch], p[80 * k + 2 * ch + 1]);
      else
        adaptive[ch]->run(p[80 * k + 2 * ch], p[80 * k + 2 * ch + 1]);
    }
  };
  DecodePool pool(std::max(1u, threads), step);
  std::vector<Key> result;
  for (unsigned epoch = 0; epoch < 2; epoch++) {
    if (epoch) {
      for (auto &d : adaptive)
        d->reset();
      for (auto &d : forward)
        d->reset();
      registry.clear();
    }
    for (size_t k = 0; k < frames; k += chunk) {
      size_t n = std::min(chunk, frames - k);
      if (threads)
        pool.run(bins.data() + 80 * k, n);
      else
        for (size_t j = 0; j < n; j++)
          for (unsigned ch = 0; ch < 40; ch++)
            step(ch, bins.data() + 80 * (k + j), 1);
    }
    for (auto &channel : output) {
      assert(channel.size() == 1);
      result.insert(result.end(), channel.begin(), channel.end());
      channel.clear();
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}
int main() {
  // The fast acquisition lookup must preserve exact-match priority and reject
  // ambiguous one-bit neighbors and low-16-bit membership false positives.
  {
    Registry addresses;
    constexpr uint32_t aa = 0x12345679;
    assert(addresses.resolve_address(aa) == 0);
    addresses.remember(aa, {123, 3, 0, true});
    assert(addresses.resolve_address(aa) == aa);
    for (unsigned bit = 0; bit < 32; bit++)
      assert(addresses.resolve_address(aa ^ (1u << bit)) == aa);
    assert(addresses.resolve_address(aa ^ 0x30000) == 0);
    addresses.remember(aa ^ 3, {456, 3, 1, true});
    assert(addresses.resolve_address(aa ^ 1) == 0);
    addresses.remember(aa ^ 1, {789, 3, 2, true});
    assert(addresses.resolve_address(aa ^ 1) == (aa ^ 1));
    uint32_t random = aa;
    for (unsigned k = 0; k < 10000; k++) {
      random = random * 1664525u + 1013904223u;
      const uint32_t expected = addresses.known(random) ? random : addresses.near_address(random);
      assert(addresses.resolve_address(random) == expected);
    }
    addresses.clear();
    assert(addresses.resolve_address(aa) == 0);
  }
  for (bool streaming : {false, true}) {
    auto serial = receive(streaming, 0, 8192);
    for (unsigned threads : {2, 4, 40})
      for (size_t chunk : {size_t(173), size_t(8192)})
        assert(receive(streaming, threads, chunk) == serial);
  }
  // Cross-channel blind CRC votes can arrive out of sample order; duplicates
  // must not count as independent observations, and bad seeds stay separate.
  Registry r;
  assert(!r.confirm(0x12345679, 123, 300));
  assert(!r.confirm(0x12345679, 123, 100));
  assert(!r.confirm(0x12345679, 123, 102));
  assert(!r.confirm(0x12345679, 456, 200));
  assert(r.confirm(0x12345679, 123, 200));
  assert(r.known(0x12345679)->init == 123);
  // Exercise concurrent installs, reads, inferred seeds, hints and neighbor
  // lookups.
  std::vector<std::thread> stress;
  for (unsigned t = 0; t < 8; t++)
    stress.emplace_back([&, t] {
      for (unsigned k = 0; k < 100; k++) {
        uint32_t aa = 0x12340000 + 100 * t + k;
        r.remember(aa, {123, 3, k, true});
        assert(r.known(aa)->init == 123);
        assert(r.resolve_address(aa) == aa);
        r.hint_address(aa, k);
        r.recently_observed(aa, k);
        r.near_address(aa ^ 1);
      }
    });
  for (auto &t : stress)
    t.join();
  // A worker exception must drain the batch, propagate, and allow reuse.
  bool fail = true;
  DecodePool pool(4, [&](unsigned ch, const float *, size_t) {
    if (fail && ch == 7)
      throw std::runtime_error("test");
  });
  bool caught = false;
  try {
    pool.run(nullptr, 0);
  } catch (const std::runtime_error &) {
    caught = true;
  }
  assert(caught);
  fail = false;
  pool.run(nullptr, 0);
  std::cout << "parallel channels, CONNECT_IND, seams, reset, registry and "
               "exceptions passed\n";
}
