// SPDX-License-Identifier: MIT
// Bounded SPSC transfer ring between the privileged USB helper and receiver.
// A full ring drops whole USB transfers; it never overwrites an in-use buffer.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace usb_rx {
constexpr uint64_t ring_magic = 0x3142585355424c42ull; // BLBUSXB1, versioned below
constexpr size_t ring_header_bytes = 4096, slot_count = 512, slot_bytes = 65536;
struct alignas(64) Header {
    uint64_t magic, version, slots, bytes;
    uint64_t reserved[4];
    alignas(64) uint64_t head;
    alignas(64) uint64_t tail;
    alignas(64) uint64_t state; // 0: waiting, 1: capturing, 2: stopped
    uint64_t transfers, usb_bytes, dropped, errors, pid;
};
struct alignas(64) Slot {
    uint64_t length;
    uint64_t reserved[7];
    uint8_t data[slot_bytes];
};
constexpr size_t ring_bytes = ring_header_bytes + sizeof(Slot) * slot_count;
static_assert(sizeof(Header) <= ring_header_bytes);
static_assert(__atomic_always_lock_free(8, nullptr), "requires lock-free cross-process 64-bit atomics");
inline uint64_t acquire(const uint64_t &v) { return __atomic_load_n(&v, __ATOMIC_ACQUIRE); }
inline void release(uint64_t &v, uint64_t x) { __atomic_store_n(&v, x, __ATOMIC_RELEASE); }
inline void add(uint64_t &v, uint64_t x = 1) { __atomic_fetch_add(&v, x, __ATOMIC_RELAXED); }

class TransferRing {
    int fd_ = -1;
    void *map_ = MAP_FAILED;
    bool writer_;
    uint64_t next_ = 0;
public:
    Header *header = nullptr;
    TransferRing(const std::string &path, bool writer, uid_t owner) : writer_(writer) {
        bool created = false;
        if (!writer) {
            fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            created = fd_ >= 0;
            if (fd_ < 0 && errno != EEXIST) throw std::runtime_error("cannot create USB ring: " + std::string(strerror(errno)));
        }
        if (fd_ < 0) fd_ = open(path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
        try {
            struct stat st{};
            if (fd_ < 0 || fstat(fd_, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
                st.st_uid != owner || (st.st_mode & 0077))
                throw std::runtime_error("USB ring must be a private, regular file owned by the receiver user");
            struct flock lock{};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;
            lock.l_start=writer ? 0 : 1;lock.l_len=1;
            if (fcntl(fd_, F_SETLK, &lock)) throw std::runtime_error("USB ring already has this participant");
            if (created) {
                if (ftruncate(fd_, ring_bytes)) throw std::runtime_error("cannot size USB ring");
            } else if (st.st_size != off_t(ring_bytes)) throw std::runtime_error("USB ring size mismatch");
            map_ = mmap(nullptr, ring_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
            if (map_ == MAP_FAILED) throw std::runtime_error("cannot map USB ring");
            header = static_cast<Header *>(map_);
            if (created) {
                header->version=1;header->slots=slot_count;header->bytes=slot_bytes;
                release(header->magic,ring_magic);
            }
            if (acquire(header->magic)!=ring_magic || header->version!=1 || header->slots!=slot_count || header->bytes!=slot_bytes)
                throw std::runtime_error("USB ring ABI mismatch");
            if (writer && (acquire(header->head) || acquire(header->tail) || acquire(header->state)))
                throw std::runtime_error("use a fresh USB ring for each capture");
            next_ = writer ? 0 : acquire(header->tail);
        } catch (...) { cleanup(); throw; }
    }
    ~TransferRing() { cleanup(); }
    TransferRing(const TransferRing &) = delete;
    TransferRing &operator=(const TransferRing &) = delete;
    void cleanup() {
        if (map_!=MAP_FAILED) munmap(map_,ring_bytes);
        if (fd_>=0) close(fd_);
        map_=MAP_FAILED;fd_=-1;header=nullptr;
    }
    Slot &slot(uint64_t n) { return *(reinterpret_cast<Slot *>(static_cast<uint8_t *>(map_)+ring_header_bytes)+(n%slot_count)); }
    bool push(const uint8_t *data, size_t length) {
        if (!writer_ || length>slot_bytes) return false;
        const uint64_t tail=acquire(header->tail);
        if (tail>next_ || next_-tail>=slot_count) { add(header->dropped);return false; }
        Slot &s=slot(next_);std::memcpy(s.data,data,length);s.length=length;
        release(header->head,++next_);return true;
    }
    const Slot *front() {
        if (writer_ || next_>=acquire(header->head)) return nullptr;
        return &slot(next_);
    }
    void pop() { release(header->tail,++next_); }
};
} // namespace usb_rx
