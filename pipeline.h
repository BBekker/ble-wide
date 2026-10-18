// SPDX-License-Identifier: MIT
#pragma once
#include "channelizer.h"
#ifdef W1700K_METAL
#include "metal-channelizer.h"
#endif
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <time.h>
#include <vector>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

// Independent time chunks share the complete FIR history. Their output is
// consumed in order, so packet decoding remains continuous across chunk edges.
class Pipeline {
  static constexpr size_t prefix = 1280;
  size_t chunk = 163840;
  bool planar;
  std::string backend_name = "cpu";
#ifdef W1700K_METAL
  std::unique_ptr<MetalChannelizer> gpu;
#endif
  std::atomic<uint64_t> filter_cpu_ns{0}, gpu_ns{0}, decode_ns{0};
  static uint64_t thread_ns() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
  }
  struct Job {
    std::vector<int16_t> iq;
    std::vector<float> bins;
    bool done = false;
    std::exception_ptr error;
  };
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::shared_ptr<Job>> todo, pending;
  // Recycle completed jobs only after the synchronous decoder callback returns.
  // At most six jobs exist; keep their multi-megabyte buffers out of the allocator.
  std::vector<std::shared_ptr<Job>> reusable;
  std::vector<std::thread> workers;
  std::vector<int16_t> input, history = std::vector<int16_t>(2 * prefix, 0);
  bool stopping = false;
  std::function<void(const float *, size_t)> emit;
  void worker() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    // FFTW planning is not thread-safe.
    std::unique_ptr<Channelizer> bank;
    {
      std::lock_guard<std::mutex> guard(mutex);
      if (backend_name == "cpu")
        bank = std::make_unique<Channelizer>();
    }
    for (;;) {
      std::shared_ptr<Job> j;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [&] { return stopping || !todo.empty(); });
        if (stopping && todo.empty()) {
          bank.reset();
          return;
        }
        j = todo.front();
        todo.pop_front();
      }
      const uint64_t cpu_start = thread_ns();
      try {
#ifdef W1700K_METAL
        if (gpu) {
          gpu->run(j->iq.data(), j->iq.size() / 2, j->bins, planar);
          gpu_ns.store(uint64_t(gpu->gpu_seconds() * 1e9));
        } else
#endif
        {
          bank->reset();
          size_t frame = 0;
          size_t frames = (j->iq.size() / 2 - prefix) / 80;
          j->bins.resize(frames * 80);
          bank->run(j->iq.data(), j->iq.size() / 2,
                    [&](const fftwf_complex *b) {
                      if (frame++ < prefix / 80)
                        return;
                      for (int ch = 0; ch < 40; ch++) {
                        int k = Channelizer::bin(ch);
                        size_t t = frame - prefix / 80 - 1;
                        size_t at = planar ? 2 * (ch * frames + t) : 80 * t + 2 * ch;
                        j->bins[at] = b[k][0];
                        j->bins[at + 1] = b[k][1];
                      }
                    });
        }
      } catch (...) {
        j->error = std::current_exception();
      }
      filter_cpu_ns.fetch_add(thread_ns() - cpu_start);
      {
        std::lock_guard<std::mutex> guard(mutex);
        j->done = true;
      }
      wake.notify_all();
    }
  }
  void consume() {
    auto j = pending.front();
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [&] { return j->done; });
    }
    pending.pop_front();
    if (j->error)
      std::rethrow_exception(j->error);
    const uint64_t start = thread_ns();
    emit(j->bins.data(), j->bins.size() / 80);
    decode_ns.fetch_add(thread_ns() - start);
    reusable.push_back(std::move(j));
  }
  void submit() {
    if (input.empty())
      return;
    auto j = reusable.empty() ? std::make_shared<Job>() : std::move(reusable.back());
    if (!reusable.empty()) reusable.pop_back();
    j->done = false;
    j->error = nullptr;
    j->iq.reserve(2 * (prefix + chunk));
    j->iq.assign(history.begin(), history.end());
    j->iq.insert(j->iq.end(), input.begin(), input.end());
    std::copy(j->iq.end() - 2 * prefix, j->iq.end(), history.begin());
    input.clear();
    pending.push_back(j);
    {
      std::lock_guard<std::mutex> guard(mutex);
      todo.push_back(j);
    }
    wake.notify_all();
    if (pending.size() >= 6)
      consume();
  }

public:
  explicit Pipeline(std::function<void(const float *, size_t)> fn,
                    const std::string &backend = "cpu", bool channel_major = true)
      : planar(channel_major), emit(std::move(fn)) {
    if (backend != "cpu" && backend != "metal" && backend != "auto")
      throw std::runtime_error("backend must be cpu, metal or auto");
    if (backend != "cpu") {
#ifdef W1700K_METAL
      try {
        gpu = std::make_unique<MetalChannelizer>();
        backend_name = "metal";
        chunk = 655360; // 4.096 ms: amortize GPU command submission.
        std::fprintf(stderr, "Metal channelizer: %s\n",
                     gpu->device_name().c_str());
      } catch (const std::exception &e) {
        if (backend == "metal")
          throw;
        std::fprintf(stderr, "Metal unavailable, using CPU: %s\n", e.what());
      }
#else
      if (backend == "metal")
        throw std::runtime_error("Metal backend was not built");
#endif
    }
    input.reserve(chunk * 2);
    for (int k = 0; k < (backend_name == "metal" ? 1 : 3); k++)
      workers.emplace_back([this] { worker(); });
  }
  ~Pipeline() {
    try {
      flush();
    } catch (...) { /* the caller reports an explicit flush/run failure */
    }
    {
      std::lock_guard<std::mutex> guard(mutex);
      stopping = true;
    }
    wake.notify_all();
    for (auto &w : workers)
      w.join();
  }
  const std::string &backend() const { return backend_name; }
  double filter_cpu_seconds() const { return filter_cpu_ns.load() / 1e9; }
  double decode_cpu_seconds() const { return decode_ns.load() / 1e9; }
  double gpu_seconds() const { return gpu_ns.load() / 1e9; }
  void run(const int16_t *p, size_t n) {
    while (n) {
      size_t take = std::min(n, chunk - input.size() / 2);
      input.insert(input.end(), p, p + take * 2);
      p += take * 2;
      n -= take;
      if (input.size() == chunk * 2)
        submit();
    }
  }
  void flush() {
    submit();
    while (!pending.empty())
      consume();
  }
  void reset() {
    flush();
    std::fill(history.begin(), history.end(), 0);
  }
};
