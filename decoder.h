// SPDX-License-Identifier: MIT
#pragma once
#include "preamble.h"
#include "soft-demod.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

struct Packet {
    int channel;
    uint64_t sample;
    uint32_t aa, crc, crc_init;
    std::string context;
    std::vector<uint8_t> pdu;
    double signal_dbfs = NAN;
    const char *demod = "hard";
    double frequency_offset_hz = 0;
    double noise_dbfs = NAN;
    bool crc_valid = true;
    uint32_t expected_crc = 0;
};
static inline uint32_t reverse24(uint32_t v) {
    uint32_t r = 0;
    for (int k = 0; k < 24; k++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}
static inline uint32_t crc_step(uint32_t crc, unsigned bit) {
    return (crc >> 1) ^ (((crc ^ bit) & 1) ? 0xda6000u : 0);
}
static inline uint32_t crc_of(const uint8_t *p, unsigned n, uint32_t init) {
    for (unsigned k = 0; k < n; k++)
        for (unsigned b = 0; b < 8; b++)
            init = crc_step(init, (p[k] >> b) & 1);
    return init;
}
static inline uint32_t recover_init(const uint8_t *p, unsigned n, uint32_t crc) {
    for (int k = int(n) * 8 - 1; k >= 0; k--) {
        unsigned feedback = crc >> 23;
        crc =
            (((crc ^ (feedback ? 0xda6000u : 0)) << 1) | (feedback ^ ((p[k / 8] >> (k % 8)) & 1))) &
            0xffffff;
    }
    return crc;
}
struct Context {
    uint32_t init = 0;
    unsigned votes = 0;
    uint64_t last = 0;
    bool observed = false;
};
class Registry {
    static constexpr size_t max_pending = 32768, max_known = 4096;
    struct Votes {
        Context context;
        std::array<uint64_t, 3> samples{};
    };
    std::unordered_map<uint64_t, Votes> pending;
    mutable std::mutex mutex;
    std::atomic<bool> populated{false};
    std::unordered_map<uint32_t, Context> entries;
    // Acquisition hint only: an address that yielded a plausible complete
    // packet before. It NEVER establishes a CRC seed or validates a packet.
    std::unordered_map<uint32_t, uint64_t> acquisition_hints;
    std::deque<std::pair<uint64_t, uint64_t>> order;
    std::unordered_map<uint32_t, uint32_t> neighbors;
    size_t neighbor_count = 0;
    // Cheap low-16-bit membership filters avoid random hash lookups at 2 Msps.
    // Full address/ambiguity checks still decide whether recovery is allowed.
    std::array<std::atomic<uint64_t>, 1024> known_low{}, near_low{};
    void install(uint32_t aa, Context c) {
        neighbor_count = 0;
        if (entries.size() >= max_known && !entries.count(aa)) {
            auto oldest = entries.begin();
            for (auto it = entries.begin(); it != entries.end(); ++it)
                if (it->second.last < oldest->second.last)
                    oldest = it;
            entries.erase(oldest);
        }
        entries[aa] = c;
        build_neighbors();
        populated.store(true, std::memory_order_release);
    }

  public:
    bool empty() const { return !populated.load(std::memory_order_acquire); }
    void remember(uint32_t aa, Context c) {
        std::lock_guard<std::mutex> lock(mutex);
        install(aa, c);
    }
    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        populated.store(false, std::memory_order_release);
        entries.clear();
        for (auto &v : known_low) v.store(0, std::memory_order_relaxed);
        for (auto &v : near_low) v.store(0, std::memory_order_relaxed);
        neighbors.clear();
        neighbor_count = 0;
        pending.clear();
        acquisition_hints.clear();
        order.clear();
    }
    size_t pending_count() const {
        std::lock_guard<std::mutex> lock(mutex);
        return pending.size();
    }
    std::optional<Context> known(uint32_t aa) {
        if (!may_know(aa)) return std::nullopt;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = entries.find(aa);
        return it == entries.end() ? std::nullopt : std::optional<Context>(it->second);
    }
  private:
    [[gnu::noinline]] void build_neighbors() {
        if (neighbor_count != entries.size()) {
            neighbors.clear();
            // Bloom bits may remain set after eviction; false positives only
            // cause a locked lookup. Never clear them while readers are active.
            for (auto &entry : entries) {
                known_low[(entry.first & 65535) / 64].fetch_or(uint64_t(1) << (entry.first & 63), std::memory_order_relaxed);
                for (unsigned bit = 0; bit < 32; bit++) {
                    uint32_t key = entry.first ^ (1u << bit);
                    near_low[(key & 65535) / 64].fetch_or(uint64_t(1) << (key & 63), std::memory_order_relaxed);
                    auto inserted = neighbors.emplace(key, entry.first);
                    if (!inserted.second && inserted.first->second != entry.first)
                        inserted.first->second = 0;
                }
            }
            neighbor_count = entries.size();
        }
    }
  public:
    bool may_know(uint32_t aa) const {
        return !empty() && ((known_low[(aa & 65535) / 64].load(std::memory_order_relaxed) >> (aa & 63)) & 1);
    }
    bool may_near(uint32_t aa) const {
        return !empty() && ((near_low[(aa & 65535) / 64].load(std::memory_order_relaxed) >> (aa & 63)) & 1);
    }
    // Acquisition needs only the resolved address, not a copied Context.
    // Reject negative membership hints inline; lock once for a possible match.
    [[gnu::always_inline]] uint32_t resolve_address(uint32_t aa) {
        if (empty()) return 0;
        const size_t index = (aa & 65535) / 64;
        const uint64_t bit = uint64_t(1) << (aa & 63);
        const bool exact = known_low[index].load(std::memory_order_relaxed) & bit;
        const bool near = near_low[index].load(std::memory_order_relaxed) & bit;
        return exact || near ? resolve_present(aa, exact, near) : 0;
    }
  private:
    [[gnu::noinline]] uint32_t resolve_present(uint32_t aa, bool exact, bool near) {
        std::lock_guard<std::mutex> lock(mutex);
        if (exact && entries.find(aa) != entries.end()) return aa;
        if (near) {
            auto it = neighbors.find(aa);
            if (it != neighbors.end()) return it->second;
        }
        return 0;
    }
  public:
    uint32_t near_address(uint32_t aa) {
        if (!may_near(aa)) return 0;
        std::lock_guard<std::mutex> lock(mutex);
        auto it = neighbors.find(aa);
        return it == neighbors.end() ? 0 : it->second;
    }
    bool recently_observed(uint32_t aa, uint64_t sample) const {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = acquisition_hints.find(aa);
        return it != acquisition_hints.end() && sample >= it->second &&
               sample - it->second < 60000000;
    }
    void hint_address(uint32_t aa, uint64_t sample) {
        std::lock_guard<std::mutex> lock(mutex);
        if (acquisition_hints.size() >= 1024 && !acquisition_hints.count(aa))
            acquisition_hints.clear();
        auto &last = acquisition_hints[aa];
        last = std::max(last, sample);
    }
    bool confirm(uint32_t aa, uint32_t init, uint64_t sample) {
        std::lock_guard<std::mutex> lock(mutex);
        auto known_it = entries.find(aa);
        if (known_it != entries.end()) return known_it->second.init == init;
        uint64_t key = (uint64_t(aa) << 24) | init;
        // Different CRC estimates are independent hypotheses. A damaged frame
        // must not erase two good observations of the same connection.
        while (!order.empty() &&
               (pending.size() >= max_pending || sample > order.front().second + 60000000 ||
                order.size() > 2 * max_pending)) {
            auto old = order.front();
            order.pop_front();
            auto it = pending.find(old.first);
            if (it != pending.end() && it->second.context.last == old.second)
                pending.erase(it);
        }
        auto &v = pending[key];
        auto &c = v.context;
        // Workers can complete different channels out of timestamp order.
        // Count distinct packets, not arrival order or timing duplicates.
        for (unsigned k = 0; k < c.votes; k++)
            if (sample > v.samples[k] ? sample - v.samples[k] <= 4 : v.samples[k] - sample <= 4)
                return false;
        c.init = init;
        c.last = std::max(c.last, sample);
        v.samples[c.votes++] = sample;
        order.emplace_back(key, c.last);
        if (c.votes < 3)
            return false;
        install(aa, c);
        pending.erase(key);
        return true;
    }
    void connection(const Packet &p) {
        if (p.aa != 0x8e89bed6 || p.pdu.size() != 36 || (p.pdu[0] & 15) != 5)
            return;
        const auto *b = p.pdu.data();
        uint32_t aa = b[14] | uint32_t(b[15]) << 8 | uint32_t(b[16]) << 16 | uint32_t(b[17]) << 24;
        uint32_t init = b[18] | uint32_t(b[19]) << 8 | uint32_t(b[20]) << 16;
        remember(aa, {reverse24(init), 3, p.sample, true});
    }
};
struct DecoderStats {
    uint64_t preamble_candidates = 0, preamble_syncs = 0, preamble_packets = 0;
    uint64_t valid = 0, crc_fail = 0, candidates = 0, recovery_attempts = 0, recovered = 0;
};
class Decoder : public DecoderStats {
  public:
    static constexpr size_t history_samples = 8192;

