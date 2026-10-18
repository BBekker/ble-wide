// SPDX-License-Identifier: MIT
// w1700k-stream's output: the shared ring every consumer reads (include/w1700k_stream.h), and the
// publisher that fills it from the receive ring, in order, unpacked to complex int16.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "ingest.h"
#include "w1700k_stream.h"

// The POSIX shared-memory object, created by the service and unlinked when it stops.
class SharedRing {
public:
    // Refuses to start while another live service owns the name.
    SharedRing(const std::string &name, unsigned slices);
    ~SharedRing();
    SharedRing(const SharedRing &) = delete;
    SharedRing &operator=(const SharedRing &) = delete;

    w1700k_stream_header *header() const { return header_; }
    // Consumers can map it from now on: the service receives (its sockets are bound).
    void ready();

private:
    std::string name_;
    size_t bytes_ = 0;
    w1700k_stream_header *header_ = nullptr;
};

class Publisher {
public:
    Publisher(const SampleRing &raw, Ingest &ingest, w1700k_stream_header *out);
    ~Publisher();
    Publisher(const Publisher &) = delete;
    Publisher &operator=(const Publisher &) = delete;

    void start();

private:
    void schedule();
    void process(uint64_t run, uint64_t first, uint64_t last);
    enum class Slot { Data, Event, Lost };
    Slot classify(uint64_t run, uint64_t index) const;
    bool overrun(uint64_t run, uint64_t index) const;
    void publish_position(uint64_t run, uint64_t newest);
    void report();  // heartbeat and counters into the header

    const SampleRing &raw_;
    Ingest &ingest_;
    w1700k_stream_header *out_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    uint64_t lost_ = 0, overruns_ = 0;
    std::thread scheduler_;
};
