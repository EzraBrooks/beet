#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <latch>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

#include "beet/result.hpp"

namespace beet {

namespace detail {
/// Stands in for the capturing lambda a parallel node passes to `bulk`.
struct bulk_fn {
  void operator()(std::size_t /*unused*/) const;
};
}  // namespace detail

/// Decides how a parallel node ticks its children within a single tick:
/// `exec.bulk(count, fn)` calls `fn(i)` for every `i` in `[0, count)` and
/// returns once all calls have finished, rethrowing the first exception thrown
/// by any call. Nodes refer to a stateful executor, so it must outlive them.
template <class E>
concept executor =
    requires(E& exec, std::size_t count, const detail::bulk_fn& fn) {
      exec.bulk(count, fn);
    };

/// Ticks children one after another on the calling thread, in order.
struct InlineExecutor {
  template <class F>
  void bulk(std::size_t count, const F& fn) const {
    for (std::size_t i = 0; i < count; ++i) {
      fn(i);
    }
  }
};

/// Ticks children concurrently on a fixed set of worker threads. The calling
/// thread helps run queued work while it waits, so nested parallel nodes on the
/// same pool cannot deadlock.
class ThreadPoolExecutor {
 public:
  explicit ThreadPoolExecutor(
      std::size_t threads = std::max(1U, std::thread::hardware_concurrency())) {
    workers_.reserve(threads);
    for (std::size_t i = 0; i < threads; ++i) {
      workers_.emplace_back(
          [this](const std::stop_token& stop) { work(stop); });
    }
  }

  ThreadPoolExecutor(const ThreadPoolExecutor&) = delete;
  ThreadPoolExecutor& operator=(const ThreadPoolExecutor&) = delete;

  template <class F>
  void bulk(std::size_t count, const F& fn) {
    if (count == 0) {
      return;
    }

    std::latch done(static_cast<std::ptrdiff_t>(count));
    [[maybe_unused]] std::atomic_flag failed;
    std::exception_ptr error;

    auto run = [&](std::size_t index) {
#if BEET_EXCEPTIONS
      try {
        fn(index);
      } catch (...) {
        if (!failed.test_and_set()) {
          error = std::current_exception();
        }
      }
#else
      fn(index);
#endif
      done.count_down();
    };

    {
      std::scoped_lock lock(mutex_);
      for (std::size_t i = 1; i < count; ++i) {
        jobs_.emplace_back([&run, i] { run(i); });
      }
    }
    wake_.notify_all();
    run(0);

    while (!done.try_wait()) {
      if (auto job = try_pop()) {
        job();
      } else {
        done.wait();
      }
    }

    if (error) {
      std::rethrow_exception(error);
    }
  }

 private:
  std::function<void()> try_pop() {
    std::scoped_lock lock(mutex_);
    if (jobs_.empty()) {
      return {};
    }
    auto job = std::move(jobs_.front());
    jobs_.pop_front();
    return job;
  }

  // Returns once stop is requested and the queue has drained.
  void work(const std::stop_token& stop) {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        if (!wake_.wait(lock, stop, [this] { return !jobs_.empty(); })) {
          return;
        }
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::deque<std::function<void()>> jobs_;
  // Last, so the workers are stopped and joined before the queue is destroyed.
  std::vector<std::jthread> workers_;
};

}  // namespace beet
