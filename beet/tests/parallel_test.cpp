#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <variant>

#include "beet/beet.hpp"

namespace {

using beet::input_t;
using beet::never;
using beet::output_t;
using beet::Result;
using beet::Status;
using beet::Task;

struct Failed {
  int id;
};

/// Counts frames destroyed before they finished.
struct HaltProbe {
  int* halted;
  bool finished = false;
  ~HaltProbe() {
    if ((halted != nullptr) && !finished) {
      ++*halted;
    }
  }
};

/// Succeeds with `base + id` (or fails with Failed{id}) on its `ticks`-th tick.
auto job(int id, int ticks, bool succeed = true, int* halted = nullptr) {
  return beet::node([=](int base) noexcept -> Task<Result<int, Failed>> {
    HaltProbe probe{.halted = halted};
    for (int i = 1; i < ticks; ++i) {
      co_await beet::running;
    }
    probe.finished = true;
    if (!succeed) {
      co_return beet::make_unexpected(Failed{id});
    }
    co_return base + id;
  });
}

/// Never finishes; if halted before it could, bumps `*halted`.
auto spinner(int* halted) {
  return beet::node([halted](int) noexcept -> Task<Result<never, never>> {
    HaltProbe probe{.halted = halted};
    for (;;) {
      co_await beet::running;
    }
  });
}

template <class N>
int ticks_to_finish(beet::Runner<N>& runner) {
  int ticks = 1;
  while (runner.tick() == Status::Running) {
    ++ticks;
  }
  return ticks;
}

}  // namespace

TEST_CASE("parallel_all ticks children together and returns a tuple") {
  auto tree = beet::parallel_all(
      job(1, 3), job(2, 1),
      beet::node([](int value) noexcept { return std::to_string(value); }));
  static_assert(std::is_same_v<input_t<decltype(tree)>, int>);
  static_assert(std::is_same_v<output_t<decltype(tree)>,
                               std::tuple<int, int, std::string>>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Failed>);

  beet::Runner runner{tree, 100};
  CHECK(ticks_to_finish(runner) == 3);
  CHECK(runner.result().value() == std::tuple{101, 102, std::string("100")});
}

TEST_CASE("parallel_all fails fast and halts the other children") {
  int halted = 0;
  auto tree = beet::parallel_all(job(1, 5, true, &halted), job(2, 2, false));
  beet::Runner runner{tree, 0};
  CHECK(ticks_to_finish(runner) == 2);
  CHECK(runner.result().error().id == 2);
  CHECK(halted == 1);
}

TEST_CASE("parallel_any returns the first success as the union of outputs") {
  int halted = 0;
  auto tree =
      beet::parallel_any(job(1, 4, true, &halted), job(2, 2), job(3, 1, false));
  static_assert(std::is_same_v<output_t<decltype(tree)>, int>);

  beet::Runner runner{tree, 10};
  CHECK(ticks_to_finish(runner) == 2);
  CHECK(runner.result().value() == 12);
  CHECK(halted == 1);
}

TEST_CASE("parallel_any output collapses when a branch never finishes") {
  int halted = 0;
  auto tree = beet::parallel_any(job(1, 2), spinner(&halted));
  static_assert(std::is_same_v<output_t<decltype(tree)>, int>);
  beet::Runner runner{tree, 10};
  CHECK(ticks_to_finish(runner) == 2);
  CHECK(runner.result().value() == 11);
  CHECK(halted == 1);
}

TEST_CASE("parallel_any output is the union of different child outputs") {
  auto leaf = beet::node([](int value) { return char(value); });
  auto tree = beet::parallel_any(job(1, 2), leaf);
  static_assert(
      std::is_same_v<output_t<decltype(tree)>, std::variant<int, char>>);
  beet::Runner runner{tree, 7};
  CHECK(ticks_to_finish(runner) == 1);
  CHECK(std::get<char>(runner.result().value()) == char(7));
}

TEST_CASE("parallel_any breaks ties toward the lowest child index") {
  auto first = beet::node([](int value) { return value; });
  auto second = beet::node([](int value) { return char(value); });
  auto tree = beet::parallel_any(first, second);
  static_assert(
      std::is_same_v<output_t<decltype(tree)>, std::variant<int, char>>);
  beet::Runner runner{tree, 7};
  CHECK(runner.tick() == Status::Success);
  CHECK(std::holds_alternative<int>(runner.result().value()));
  CHECK(std::get<int>(runner.result().value()) == 7);
}

TEST_CASE("parallel_any keeps running while a never branch is alive") {
  auto tree = beet::parallel_any(job(1, 1, false), spinner(nullptr));
  static_assert(std::is_same_v<output_t<decltype(tree)>, int>);
  beet::Runner runner{tree, 0};
  for (int i = 0; i < 5; ++i) {
    CHECK(runner.tick() == Status::Running);
  }
}

TEST_CASE("parallel_any of never-output children can only fail") {
  auto failer = [](int id, int ticks) {
    return beet::node([=](int) noexcept -> Task<Result<never, Failed>> {
      for (int i = 1; i < ticks; ++i) {
        co_await beet::running;
      }
      co_return beet::make_unexpected(Failed{id});
    });
  };
  auto tree = beet::parallel_any(failer(1, 2), failer(2, 3));
  static_assert(std::is_same_v<output_t<decltype(tree)>, never>);
  beet::Runner runner{tree, 0};
  CHECK(ticks_to_finish(runner) == 3);
  CHECK(runner.result().error().id == 2);
}

