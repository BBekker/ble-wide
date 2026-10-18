// SPDX-License-Identifier: MIT
#include "ingest.h"
#include "../../include/posix.h"
#include "../../include/iq8.h"
#include "../usb/frames.h"
#include "../usb/ring.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <dlfcn.h>
#include <pthread/qos.h>
#endif
#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
// The C ABI names a field `export`, a reserved keyword in C++.
#define export export_config
#include "../../include/uapi/w1700k_sdr.h"
#undef export
#endif

namespace {

// struct w1700k_sdr_export_header: le32 magic "SDR1", le16 offset, le16 slice_bytes, le64 seq.
constexpr uint32_t kMagic = 0x31524453;
constexpr size_t kHeader = 16;
constexpr unsigned kBatch = 64;
constexpr size_t kDatagram = 9216;
#ifdef __APPLE__
struct RxMessage {
    msghdr msg_hdr;
    size_t msg_len;
};

// Darwin's private msghdr_x ABI extends msghdr with size_t msg_datalen.
// Resolve it at runtime so machines without this optional API still use recvmsg.
// Layout and semantics: apple-oss-distributions/xnu, bsd/sys/socket_private.h.
static_assert(sizeof(msghdr) == 48 && offsetof(RxMessage, msg_len) == 48 && sizeof(RxMessage) == 56,
              "unexpected Darwin msghdr_x layout");
using ReceiveMany = ssize_t (*)(int, const RxMessage *, unsigned, int);
ReceiveMany receive_many()
{
#ifdef W1700K_DISABLE_RECVMSG_X
    return nullptr;  // exercise the portable fallback in regression tests
#else
    return reinterpret_cast<ReceiveMany>(dlsym(RTLD_DEFAULT, "recvmsg_x"));
#endif
}

// Wait for the first datagram, then drain the available batch without blocking.
int receive_batch(int fd, RxMessage *messages, unsigned count)
{
    static thread_local ReceiveMany many = receive_many();
    if (many) {
        // No MSG_WAITALL: return the queued datagrams without waiting to fill a batch.
        const ssize_t n = many(fd, messages, count, 0);
        if (n >= 0 || (errno != ENOSYS && errno != EOPNOTSUPP)) return int(n);
        many = nullptr;
    }
    unsigned received = 0;
    while (received < count) {
        ssize_t bytes = recvmsg(fd, &messages[received].msg_hdr, received ? MSG_DONTWAIT : 0);
        if (bytes < 0) {
            if (errno == EINTR) continue;
            return received ? int(received) : -1;
        }
        messages[received++].msg_len = unsigned(bytes);
    }
    return int(received);
}
#else
using RxMessage = mmsghdr;
int receive_batch(int fd, RxMessage *messages, unsigned count)
{
    return recvmmsg(fd, messages, count, MSG_WAITFORONE, nullptr);
}
#endif
constexpr size_t kRawBytes = kSliceSamples * 4;
constexpr size_t kPackedBytes = kSliceSamples / 8 * 7 * 4;
constexpr size_t kMinSliceBytes = 4096;          // shorter completions are firmware events
constexpr uint64_t kRunGapNs = 1000000000;       // silence that ends a run
constexpr uint64_t kRunJumpBack = 1 << 20;       // slices back: a new run (the driver was reloaded)
constexpr uint64_t kRunJumpForward = uint64_t(1) << 39;  // keeps slot indices below the tag's run field

uint64_t now_ns()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void atomic_max(std::atomic<uint64_t> &a, uint64_t v)
{
    uint64_t o = a.load(std::memory_order_relaxed);
    while (v > o && !a.compare_exchange_weak(o, v, std::memory_order_acq_rel))
        ;
}

std::mutex run_mutex;

}  // namespace

SampleRing::SampleRing(unsigned capacity_slices)
    : slices_(capacity_slices), words_(size_t(capacity_slices) * kSliceSamples),
      tags_(new std::atomic<uint64_t>[capacity_slices])
{
    if (!capacity_slices || (capacity_slices & (capacity_slices - 1)))
        throw std::runtime_error("ring capacity must be a power of two");
    for (unsigned i = 0; i < capacity_slices; ++i) tags_[i].store(0, std::memory_order_relaxed);
}

