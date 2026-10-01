#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#ifdef __linux__
#include <unistd.h>
#endif

namespace rex::plume_renderer {

// A fixed set of worker threads for the frame's independent pieces of work
// (hashing textures, checking cached vertices). The frame is encoded while the
// title waits in VdSwap, so whatever is spread over these threads comes off
// the title's own time. The calling thread works too, and ParallelFor returns
// once every index is done.
class WorkerPool {
 public:
  static WorkerPool& Get() {
    static WorkerPool pool;
    return pool;
  }

  size_t threads() const { return workers_.size() + 1; }

  // The workers' kernel thread ids (Linux), for handing to the scheduler.
  std::vector<int> thread_ids() {
    std::lock_guard lock(mutex_);
    return thread_ids_;
  }

  // fn(i) for every i in [0, count), in any order, on any thread.
  void ParallelFor(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) {
      return;
    }
    if (count == 1 || workers_.empty()) {
      for (size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    {
      std::lock_guard lock(mutex_);
      job_ = &fn;
      count_ = count;
      next_.store(0, std::memory_order_relaxed);
      done_.store(0, std::memory_order_relaxed);
      ++generation_;
    }
    wake_.notify_all();
    Work(fn, count);
    std::unique_lock lock(mutex_);
    // Every index done, and no worker still inside this job: one that took it
    // late must not go on to take indices of the next job with this one's fn.
    finished_.wait(lock, [&] {
      return done_.load(std::memory_order_acquire) == count && active_ == 0;
    });
    job_ = nullptr;
  }

 private:
  WorkerPool() {
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    // Eight workers at most, and two cores left alone for the title's own
    // threads and the driver.
    const unsigned count = std::min(8u, hardware > 3 ? hardware - 3 : 1u);
    for (unsigned i = 0; i < count; ++i) {
      workers_.emplace_back([this] { Loop(); });
    }
  }

  ~WorkerPool() {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : workers_) {
      t.join();
    }
  }

  void Work(const std::function<void(size_t)>& fn, size_t count) {
    for (;;) {
      const size_t i = next_.fetch_add(1, std::memory_order_relaxed);
      if (i >= count) {
        break;
      }
      fn(i);
      if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
        std::lock_guard lock(mutex_);
        finished_.notify_all();
      }
    }
  }

  void Loop() {
    uint64_t seen = 0;
#ifdef __linux__
    {
      std::lock_guard lock(mutex_);
      thread_ids_.push_back(int(gettid()));
    }
#endif
    for (;;) {
      const std::function<void(size_t)>* job = nullptr;
      size_t count = 0;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
        if (stopping_) {
          return;
        }
        seen = generation_;
        job = job_;
        count = count_;
        if (job) {
          ++active_;
        }
      }
      if (job) {
        Work(*job, count);
        std::lock_guard lock(mutex_);
        --active_;
        finished_.notify_all();
      }
    }
  }

  std::vector<std::thread> workers_;
  std::vector<int> thread_ids_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable finished_;
  const std::function<void(size_t)>* job_ = nullptr;
  size_t count_ = 0;
  std::atomic<size_t> next_{0};
  std::atomic<size_t> done_{0};
  uint64_t generation_ = 0;
  unsigned active_ = 0;
  bool stopping_ = false;
};

}  // namespace rex::plume_renderer