TEST_CASE("parallel_any fails with the last child's error when all fail") {
  auto tree = beet::parallel_any(job(1, 1, false), job(2, 3, false));
  beet::Runner runner{tree, 0};
  CHECK(ticks_to_finish(runner) == 3);
  CHECK(runner.result().error().id == 2);
}

TEST_CASE("parallel_n succeeds once K children succeed") {
  auto tree = beet::parallel_n<2>(job(1, 1), job(2, 5), job(3, 2));
  static_assert(
      std::is_same_v<output_t<decltype(tree)>,
                     std::tuple<std::optional<int>, std::optional<int>,
                                std::optional<int>>>);
  beet::Runner runner{tree, 0};
  CHECK(ticks_to_finish(runner) == 2);
  auto [first, second, third] = runner.result().value();
  CHECK(first == 1);
  CHECK_FALSE(second.has_value());
  CHECK(third == 3);
}

TEST_CASE("parallel_n fails once success is impossible") {
  auto tree =
      beet::parallel_n<2>(job(1, 1, false), job(2, 5), job(3, 2, false));
  beet::Runner runner{tree, 0};
  CHECK(ticks_to_finish(runner) == 2);
  CHECK(runner.result().error().id == 3);
}

TEST_CASE("children that take no input ignore the parallel input") {
  auto tree = beet::parallel_all(job(1, 1), beet::node([] { return 'x'; }));
  static_assert(std::is_same_v<input_t<decltype(tree)>, int>);
  beet::Runner runner{tree, 5};
  runner.tick();
  CHECK(runner.result().value() == std::tuple{6, 'x'});
}

TEST_CASE("a thread pool executor ticks children concurrently") {
  beet::ThreadPoolExecutor pool(4);
  std::atomic<int> inside{0};
  std::atomic<int> peak{0};
  auto busy = beet::node([&](int id) {
    int now = ++inside;
    for (int seen = peak;
         now > seen && !peak.compare_exchange_weak(seen, now);) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    --inside;
    return id;
  });
  auto tree = beet::parallel_all(pool, busy, busy, busy, busy);
  beet::Runner runner{tree, 7};
  CHECK(runner.tick() == Status::Success);
  CHECK(runner.result().value() == std::tuple{7, 7, 7, 7});
  CHECK(peak > 1);
}

/// Ticks children in reverse order and counts `bulk` calls.
struct ReverseExecutor {
  int calls = 0;

  template <class F>
  void bulk(std::size_t count, const F& fn) {
    ++calls;
    for (std::size_t i = count; i-- > 0;) {
      fn(i);
    }
  }
};
static_assert(beet::executor<ReverseExecutor>);
static_assert(beet::executor<beet::InlineExecutor>);
static_assert(beet::executor<beet::ThreadPoolExecutor>);

/// Cannot be given the capturing lambda a parallel node passes to `bulk`.
struct PointerOnlyExecutor {
  void bulk(std::size_t count, void (*fn)(std::size_t));
};
static_assert(!beet::executor<PointerOnlyExecutor>);

TEST_CASE("any type modeling executor can tick parallel children") {
  ReverseExecutor exec;
  std::string order;
  auto mark = [&order](char id) {
    return beet::node([&order, id] { order += id; });
  };
  auto tree = beet::parallel_all(exec, mark('a'), mark('b'), mark('c'));
  beet::Runner runner{tree};
  CHECK(runner.tick() == Status::Success);
  CHECK(exec.calls == 1);
  CHECK(order == "cba");
}

TEST_CASE("nested parallel nodes on one pool do not deadlock") {
  beet::ThreadPoolExecutor pool(2);
  auto leaf = beet::node([](int value) { return value + 1; });
  auto inner = beet::parallel_all(pool, leaf, leaf, leaf);
  auto tree = beet::parallel_all(pool, inner, inner, inner);
  beet::Runner runner{tree, 0};
  CHECK(runner.tick() == Status::Success);
  CHECK(std::get<2>(runner.result().value()) == std::tuple{1, 1, 1});
}

TEST_CASE("exceptions in pool workers become errors") {
  beet::ThreadPoolExecutor pool(2);
  auto boom = beet::node([](int) -> int { throw std::runtime_error("boom"); });
  auto tree = beet::parallel_all(pool, job(1, 1), boom);
  beet::Runner runner{tree, 0};
  CHECK(runner.tick() == Status::Failure);
  CHECK(std::get<beet::Exception>(runner.result().error()).what() == "boom");
}

TEST_CASE(
    "exceptions from noexcept coroutines in pool workers surface from tick()") {
  beet::ThreadPoolExecutor pool(2);
  auto boom = beet::node([](int) noexcept -> Task<Result<int, never>> {
    throw std::runtime_error("boom");
    co_return 0;
  });
  auto tree = beet::parallel_all(pool, job(1, 1), boom);
  beet::Runner runner{tree, 0};
  CHECK_THROWS_AS(runner.tick(), std::runtime_error);
}