  private:
    struct Frame {
        uint64_t start = 0;
        unsigned bits = 0, target = 0;
        uint32_t aa = 0, init = 0;
        uint8_t whitening = 0;
        bool active = false, known = false;
        double energy_start = 0;
        uint64_t energy_sample = 0;
        std::array<uint8_t, 260> bytes{};
    };
    struct Phase {
        uint64_t history = 0;
        std::array<Frame, 4> frames;
    } phase[2];
    Registry &registry;
    std::function<void(const Packet &)> emit;
    int channel;
    float prev_i = 0, prev_q = 0;
    double floor = 1, power = 0, background = 0;
    double energy_total = 0;
    uint64_t samples = 0, last_end = 0;
    uint32_t last_crc = 0, last_aa = 0;
    bool adaptive, preamble_enabled, hard_acquisition;
    uint64_t segment_start = 0;
    uint64_t offset_history[2][2]{};
    bool offset_active = false;
    static constexpr size_t history_size = history_samples;
    // Keep cold history out of the hot Decoder object. The per-channel cache
    // line offset prevents all forty rings from contending for identical sets.
    std::vector<std::array<float, 2>> iq_history;
    std::array<float, 2> *history_data;
    size_t history_stride;
    struct Retry {
        uint64_t last_aa, due;
        uint32_t aa, seed;
        unsigned max_bytes;
        double timing = NAN, offset = 0;
        bool known = true;
    };
    std::deque<Retry> retries;
    struct Received {
        uint64_t sample;
        uint32_t aa, crc;
    };
    std::deque<Received> received;
    PreambleCorrelator preamble;
    struct PreambleCandidate {
        uint64_t end;
        double score;
    };
    std::deque<PreambleCandidate> preambles;
    bool already_received(uint64_t start, uint32_t aa, uint32_t crc = 0,
                          bool any_crc = true) const {
        for (auto r : received)
            if (r.aa == aa && (any_crc || r.crc == crc) &&
                std::abs(int64_t(r.sample) - int64_t(start)) <= 4)
                return true;
        return false;
    }
    void publish(Packet &p) {
        if (already_received(p.sample, p.aa, p.crc, false))
            return;
        while (!received.empty() && samples > received.front().sample + history_size)
            received.pop_front();
        received.push_back({p.sample, p.aa, p.crc});
        valid++;
        registry.connection(p);
        emit(p);
    }
    void schedule(uint32_t aa, uint32_t seed, uint64_t end = UINT64_MAX, double timing = NAN,
                  double offset = 0, bool known = true) {
        if (end == UINT64_MAX)
            end = samples;
        if (!adaptive || samples < segment_start + 67)
            return;
        for (auto &r : retries)
            if (r.aa == aa && std::abs(int64_t(end) - int64_t(r.last_aa)) <= 4) {
                // A connection may become known while speculative work waits.
                // Preserve the stronger CRC context and its AA-fit fallback.
                if (known && !r.known) {
                    r.known = true;
                    r.seed = seed;
                }
                if (std::isfinite(timing) && !std::isfinite(r.timing)) {
                    r.timing = timing + int64_t(end) - int64_t(r.last_aa);
                    r.offset = offset;
                }
                return;
            }
        if (!known && retries.size() >= 8)
            return;
        if (retries.size() >= 16) {
            auto speculative = std::find_if(retries.begin(), retries.end(),
                                            [](const Retry &r) { return !r.known; });
            if (speculative == retries.end())
                return;
            retries.erase(speculative);
        }
        // Extended advertisements can carry up to 255 payload bytes too.
        unsigned bytes = aa == 0x8e89bed6 ? 260 : 256;
        Retry r{end, end + bytes * 16 + 4, aa, seed, bytes, timing, offset, known};
        auto at = std::upper_bound(retries.begin(), retries.end(), r.due,
                                   [](uint64_t due, const Retry &r) { return due < r.due; });
        retries.insert(at, r);
    }
    [[gnu::noinline]] void correlate_candidate(double score) {
        if (samples < segment_start + 22)
            return;
        // Collapse neighboring sample-phase peaks, not distinct symbol starts.
        if (!preambles.empty() && samples <= preambles.back().end + 1) {
            if (score > preambles.back().score)
                preambles.back() = {samples, score};
        } else if (preambles.size() < 16)
            preambles.push_back({samples, score});
    }
    [[gnu::noinline]] void synchronize_pending() {
        while (!preambles.empty() && preambles.front().end + 66 <= samples) {
            auto candidate = preambles.front();
            preambles.pop_front();
            preamble_candidates++;
            std::array<PreambleCorrelator::Sync, 3> fits{};
            PreambleCorrelator::synchronize(
                candidate.end,
                [&](uint64_t n) { return history_data[(n % history_size) * history_stride]; },
                fits);
            constexpr unsigned back[] = {2, 0, 4};
            for (unsigned boundary = 0; boundary < 3; boundary++) {
                uint64_t end = candidate.end - back[boundary];
                const auto &sync = fits[boundary];
                if (!sync.score)
                    continue;
                uint32_t aa = sync.aa;
                if (__builtin_popcount(aa ^ 0x8e89bed6u) <= 2)
                    aa = 0x8e89bed6;
                bool adv = aa == 0x8e89bed6;
                auto context = adv ? std::nullopt : registry.known(aa);
                if (!adv && !context) {
                    uint32_t near = registry.near_address(aa);
                    if (near) {
                        aa = near;
                        context = registry.known(aa);
                    }
                }
                if (sync.first_bit != bool(aa & 1))
                    continue;
                if (!adv && !context && (!valid_aa(aa) || power < floor * 4))
                    continue;
                preamble_syncs++;
                schedule(aa,
                         adv       ? 0xaaaaaa
                         : context ? context->init
                                   : 0,
                         end + 64, sync.timing, sync.offset, adv || context);
                if (adv || context)
                    break;
            }
        }
    }
    [[gnu::always_inline]] void acquire(uint64_t history) {
        uint32_t aa = uint32_t(history >> 8), difference = aa ^ 0x8e89bed6u;
        if ((difference & (difference - 1)) == 0) {
            acquire_candidate(history);
            return;
        }
        if (registry.empty())
            return;
        unsigned pre = uint8_t(history), d = pre ^ ((aa & 1) ? 0x55 : 0xaa);
        if (((d & (d - 1)) == 0 && registry.may_know(aa)) ||
            ((pre == 0x55 || pre == 0xaa) && registry.may_near(aa)))
            acquire_candidate(history);
    }
    [[gnu::noinline]] void acquire_candidate(uint64_t history) {
        uint32_t aa = uint32_t(history >> 8);
        unsigned errors = __builtin_popcount(aa ^ 0x8e89bed6u);
        if (errors == 0 || (errors == 1 && power > floor * 2))
            schedule(0x8e89bed6, 0xaaaaaa);
        if (errors == 0 || registry.empty())
            return;
        unsigned preamble = uint8_t(history), expected = (aa & 1) ? 0x55 : 0xaa;
        if (__builtin_popcount(preamble ^ expected) <= 1 && registry.may_know(aa)) {
            if (auto known = registry.known(aa)) {
                schedule(aa, known->init);
                return;
            }
        }
        if ((preamble == 0x55 || preamble == 0xaa) && power > floor * 2) {
            uint32_t near = registry.near_address(aa);
            if (near && preamble == ((near & 1) ? 0x55u : 0xaau))
                if (auto known = registry.known(near)) schedule(near, known->init);
        }
    }
    [[gnu::noinline]] void recover_pending() {
        while (!retries.empty() && retries.front().due <= samples) {
            auto r = retries.front();
            retries.pop_front();
            if (already_received(r.last_aa - 63, r.aa))
                continue;
            recovery_attempts++;
            SoftDemod::Result result;
            if (!SoftDemod::recover(
                    r.last_aa, r.aa, channel, r.seed, r.max_bytes,
                    [&](uint64_t n) { return history_data[(n % history_size) * history_stride]; },
                    [&](const uint8_t *p, size_t n, uint32_t seed, uint32_t crc) {
                        return !r.known || crc_of(p, n, seed) == crc;
                    },
                    result, r.timing, r.offset, r.known))
                continue;
            if (!r.known) {
                r.seed = recover_init(result.bytes.data(), result.bytes.size(), result.crc);
                candidates++;
                if (!registry.confirm(r.aa, r.seed, r.last_aa - 63))
                    continue;
            }
            auto context = registry.known(r.aa);
            Packet p{channel,
                     r.last_aa - 63,
                     r.aa,
                     result.crc,
                     reverse24(r.seed),
                     r.aa == 0x8e89bed6                                       ? "advertising"
                     : context && context->observed ? "connection_request"
                                                                              : "repeated_crc_seed",
                     std::move(result.bytes)};
            p.signal_dbfs = 10 * std::log10(std::max(1e-12, result.energy / (8192. * 8192.)));
            p.demod = std::isfinite(r.timing) ? "preamble" : "adaptive";
            if (std::isfinite(r.timing))
                preamble_packets++;
            p.frequency_offset_hz = result.offset_hz;
            recovered++;
            publish(p);
        }
    }

