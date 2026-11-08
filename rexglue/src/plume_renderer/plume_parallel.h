#pragma once

#include "plume_renderer/plume_heap_check.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/thread.h>
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

  // fn(i) for every i in [0, count), in any order, on any thread. Returns as
  // soon as the last index is done - not once every worker has also noticed:
  // on a machine with fewer free cores than threads (the Xbox One), waiting
  // for a worker that took the job and was then descheduled cost the frame
  // milliseconds after all the work was finished.
  void ParallelFor(size_t count, const std::function<void(size_t)>& fn) {
    if (count == 0) {
      return;
    }
    // Inline when there is nothing to share, or when called from inside a job
    // (a worker, or the caller while it works): one job runs at a time.
    if (count == 1 || workers_.empty() || InsideJob()) {
      for (size_t i = 0; i < count; ++i) {
        fn(i);
      }
      return;
    }
    std::lock_guard caller(call_mutex_);
    InsideJob() = true;
    struct Leave {
      ~Leave() { InsideJob() = false; }
    } leave;
    uint32_t generation;
    {
      std::lock_guard lock(mutex_);
      generation = ++generation_;
      job_ = &fn;
      count_ = count;
      done_.store(0, std::memory_order_relaxed);
      // The job's number rides with the next index, so a worker still holding
      // the last job can never claim an index of this one.
      ticket_.store(uint64_t(generation) << 32, std::memory_order_release);
    }
    wake_.notify_all();
    Work(fn, count, generation);
    std::unique_lock lock(mutex_);
    finished_.wait(lock, [&] { return done_.load(std::memory_order_acquire) == count; });
    job_ = nullptr;
  }

 private:
  static bool& InsideJob() {
    thread_local bool inside = false;
    return inside;
  }

  WorkerPool() {
    const unsigned hardware = std::max(2u, std::thread::hardware_concurrency());
    // Eight workers at most, and two cores left alone for the title's own
    // threads and the driver. XERENGE_WORKERS=N chooses the number.
    unsigned count = std::min(8u, hardware > 3 ? hardware - 3 : 1u);
#if defined(PLUME_UWP) || defined(REX_PLATFORM_UWP)
    // The Xbox One gives the game few cores, and the title's thread, its audio
    // worker and the present thread keep three of them busy: with five workers
    // one was descheduled holding a claimed index, and the present thread
    // waited a time slice for it - 14 ms a race frame in "unpacking ahead",
    // for work that took a fraction of one.
    count = std::min(count, 2u);
#endif
    if (const char* text = std::getenv("XERENGE_WORKERS"); text && *text) {
      count = std::min(16u, unsigned(std::strtoul(text, nullptr, 10)));
    }
    std::fprintf(stderr, "plume: worker pool of %u for %u hardware thread(s)\n", count, hardware);
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

  // Claims and runs indices of job `generation` until none are left.
  void Work(const std::function<void(size_t)>& fn, size_t count, uint32_t generation) {
    for (;;) {
      uint64_t ticket = ticket_.load(std::memory_order_acquire);
      size_t i;
      do {
        i = size_t(ticket & 0xFFFFFFFFu);
        if (uint32_t(ticket >> 32) != generation || i >= count) {
          return;
        }
      } while (!ticket_.compare_exchange_weak(ticket, ticket + 1, std::memory_order_acq_rel));
      fn(i);
      HeapCheck("worker job");
      if (done_.fetch_add(1, std::memory_order_acq_rel) + 1 == count) {
        std::lock_guard lock(mutex_);
        finished_.notify_all();
      }
    }
  }

  void Loop() {
    uint32_t seen = 0;
    rex::thread::set_current_thread_name("plume worker");
    InsideJob() = true;
#ifdef __linux__
    {
      std::lock_guard lock(mutex_);
      thread_ids_.push_back(int(gettid()));
    }
#endif
    for (;;) {
      const std::function<void(size_t)>* job = nullptr;
      size_t count = 0;
      uint32_t generation = 0;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [&] { return stopping_ || generation_ != seen; });
        if (stopping_) {
          return;
        }
        seen = generation = generation_;
        job = job_;
        count = count_;
      }
      // The job may be over and its fn gone by now: Work dereferences it only
      // after claiming an index of that very job, which keeps it alive.
      if (job) {
        Work(*job, count, generation);
      }
    }
  }

  std::vector<std::thread> workers_;
  std::vector<int> thread_ids_;
  std::mutex call_mutex_;
  std::mutex mutex_;
  std::condition_variable wake_;
  std::condition_variable finished_;
  const std::function<void(size_t)>* job_ = nullptr;
  size_t count_ = 0;
  std::atomic<uint64_t> ticket_{0};
  std::atomic<size_t> done_{0};
  uint32_t generation_ = 0;
  bool stopping_ = false;
};

}  // namespace rex::plume_renderer
