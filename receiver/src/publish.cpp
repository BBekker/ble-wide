// SPDX-License-Identifier: MIT
#include "publish.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <stdexcept>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr unsigned kChunk = 16;            // minimum slots per active-stream pass
constexpr uint64_t kHorizon = 1024;        // a slot this far behind the newest completion received is lost
constexpr uint64_t kIdleNs = 5000000;      // nothing new for this long: publish what has arrived
constexpr uint64_t kReportNs = 100000000;  // heartbeat and counters

int16_t i_of(uint32_t w) { return int16_t(int32_t(w << 2) >> 18); }   // bits [29:16], sign-extended
int16_t q_of(uint32_t w) { return int16_t(int32_t(w << 18) >> 18); }  // bits [13:0]

template <class T> void store(T *p, T v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
template <class T> T load(const T *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }

}  // namespace

// ---- the shared ring ------------------------------------------------------------------------------

SharedRing::SharedRing(const std::string &name, unsigned slices) : name_(name)
{
    if (name.size() < 2 || name[0] != '/' || name.find('/', 1) != std::string::npos)
        throw std::runtime_error("the shared-memory name must look like /w1700k-stream, not " + name);
    if (!slices || (slices & (slices - 1))) throw std::runtime_error("the ring must hold a power of two slices");
    size_t old_bytes = 0;
    if (w1700k_stream_header *old = w1700k_stream_open(name.c_str(), 0, &old_bytes, nullptr)) {
        const uint64_t pid = old->service_pid;
        const bool alive = w1700k_stream_alive(old) && kill(pid_t(pid), 0) == 0;
        munmap(old, old_bytes);
        if (alive) throw std::runtime_error("another w1700k-stream (pid " + std::to_string(pid) + ") serves " + name);
    }
    shm_unlink(name.c_str());  // a stopped service's object: consumers still mapping it keep their copy

    const uint64_t tags = W1700K_STREAM_HEADER_BYTES;
    const uint64_t samples = tags + ((uint64_t(slices) * 8 + 4095) & ~uint64_t(4095));
    bytes_ = size_t(samples + uint64_t(slices) * W1700K_STREAM_SLICE * 4);
    const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) throw std::runtime_error("shm_open " + name + ": " + std::strerror(errno));
    if (ftruncate(fd, off_t(bytes_))) {
        const std::string why = std::strerror(errno);
        close(fd);
        shm_unlink(name.c_str());
        throw std::runtime_error("sizing " + name + ": " + why);
    }
    void *p = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        shm_unlink(name.c_str());
        throw std::runtime_error("mapping " + name + ": " + std::strerror(errno));
    }
    header_ = static_cast<w1700k_stream_header *>(p);
    header_->version = W1700K_STREAM_VERSION;
    header_->header_bytes = W1700K_STREAM_HEADER_BYTES;
    header_->slice_samples = W1700K_STREAM_SLICE;
    header_->ring_slices = slices;
    header_->tags_offset = tags;
    header_->samples_offset = samples;
    header_->total_bytes = bytes_;
    header_->service_pid = uint64_t(getpid());
    store(&header_->heartbeat_ns, w1700k_stream_now_ns());
}

void SharedRing::ready()
{
    store(&header_->heartbeat_ns, w1700k_stream_now_ns());
    store(&header_->magic, W1700K_STREAM_MAGIC);  // last: a consumer that sees it sees the layout
}

SharedRing::~SharedRing()
{
    if (!header_) return;
    store(&header_->heartbeat_ns, uint64_t(0));  // consumers still mapping it see the service gone
    shm_unlink(name_.c_str());
    munmap(header_, bytes_);
}

// ---- the publisher --------------------------------------------------------------------------------

Publisher::Publisher(const SampleRing &raw, Ingest &ingest, w1700k_stream_header *out)
    : raw_(raw), ingest_(ingest), out_(out)
{
}

void Publisher::start()
{
    if (!scheduler_.joinable()) scheduler_ = std::thread([this] { schedule(); });
}

Publisher::~Publisher()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_one();
    if (scheduler_.joinable()) scheduler_.join();
}

void Publisher::publish_position(uint64_t run, uint64_t newest)
{
    const uint64_t seq = load(&out_->position_seq);
    store(&out_->position_seq, seq + 1);
    std::atomic_thread_fence(std::memory_order_release);
    store(&out_->run, run);
    store(&out_->newest, newest);
    store(&out_->position_seq, seq + 2);
}