    static bool valid_aa(uint32_t aa) {
        if (aa == 0 || aa == 0xffffffff || __builtin_popcount(aa ^ 0x8e89bed6u) <= 1)
            return false;
        unsigned run = 1, transitions = 0, top = 0;
        for (int k = 1; k < 32; k++) {
            bool same = ((aa >> k) & 1) == ((aa >> (k - 1)) & 1);
            run = same ? run + 1 : 1;
            if (run > 6)
                return false;
            if (!same) {
                transitions++;
                if (k >= 27)
                    top++;
            }
        }
        return transitions <= 24 && top >= 2 &&
               !((aa & 255) == ((aa >> 8) & 255) && (aa & 255) == ((aa >> 16) & 255) &&
                 (aa & 255) == (aa >> 24));
    }
    void consume(Frame &p, unsigned b) {
        b ^= p.whitening & 1;
        p.whitening = uint8_t((p.whitening >> 1) ^ ((p.whitening & 1) ? 0x44 : 0));
        unsigned pos = p.bits++;
        if (!(pos & 7))
            p.bytes[pos / 8] = 0;
        p.bytes[pos / 8] |= uint8_t(b << (pos & 7));
        if (p.bits == 16) {
            unsigned len = p.bytes[1], type = p.bytes[0] & 15;
            bool good;
            if (p.aa == 0x8e89bed6)
                good = type == 7 ? len >= 1
                                 : ((type == 0 || type == 2 || type == 4 || type == 6)
                                        ? len >= 6 && len <= 37
                                    : (type == 1 || type == 3) ? len == 12
                                                               : type == 5 && len == 34);
            else
                good = (p.bytes[0] & 3) != 0 && !(p.bytes[0] & 0xe0) && len <= 251;
            if (!good) {
                p.active = false;
                return;
            }
            p.target = (len + 5) * 8;
        }
        if (p.bits != p.target || p.bits <= 16)
            return;
        p.active = false;
        unsigned n = p.bytes[1] + 2;
        uint32_t crc = p.bytes[n] | uint32_t(p.bytes[n + 1]) << 8 | uint32_t(p.bytes[n + 2]) << 16;
        if (last_aa == p.aa && last_crc == crc && samples <= last_end + 2)
            return;
        std::string context = "advertising";
        if (p.known) {
            if (crc_of(p.bytes.data(), n, p.init) != crc) {
                crc_fail++;
                return;
            }
            if (p.aa != 0x8e89bed6) {
                auto c = registry.known(p.aa);
                context = c && c->observed ? "connection_request" : "repeated_crc_seed";
            }
        } else {
            p.init = recover_init(p.bytes.data(), n, crc);
            candidates++;
            if (!registry.confirm(p.aa, p.init, p.start))
                return;
            context = "repeated_crc_seed";
        }
        last_end = samples;
        last_crc = crc;
        last_aa = p.aa;
        Packet packet{channel,
                      p.start,
                      p.aa,
                      crc,
                      reverse24(p.init),
                      context,
                      std::vector<uint8_t>(p.bytes.begin(), p.bytes.begin() + n)};
        // Mean channel-filtered power over the PDU and CRC, using every 2 Msps
        // sample. This is relative ADC level, not antenna-calibrated dBm.
        const double mean = (energy_total - p.energy_start) / (samples - p.energy_sample);
        packet.signal_dbfs = 10 * std::log10(std::max(1e-12, mean / (8192.0 * 8192.0)));
        publish(packet);
    }

