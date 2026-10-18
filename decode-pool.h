// SPDX-License-Identifier: MIT
#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <time.h>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

// Persistent workers claim whole channels from a bounded batch. Each run waits
// for every channel before returning, preserving sample order across batches
// and making reset, status reads, and output collection safe on the caller.
class DecodePool {
  std::vector<std::thread> workers;
  std::mutex mutex;
  std::condition_variable wake, done;
  std::function<void(unsigned, const float *, size_t)> decode;
  const float *bins = nullptr;
  size_t frames = 0, generation = 0;
  unsigned remaining = 0;
  std::atomic<unsigned> next{0};
  std::atomic<uint64_t> worker_cpu_ns{0};
  std::exception_ptr error;
  bool stopping = false;
  static uint64_t cpu_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
  }
  void work() {
    try {
      for (;;) {
        unsigned ch = next.fetch_add(1, std::memory_order_relaxed);
        if (ch >= 37)
          break;
        decode(ch, bins, frames);
      }
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex);
      if (!error)
        error = std::current_exception();
    }
  }
  void worker() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    size_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return stopping || generation != seen; });
        if (stopping)
          return;
        seen = generation;
      }
      auto start = cpu_ns();
      work();
      worker_cpu_ns.fetch_add(cpu_ns() - start, std::memory_order_relaxed);
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!--remaining)
          done.notify_one();
      }
    }
  }

public:
  DecodePool(unsigned count,
             std::function<void(unsigned, const float *, size_t)> fn)
      : decode(std::move(fn)) {
    if (count < 1 || count > 40)
      throw std::runtime_error("decode threads must be 1..40");
    try {
      for (unsigned k = 1; k < count; k++)
        workers.emplace_back([this] { worker(); });
    } catch (...) {
      shutdown();
      throw;
    }
  }
  ~DecodePool() { shutdown(); }
  DecodePool(const DecodePool &) = delete;
  DecodePool &operator=(const DecodePool &) = delete;
  unsigned threads() const { return unsigned(workers.size()) + 1; }
  double extra_cpu_seconds() const { return worker_cpu_ns.load() / 1e9; }
  void run(const float *p, size_t n) {
    // Learn CONNECT_IND before processing data channels from this batch.
    // This avoids a fast data worker outrunning connection discovery.
    for (unsigned ch = 37; ch < 40; ch++)
      decode(ch, p, n);
    {
      std::lock_guard<std::mutex> lock(mutex);
      bins = p;
      frames = n;
      error = nullptr;
      next.store(0, std::memory_order_relaxed);
      remaining = unsigned(workers.size());
      generation++;
    }
    wake.notify_all();
    work(); // the calling thread is also a decoder worker
    std::unique_lock<std::mutex> lock(mutex);
    done.wait(lock, [&] { return remaining == 0; });
    if (error)
      std::rethrow_exception(error);
  }

private:
  void shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    wake.notify_all();
    for (auto &w : workers)
      if (w.joinable())
        w.join();
  }
};