void Publisher::report()
{
    const Ingest::Stats s = ingest_.stats();
    store(&out_->datagrams, s.datagrams);
    store(&out_->bytes, s.bytes);
    store(&out_->slices, s.slices);
    store(&out_->packed, s.packed);
    store(&out_->events, s.events);
    store(&out_->pieces, s.pieces);
    store(&out_->bad, s.bad);
    store(&out_->late, s.late);
    store(&out_->before_start, s.before_start);
    store(&out_->duplicates, s.duplicates);
    store(&out_->socket_drops, s.socket_drops);
    store(&out_->spare_words, s.spare_words);
    store(&out_->stale_slices, s.stale_slices);
    store(&out_->stale_lines, s.stale_lines);
    store(&out_->lost, lost_);
    store(&out_->overruns, overruns_);
    store(&out_->received, s.newest);
    store(&out_->run_started_ns, uint64_t(s.run_started * 1e9));
    store(&out_->last_any_ns, uint64_t(s.last_any * 1e9));
    store(&out_->heartbeat_ns, w1700k_stream_now_ns());
}

// Walk the receive ring in order. A slot is resolved when its completion is in,
// or kHorizon newer completions have arrived without it. Unpack up to 128 resolved
// slots per pass; an idle stream also publishes an incomplete chunk.
void Publisher::schedule()
{
    uint64_t run = 0, resolved = 0, published = 0;
    uint64_t seen_newest = 0, seen_at = 0, reported = 0, restarts = load(&out_->restart_requests);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
        }
        const uint64_t t = w1700k_stream_now_ns();
        if (t - reported >= kReportNs) {
            report();
            reported = t;
        }
        const uint64_t requests = load(&out_->restart_requests);
        if (requests != restarts) {
            restarts = requests;
            ingest_.restart_run();
        }

        const Ingest::Position p = ingest_.position();
        if (p.run != run) {
            run = p.run;
            resolved = published = 0;
            seen_newest = 0;
            publish_position(run, 0);
        }
        if (p.newest != seen_newest) {
            seen_newest = p.newest;
            seen_at = t;
        }
        const bool idle = t - seen_at > kIdleNs;
        while (resolved < p.newest) {
            const uint64_t tag = raw_.tag(resolved);
            if (tag == SampleRing::tag_of(run, resolved) || tag == SampleRing::event_tag_of(run, resolved) ||
                p.newest > resolved + kHorizon)
                ++resolved;
            else
                break;
        }
        if (resolved >= published + kChunk || (idle && resolved > published)) {
            const uint64_t last = std::min(resolved, published + 128);
            process(run, published, last);
            published = last;
            publish_position(run, published);
            continue;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::microseconds(200), [&] { return stopping_; });
    }
}

Publisher::Slot Publisher::classify(uint64_t run, uint64_t index) const
{
    const uint64_t tag = raw_.tag(index);
    if (tag == SampleRing::tag_of(run, index)) return Slot::Data;
    if (tag == SampleRing::event_tag_of(run, index)) return Slot::Event;
    return Slot::Lost;
}

// A lost slot whose place in the receive ring holds a later completion of the run: it may have
// arrived and been overwritten before it was published.
bool Publisher::overrun(uint64_t run, uint64_t index) const
{
    const uint64_t tag = raw_.tag(index) & ~(uint64_t(1) << 63);
    return (tag >> 40) == run && (tag & ((uint64_t(1) << 40) - 1)) > index + 1;
}

void Publisher::process(uint64_t run, uint64_t first, uint64_t last)
{
    uint64_t *tags = w1700k_stream_tags(out_);
    const uint64_t mask = out_->ring_slices - 1;
    auto claim = [&](uint64_t index) {
        __atomic_store_n(&tags[index & mask], uint64_t(0), __ATOMIC_RELAXED);
        std::atomic_thread_fence(std::memory_order_release);
    };
    auto finish = [&](uint64_t index, uint64_t kind) {
        __atomic_store_n(&tags[index & mask], w1700k_stream_tag(run, index) | kind, __ATOMIC_RELEASE);
    };

    uint64_t lost = 0, overruns = 0;
    for (uint64_t s = first; s < last; ++s) {
        const Slot kind = classify(run, s);
        if (kind == Slot::Event) {
            finish(s, W1700K_STREAM_TAG_EVENT);
            continue;
        }
        if (kind == Slot::Data) {
            claim(s);
            const uint32_t *words = raw_.slice(s);
            int16_t *dst = w1700k_stream_slice(out_, s);
            for (unsigned k = 0; k < kSliceSamples; ++k) {
                dst[2 * k] = i_of(words[k]);
                dst[2 * k + 1] = q_of(words[k]);
            }
            if (raw_.tag(s) == SampleRing::tag_of(run, s)) {
                finish(s, 0);
                continue;
            }
        }
        finish(s, W1700K_STREAM_TAG_LOST);  // never arrived, or overwritten while being copied
        ++lost;
        if (overrun(run, s)) ++overruns;
    }
    if (lost) lost_ += lost;
    if (overruns) overruns_ += overruns;
}