    [[gnu::always_inline]] void bit(Phase &phase, unsigned b) {
        phase.history = (phase.history >> 1) | (uint64_t(b) << 39);
        for (auto &p : phase.frames)
            if (p.active)
                consume(p, b);
        if (!hard_acquisition)
            return;
        uint32_t aa = uint32_t(phase.history >> 8);
        if (adaptive)
            acquire(phase.history);
        bool adv = aa == 0x8e89bed6;
        if (!adv && (uint8_t(phase.history) != ((aa & 1) ? 0x55 : 0xaa) || !valid_aa(aa)))
            return;
        auto c = adv ? std::nullopt : registry.known(aa);
        if (!adv && !c && power < floor * 4)
            return;
        // Keep searching while speculative frames are being collected. Noise
        // can contain a plausible preamble/header before the real packet.
        Frame *slot = nullptr;
        for (auto &p : phase.frames)
            if (!p.active) {
                slot = &p;
                break;
            }
        if (!slot)
            for (auto &p : phase.frames)
                if (!p.known && (!slot || p.start < slot->start))
                    slot = &p;
        if (!slot)
            return;
        slot->active = true;
        slot->bits = 0;
        slot->target = 16;
        slot->aa = aa;
        slot->known = adv || c;
        slot->init = adv ? 0xaaaaaa : c ? c->init : 0;
        slot->whitening = uint8_t(channel | 64);
        slot->start = samples >= 63 ? samples - 63 : 0;
        slot->energy_start = energy_total;
        slot->energy_sample = samples;
    }

