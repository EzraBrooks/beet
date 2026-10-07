#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace beet {

/// Decides how a parallel node ticks its children within a single tick.
class Executor {
 public:
  virtual ~Executor() = default;

  /// Calls `fn(i)` for every `i` in `[0, n)` and returns once all calls have
  /// finished. Rethrows the first exception thrown by any call.
  virtual void bulk(std::size_t n,
                    const std::function<void(std::size_t)>& fn) = 0;
};

/// Ticks children one after another on the calling thread, in order.
class InlineExecutor final : public Executor {
 public:
  void bulk(std::size_t n,
            const std::function<void(std::size_t)>& fn) override {
    for (std::size_t i = 0; i < n; ++i) fn(i);
  }
};

inline Executor& inline_executor() {
  static InlineExecutor executor;
  return executor;
}

/// Ticks children concurrently on a fixed set of worker threads. The calling
/// thread helps run queued work while it waits, so nested parallel nodes on the
/// same pool cannot deadlock.
class ThreadPoolExecutor final : public Executor {
 public:
  explicit ThreadPoolExecutor(
      std::size_t threads = std::max(1u, std::thread::hardware_concurrency())) {
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i)
      workers_.emplace_back([this] { work(); });
  }

  ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
  ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

  ~ThreadPoolExecutor() override {
    {
      std::lock_guard lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) worker.join();
  }

  void bulk(std::size_t n,
            const std::function<void(std::size_t)>& fn) override {
    if (n == 0) return;

    struct Batch {
      std::mutex mutex;
      std::condition_variable finished;
      std::size_t remaining = 0;
      std::exception_ptr error;
    } batch;
    batch.remaining = n;

    auto run = [&batch, &fn](std::size_t i) {
      std::exception_ptr error;
      try {
        fn(i);
      } catch (...) {
        error = std::current_exception();
      }
      std::lock_guard lock(batch.mutex);
      if (error && !batch.error) batch.error = error;
      if (--batch.remaining == 0) batch.finished.notify_all();
    };

    {
      std::lock_guard lock(mutex_);
      for (std::size_t i = 1; i < n; ++i)
        jobs_.emplace_back([run, i] { run(i); });
    }
    wake_.notify_all();
    run(0);

    for (;;) {
      {
        std::lock_guard lock(batch.mutex);
        if (batch.remaining == 0) break;
      }
      if (auto job = try_pop()) {
        job();
        continue;
      }
      std::unique_lock lock(batch.mutex);
      batch.finished.wait(lock, [&] { return batch.remaining == 0; });
      break;
    }

    if (batch.error) std::rethrow_exception(batch.error);
  }

 private:
  std::function<void()> try_pop() {
    std::lock_guard lock(mutex_);
    if (jobs_.empty()) return {};
    auto job = std::move(jobs_.front());
    jobs_.pop_front();
    return job;
  }

  void work() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
        if (stopping_ && jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace beet