void SampleRing::claim(uint64_t index)
{
    tags_[index & (slices_ - 1)].store(0, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

void SampleRing::put(uint64_t run, uint64_t index, const uint32_t *words)
{
    claim(index);
    std::memcpy(&words_[(index & (slices_ - 1)) * kSliceSamples], words, kSliceSamples * 4);
    tags_[index & (slices_ - 1)].store(tag_of(run, index), std::memory_order_release);
}

// Packed slices back to raw words: of each 8 samples, the 8th's 28 bits sit in the spare bits
// [31:30] and [15:14] of the other 7 (o_k = w_k | ((w7 << (14 - 2k)) & 0xc000c000)).
void SampleRing::put_packed(uint64_t run, uint64_t index, const uint32_t *packed)
{
    claim(index);
    uint32_t *out = &words_[(index & (slices_ - 1)) * kSliceSamples];
    const auto *bytes = reinterpret_cast<const uint8_t *>(packed);
    for (unsigned g = 0; g < kSliceSamples / 8; ++g, bytes += 28, out += 8) {
        // Ethernet payloads can be two-byte aligned. memcpy permits unaligned
        // wire words without a whole-datagram copy or undefined C++ loads.
        uint32_t words[7];
        std::memcpy(words, bytes, sizeof(words));
        uint32_t tail = 0;
        for (unsigned k = 0; k < 7; ++k) {
            out[k] = words[k] & ~0xc000c000u;
            tail |= (words[k] & 0xc000c000u) >> (14 - 2 * k);
        }
        out[7] = tail;
    }
    tags_[index & (slices_ - 1)].store(tag_of(run, index), std::memory_order_release);
}

void SampleRing::put_event(uint64_t run, uint64_t index)
{
    tags_[index & (slices_ - 1)].store(event_tag_of(run, index), std::memory_order_release);
}

void SampleRing::put_iq4(uint64_t run, uint64_t index, const uint8_t *bytes, unsigned shift, bool eight)
{
    claim(index);
    uint32_t *out = &words_[(index & (slices_ - 1)) * kSliceSamples];
    if (eight) iq8_unpack_block(bytes, out, kSliceSamples, shift);
    else for (unsigned k = 0; k < kSliceSamples; ++k) out[k] = iq4_unpack(bytes[k], shift);
    tags_[index & (slices_ - 1)].store(tag_of(run, index), std::memory_order_release);
}

struct Ingest::Worker {
    int fd = -1;
    std::thread thread;
    std::vector<uint8_t> buffers = std::vector<uint8_t>(kBatch * kDatagram);
    std::unique_ptr<usb_rx::TransferRing> usb;
    // Written by this worker only, read by stats().
    std::atomic<uint64_t> datagrams{0}, bytes{0}, slices{0}, packed{0}, events{0}, pieces{0}, bad{0}, late{0},
        duplicates{0}, drops{0}, before{0}, spare{0}, stale{0}, stale_lines{0};
};

Ingest::Ingest(SampleRing &ring, const std::string &bind_ip, unsigned port, unsigned threads,
               unsigned ports, const std::string &usb_ring)
    : ring_(ring), bind_ip_(bind_ip), port_(port),
#ifdef __APPLE__
      threads_(ports),
#else
      threads_(ports > 1 ? ports : threads),
#endif
      ports_(ports), usb_ring_(usb_ring)
{
    if (!ports || ports > 64 || port + ports - 1 > 65535)
        throw std::runtime_error("invalid UDP port range");
    in_addr check;
    if (bind_ip.rfind("/dev/", 0) != 0 && inet_pton(AF_INET, bind_ip.c_str(), &check) != 1)
        throw std::runtime_error("not an IPv4 address: " + bind_ip);
    if (!usb_ring.empty() && bind_ip.rfind("/dev/", 0) == 0)
        throw std::runtime_error("USB and direct radio input cannot be combined");
#ifdef __APPLE__
    // Darwin's SO_REUSEPORT does not provide Linux's UDP flow distribution.
    (void)threads;
#endif
}

// Binds the sockets and starts receiving: not before, or the kernel would queue (and then
// drop) what came while nothing read.
void Ingest::start()
{
    if (!workers_.empty()) return;
    if (!usb_ring_.empty()) {
        auto w = std::make_unique<Worker>();
        w->usb = std::make_unique<usb_rx::TransferRing>(usb_ring_, false, getuid());
        workers_.push_back(std::move(w));
        workers_[0]->thread = std::thread([this] { work_usb(*workers_[0]); });
        return;
    }
    if (bind_ip_.rfind("/dev/", 0) == 0) {
#ifdef __linux__
        auto w = std::make_unique<Worker>();
        w->fd = open(bind_ip_.c_str(), O_RDWR | O_CLOEXEC);
        w1700k_sdr_info info{};
        w1700k_sdr_stream_rx geometry{};
        if (w->fd < 0 || ioctl(w->fd, W1700K_SDR_GET_INFO, &info) ||
            info.abi_version != W1700K_SDR_ABI_VERSION ||
            ioctl(w->fd, W1700K_SDR_STREAM_RX, &geometry) ||
            !geometry.slices || geometry.slice_bytes != kRawBytes ||
            geometry.slices <= geometry.slots + 128) {
            if (w->fd >= 0) close(w->fd);
            throw std::runtime_error("cannot read SDR DMA geometry; load the radio first");
        }
        device_bytes_ = geometry.header_bytes + size_t(geometry.slices) * geometry.slice_bytes;
        device_map_ = mmap(nullptr, device_bytes_, PROT_READ, MAP_SHARED, w->fd, 0);
        if (device_map_ == MAP_FAILED) {
            const std::string why = std::strerror(errno);
            device_map_ = nullptr;
            close(w->fd);
            throw std::runtime_error("cannot map SDR DMA ring: " + why);
        }
        workers_.push_back(std::move(w));
        workers_[0]->thread = std::thread([this] { work_device(*workers_[0]); });
        return;
#else
        throw std::runtime_error("direct SDR DMA input requires Linux on the router");
#endif
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(uint16_t(port_));
    inet_pton(AF_INET, bind_ip_.c_str(), &address.sin_addr);
    for (unsigned i = 0; i < threads_; ++i) {
        address.sin_port = htons(uint16_t(port_ + (ports_ > 1 ? i : 0)));
        auto w = std::make_unique<Worker>();
        int one = 1, rcvbuf = 48 << 20;
        timeval tick{0, 100000};
        w->fd = socket_cloexec(AF_INET, SOCK_DGRAM, 0);
        int buffer_result = -1;
#ifdef __APPLE__
        // Darwin may cap the first request, but reject a repeated oversized request.
        while (w->fd >= 0 && (buffer_result = setsockopt(w->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)))) {
            if ((errno != ENOBUFS && errno != EINVAL) || rcvbuf <= 65536) break;
            rcvbuf /= 2;
        }
        socklen_t buffer_size = sizeof(rcvbuf);
        if (!buffer_result) getsockopt(w->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &buffer_size);
        std::fprintf(stderr, "w1700k-stream: macOS receive port %u, buffer %d bytes\n", unsigned(ntohs(address.sin_port)), rcvbuf);
#else
        if (w->fd >= 0) buffer_result = setsockopt(w->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
#endif
        if (w->fd < 0 || setsockopt(w->fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) ||
            buffer_result ||
#ifndef __APPLE__
            setsockopt(w->fd, SOL_SOCKET, SO_RXQ_OVFL, &one, sizeof(one)) ||
#endif
            setsockopt(w->fd, SOL_SOCKET, SO_RCVTIMEO, &tick, sizeof(tick)) ||
            bind(w->fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) {
            std::string why = std::strerror(errno);
            if (w->fd >= 0) close(w->fd);
            throw std::runtime_error("UDP " + bind_ip_ + ":" + std::to_string(port_) + ": " + why);
        }
        workers_.push_back(std::move(w));
    }
    for (auto &w : workers_) w->thread = std::thread([this, p = w.get()] { work(*p); });
}

Ingest::~Ingest()
{
    stopping_ = true;
    for (auto &w : workers_) {
        if (w->thread.joinable()) w->thread.join();
        close(w->fd);
    }
#ifdef __linux__
    if (device_map_) munmap(device_map_, device_bytes_);
#endif
}

void Ingest::work_usb(Worker &w)
{
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    usb_rx::Filter filter;
    inet_pton(AF_INET, bind_ip_.c_str(), filter.destination);
    filter.port = uint16_t(port_); filter.ports = uint16_t(ports_);
    usb_rx::Counters counts;
    while (!stopping_.load(std::memory_order_relaxed)) {
        const auto *slot = w.usb->front();
        if (!slot) { usleep(100); continue; }
        const uint64_t t = now_ns();
        if (slot->length > usb_rx::slot_bytes) ++counts.malformed;
        else usb_rx::aggregate(slot->data, slot->length, filter, counts,
            [&](const uint8_t *data, size_t n) {
                const uint64_t previous = last_ns_.exchange(t, std::memory_order_acq_rel);
                take(w, data, n, previous && int64_t(t - previous) > int64_t(kRunGapNs), t);
            });
        // Malformed/failed network frames are additional to malformed IQ records.
        // Reset this accumulator after charging it, keeping the existing stream counters.
        w.bad.fetch_add(counts.malformed + counts.checksum_errors + counts.fragments, std::memory_order_relaxed);
        counts.malformed = counts.checksum_errors = counts.fragments = 0;
        w.usb->pop();
    }
    std::fprintf(stderr, "w1700k-stream: USB frames=%llu datagrams=%llu ignored=%llu dropped_transfers=%llu\n",
        (unsigned long long)counts.frames, (unsigned long long)counts.datagrams,
        (unsigned long long)counts.ignored, (unsigned long long)usb_rx::acquire(w.usb->header->dropped));
}

void Ingest::work_device(Worker &w)
{
#ifdef __linux__
    const auto *h = static_cast<const w1700k_sdr_stream_ring *>(device_map_);
    const auto *samples = static_cast<const uint8_t *>(device_map_) + h->header_bytes;
    const uint64_t safe = h->slices - h->slots;
    uint64_t next = __atomic_load_n(&h->completed, __ATOMIC_ACQUIRE);
    alignas(8) uint8_t packet[kHeader + kRawBytes];
    while (!stopping_.load(std::memory_order_relaxed)) {
        const uint64_t completed = __atomic_load_n(&h->completed, __ATOMIC_ACQUIRE);
        // Leave a margin behind the producer, matching the kernel UDP exporter.
        const uint64_t end = completed > 64 ? completed - 64 : 0;
        if (next >= end) { usleep(500); continue; }
        if (completed - next >= safe) {
            w.drops.fetch_add(completed - next - 64, std::memory_order_relaxed);
            next = end;
            new_run_.store(true, std::memory_order_release);
            continue;
        }
        const uint16_t length = (h->ctrl[next % h->slices] >> 16) & 0x3fff;
        if (length > kRawBytes) { ++w.bad; ++next; continue; }
        const uint16_t offset = 0;
        std::memcpy(packet, &kMagic, 4);
        std::memcpy(packet + 4, &offset, 2);
        std::memcpy(packet + 6, &length, 2);
        std::memcpy(packet + 8, &next, 8);
        std::memcpy(packet + kHeader, samples + (next % h->slices) * h->slice_bytes, length);
        if (__atomic_load_n(&h->completed, __ATOMIC_ACQUIRE) - next >= safe) continue;
        const uint64_t t = now_ns(), previous = last_ns_.exchange(t, std::memory_order_acq_rel);
        take(w, packet, kHeader + length, previous && t - previous > kRunGapNs, t);
        ++next;
    }
#else
    (void)w;
#endif
}

void Ingest::work(Worker &w)
{
#ifdef __APPLE__
    // Drain UDP ahead of CPU-heavy consumers to avoid socket-buffer drops.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    RxMessage msgs[kBatch]{};
    iovec iov[kBatch];
#ifndef __APPLE__
    char control[kBatch][CMSG_SPACE(sizeof(uint32_t))];
#endif
    for (unsigned k = 0; k < kBatch; ++k) {
        iov[k] = {&w.buffers[k * kDatagram], kDatagram};
        msgs[k].msg_hdr.msg_iov = &iov[k];
        msgs[k].msg_hdr.msg_iovlen = 1;
#ifndef __APPLE__
        msgs[k].msg_hdr.msg_control = control[k];
#endif
    }
    while (!stopping_.load(std::memory_order_relaxed)) {
        for (unsigned k = 0; k < kBatch; ++k) {
            msgs[k].msg_hdr.msg_flags = 0;
#ifndef __APPLE__
            msgs[k].msg_hdr.msg_controllen = sizeof(control[k]);
#endif
        }
        int n = receive_batch(w.fd, msgs, kBatch);
        if (n <= 0) continue;
        // A second of silence ends the run: the next slice starts another. Signed: another
        // thread may have stamped a later time between our clock read and the exchange.
        uint64_t t = now_ns(), previous = last_ns_.exchange(t, std::memory_order_acq_rel);
        bool fresh = previous && int64_t(t - previous) > int64_t(kRunGapNs);
        if (fresh) new_run_.store(true, std::memory_order_release);
        for (int k = 0; k < n; ++k) {
            if (msgs[k].msg_hdr.msg_flags & MSG_TRUNC) {
                w.datagrams.fetch_add(1, std::memory_order_relaxed);
                w.bytes.fetch_add(msgs[k].msg_len, std::memory_order_relaxed);
                w.bad.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            take(w, &w.buffers[k * kDatagram], msgs[k].msg_len, false, t);
#ifndef __APPLE__
            for (cmsghdr *c = CMSG_FIRSTHDR(&msgs[k].msg_hdr); c; c = CMSG_NXTHDR(&msgs[k].msg_hdr, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SO_RXQ_OVFL) {
                    uint32_t drops;
                    std::memcpy(&drops, CMSG_DATA(c), sizeof(drops));
                    w.drops.store(drops, std::memory_order_relaxed);
                }
#endif
        }
    }
}

// Firmware events are filed as event slots rather than dropped: the scheduler steps over them
// without counting a loss, and the samples either side stay contiguous. A completion the unit
// does not send at all (an empty one) cannot be told from a lost datagram and counts as lost.
void Ingest::take(Worker &w, const uint8_t *data, size_t n, bool fresh, uint64_t t)
{
    if (fresh) new_run_.store(true, std::memory_order_release);
    w.datagrams.fetch_add(1, std::memory_order_relaxed);
    w.bytes.fetch_add(n, std::memory_order_relaxed);
    // IQ81 jumbo datagrams contain two complete records, each with its own
    // scale and DMA sequence. Validate both before publishing either one.
    constexpr size_t record_bytes = kHeader + 2 * kSliceSamples;
    uint32_t first_magic = 0;
    if (n >= 4) std::memcpy(&first_magic, data, 4);
    if (first_magic == IQ8_MAGIC && n == 2 * record_bytes) {
        for (unsigned k = 0; k < 2; ++k) {
            uint32_t magic;
            uint16_t shift, samples;
            const uint8_t *p = data + k * record_bytes;
            std::memcpy(&magic, p, 4);
            std::memcpy(&shift, p + 4, 2);
            std::memcpy(&samples, p + 6, 2);
            if (magic != IQ8_MAGIC || shift > 7 || samples != kSliceSamples) {
                w.bad.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        take_record(w, data, record_bytes, t);
        take_record(w, data + record_bytes, record_bytes, t);
        return;
    }
    take_record(w, data, n, t);
}

void Ingest::take_record(Worker &w, const uint8_t *data, size_t n, uint64_t t)
{
    uint32_t magic;
    uint16_t offset, slice_bytes;
    uint64_t seq;
    if (n < kHeader) {
        w.bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(&magic, data, 4);
    std::memcpy(&offset, data + 4, 2);
    std::memcpy(&slice_bytes, data + 6, 2);
    std::memcpy(&seq, data + 8, 8);
    const size_t len = n - kHeader;
    const bool iq8 = magic == IQ8_MAGIC;
    const bool quantized = iq8 || magic == IQ4_MAGIC;
    if (quantized ? (offset > (iq8 ? 7 : 11) || slice_bytes != kSliceSamples || len != kSliceSamples * (iq8 ? 2 : 1))
            : (magic != kMagic || offset + len > slice_bytes)) {
        w.bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const bool event = !quantized && slice_bytes < kMinSliceBytes;
    if (!quantized && !event && (offset || len != slice_bytes)) {  // the 10G path sends whole slices
        w.pieces.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!quantized && !event && slice_bytes != kRawBytes && slice_bytes != kPackedBytes) {
        w.bad.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    uint64_t run = run_.load(std::memory_order_acquire), base = base_.load(std::memory_order_acquire);
    auto jumped = [](uint64_t seq, uint64_t base) {
        return seq < base ? base - seq > kRunJumpBack : seq - base > kRunJumpForward;
    };
    if (!run || jumped(seq, base) || new_run_.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(run_mutex);
        if (run_.load() == run && (new_run_.exchange(false) || !run || jumped(seq, base_.load()))) {
            base_.store(seq);
            newest_.store(0);
            started_ns_.store(t);
            run_.store(run + 1, std::memory_order_release);
        }
        run = run_.load(std::memory_order_acquire);
        base = base_.load(std::memory_order_acquire);
    }
    // A completion a little before the run's first was overtaken on the way: late (or, in the
    // run's first second, from before this server joined the stream).
    if (seq < base) {
        (int64_t(t - started_ns_.load()) < int64_t(kRunGapNs) ? w.before : w.late).fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t index = seq - base;
    if (index + ring_.capacity_slices() < newest_.load(std::memory_order_relaxed)) {
        w.late.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint64_t held = ring_.tag(index);
    if (held == SampleRing::tag_of(run, index) || held == SampleRing::event_tag_of(run, index)) {
        w.duplicates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (quantized) {
        ring_.put_iq4(run, index, data + kHeader, offset, iq8);
        w.slices.fetch_add(1, std::memory_order_relaxed);
        w.packed.fetch_add(1, std::memory_order_relaxed);
        // Reduced precision can legitimately repeat blocks/lines; the raw
        // stale-data heuristic is not valid for this lossy format.
    } else if (event) {
        ring_.put_event(run, index);
        w.events.fetch_add(1, std::memory_order_relaxed);
    } else if (slice_bytes == kRawBytes) {
        const uint32_t *words = reinterpret_cast<const uint32_t *>(data + kHeader);
        uint64_t spare = 0;
        for (unsigned k = 0; k < kSliceSamples; ++k) spare += (words[k] & 0xc000c000u) != 0;
        if (spare) w.spare.fetch_add(spare, std::memory_order_relaxed);
        ring_.put(run, index, words);
        w.slices.fetch_add(1, std::memory_order_relaxed);
        check(w, run, index);
    } else {
        ring_.put_packed(run, index, reinterpret_cast<const uint32_t *>(data + kHeader));
        w.slices.fetch_add(1, std::memory_order_relaxed);
        w.packed.fetch_add(1, std::memory_order_relaxed);
        check(w, run, index);
    }
    atomic_max(newest_, index + 1);
}

// A slice equal to the one a capture-ring lap earlier is what a reader that overtook the capture
// sends; an 8-sample line equal to the same line a lap earlier (28 bytes packed, 32 raw) is what a
// packer reading a stale D-cache line sends. Noise never repeats; a flat line (all eight samples
// equal, which the radio does output) can, so it is not compared.
void Ingest::check(Worker &w, uint64_t run, uint64_t index)
{
    if (index < kLap || ring_.tag(index - kLap) != SampleRing::tag_of(run, index - kLap)) return;
    const uint32_t *now = ring_.slice(index), *then = ring_.slice(index - kLap);
    uint64_t lines = 0, stale = 0;
    for (unsigned g = 0; g < kSliceSamples; g += 8) {
        bool flat = true;
        for (unsigned k = 1; k < 8 && flat; ++k) flat = now[g + k] == now[g];
        if (flat) continue;
        ++lines;
        stale += std::memcmp(now + g, then + g, 32) == 0;
    }
    if (ring_.tag(index - kLap) != SampleRing::tag_of(run, index - kLap)) return;  // overwritten meanwhile
    if (stale) w.stale_lines.fetch_add(stale, std::memory_order_relaxed);
    if (lines && stale == lines) w.stale.fetch_add(1, std::memory_order_relaxed);
}

Ingest::Position Ingest::position() const
{
    for (;;) {
        const uint64_t run = run_.load(std::memory_order_acquire), newest = newest_.load(std::memory_order_acquire);
        if (run_.load(std::memory_order_acquire) == run) return {run, newest};
    }
}

void Ingest::restart_run() { new_run_.store(true, std::memory_order_release); }

Ingest::Stats Ingest::stats() const
{
    Stats s;
    for (const auto &w : workers_) {
        s.datagrams += w->datagrams.load(std::memory_order_relaxed);
        s.bytes += w->bytes.load(std::memory_order_relaxed);
        s.slices += w->slices.load(std::memory_order_relaxed);
        s.packed += w->packed.load(std::memory_order_relaxed);
        s.events += w->events.load(std::memory_order_relaxed);
        s.pieces += w->pieces.load(std::memory_order_relaxed);
        s.bad += w->bad.load(std::memory_order_relaxed);
        s.late += w->late.load(std::memory_order_relaxed);
        s.before_start += w->before.load(std::memory_order_relaxed);
        s.duplicates += w->duplicates.load(std::memory_order_relaxed);
        s.socket_drops += w->drops.load(std::memory_order_relaxed);
        s.spare_words += w->spare.load(std::memory_order_relaxed);
        s.stale_slices += w->stale.load(std::memory_order_relaxed);
        s.stale_lines += w->stale_lines.load(std::memory_order_relaxed);
    }
    s.run = run_.load();
    s.newest = newest_.load();
    s.run_started = started_ns_.load() / 1e9;
    s.last_any = last_ns_.load() / 1e9;
    return s;
}