  public:
    Decoder(int ch, Registry &r, std::function<void(const Packet &)> fn, bool improve = true,
            std::array<float, 2> *shared_history = nullptr, size_t stride = 1,
            bool correlate = false, bool hard = true)
        : registry(r), emit(fn), channel(ch), adaptive(improve),
          preamble_enabled(improve && correlate), hard_acquisition(hard),
          iq_history(improve && !shared_history ? history_size + ch * 8 : 0),
          history_data(shared_history ? shared_history
                       : improve      ? iq_history.data() + ch * 8
                                      : nullptr),
          history_stride(stride) {}
    void reset() {
        offset_active = false;
        preamble.reset();
        preambles.clear();
        retries.clear();
        received.clear();
        segment_start = samples;
        for (auto &phase : offset_history)
            for (auto &h : phase)
                h = 0;
        for (auto &p : phase)
            p = Phase{};
        prev_i = prev_q = 0;
        floor = 1;
        power = background = 0;
        energy_total = 0;
        last_end = 0;
        last_crc = last_aa = 0;
    }
    [[gnu::always_inline]] void run(float i, float q) {
        if (adaptive) {
            history_data[(samples % history_size) * history_stride] = {i, q};
            if (!retries.empty() && retries.front().due <= samples)
                recover_pending();
        }
        double e = double(i) * i + double(q) * q;
        energy_total += e;
        power += .125 * (e - power);
        background += .0001 * (e - background);
        floor += (e < floor ? .001 : .00001) * (e - floor);
        float cross = prev_i * q - prev_q * i;
        if (preamble_enabled) {
            if (!preambles.empty() && preambles.front().end + 66 <= samples)
                synchronize_pending();
            if (power > 1.5 * background) {
                float dot = prev_i * i + prev_q * q;
                float discriminator = cross / (std::abs(cross) + std::abs(dot) + 1e-20f);
                double score = preamble.push(samples, discriminator);
                if (score > 0)
                    correlate_candidate(score);
            } else if (offset_active)
                preamble.reset();
        }
        if (adaptive && power > 1.5 * background) {
            offset_active = true;
            // Cheap acquisition hypotheses at +/-100 kHz. The packet-local
            // fit estimates the actual offset; no atan2 runs on idle channels.
            float dot = prev_i * i + prev_q * q;
            for (int k = 0; hard_acquisition && k < 2; k++) {
                auto &h = offset_history[samples & 1][k];
                h = (h >> 1) | (uint64_t(cross + (k ? .3249197f : -.3249197f) * dot > 0) << 39);
                // Offset searches are useful during bursts, not stationary
                // channel noise. Zero-offset recovery remains unconditional.
                acquire(h);
            }
        }
        if (adaptive && offset_active && power <= 1.5 * background) {
            for (auto &phase : offset_history)
                for (auto &h : phase)
                    h = 0;
            offset_active = false;
        }
        prev_i = i;
        prev_q = q;
        bit(phase[samples & 1], cross > 0);
        samples++;
    }
};
