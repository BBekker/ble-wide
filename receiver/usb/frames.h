// SPDX-License-Identifier: MIT
// RTL8157 RX descriptor v2 and Ethernet/IPv4 framing. All reads are bounds checked.
// Protocol references and attribution: receiver/usb/README.md.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace usb_rx {
inline uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
inline uint32_t le32(const uint8_t *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline uint32_t checksum_sum(const uint8_t *p, size_t n, uint32_t sum = 0) {
    for (; n >= 2; n -= 2, p += 2) sum += be16(p);
    if (n) sum += uint32_t(*p) << 8;
    return sum;
}
inline bool checksum_ok(uint32_t sum) {
    while (sum >> 16) sum = (sum & 65535) + (sum >> 16);
    return sum == 65535;
}
struct Counters {
    uint64_t frames = 0, datagrams = 0, ignored = 0, malformed = 0;
    uint64_t checksum_errors = 0, fragments = 0;
};
struct Filter {
    uint8_t destination[4] = {10,77,0,10};
    uint16_t port = 7700, ports = 2;
};

// frame excludes the Ethernet FCS. Checksums reported bad by hardware never
// enter the sample pipeline. If checksum offload did not validate UDP, verify
// its nonzero checksum here (zero is permitted for IPv4 UDP).
template<class Deliver>
void ethernet(const uint8_t *frame, size_t size, uint32_t status, const Filter &filter,
              Counters &c, Deliver deliver) {
    ++c.frames;
    if (size < 14) { ++c.malformed; return; }
    uint16_t type = be16(frame + 12);
    size_t at = 14;
    for (unsigned tags = 0; type == 0x8100 || type == 0x88a8; ++tags) {
        if (tags == 2 || size - at < 4) { ++c.malformed; return; }
        type = be16(frame + at + 2); at += 4;
    }
    if (type != 0x0800) { ++c.ignored; return; }
    if (size - at < 20) { ++c.malformed; return; }
    const uint8_t *ip = frame + at;
    const size_t ihl = (ip[0] & 15) * 4, total = be16(ip + 2);
    if (ip[0] >> 4 != 4 || ihl < 20 || total < ihl || total > size - at) {
        ++c.malformed; return;
    }
    if (ip[9] != 17 || std::memcmp(ip + 16, filter.destination, 4)) { ++c.ignored; return; }
    if (be16(ip + 6) & 0x3fff) { ++c.fragments; return; }
    if (total - ihl < 8) { ++c.malformed; return; }
    const uint8_t *udp = ip + ihl;
    const uint16_t port = be16(udp + 2), length = be16(udp + 4);
    if (port < filter.port || uint32_t(port) >= uint32_t(filter.port) + filter.ports) { ++c.ignored; return; }
    if (length < 8 || length != total - ihl) { ++c.malformed; return; }
    constexpr uint32_t ip_bad = 1u << 26, udp_bad = 1u << 25;
    constexpr uint32_t ipv4_checked = 1u << 14, udp_checked = 1u << 11;
    if (status & (ip_bad | udp_bad) || !checksum_ok(checksum_sum(ip, ihl))) {
        ++c.checksum_errors; return;
    }
    if (be16(udp + 6) && (status & (ipv4_checked | udp_checked)) != (ipv4_checked | udp_checked)) {
        uint32_t sum = checksum_sum(ip + 12, 8, 17 + length);
        if (!checksum_ok(checksum_sum(udp, length, sum))) { ++c.checksum_errors; return; }
    }
    ++c.datagrams;
    deliver(udp + 8, size_t(length - 8));
}

// Each transfer contains 16-byte descriptors followed by frames including FCS,
// aligned to 16 bytes. An incomplete descriptor/frame is rejected, never guessed.
template<class Deliver>
void aggregate(const uint8_t *data, size_t size, const Filter &filter, Counters &c, Deliver deliver) {
    size_t at = 0;
    while (at < size) {
        if (size - at < 16) { ++c.malformed; return; }
        const size_t frame_size = (le32(data + at) >> 17) & 0x7fff;
        if (frame_size < 64 || frame_size > size - at - 16) { ++c.malformed; return; }
        ethernet(data + at + 16, frame_size - 4, le32(data + at + 8), filter, c, deliver);
        const size_t end = at + 16 + frame_size;
        const size_t next = (end + 15) & ~size_t(15);
        if (next >= size) return; // final USB transfer may omit alignment padding
        at = next;
    }
}
} // namespace usb_rx
