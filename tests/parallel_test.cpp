#include <doctest/doctest.h>

#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
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
    if (halted && !finished) ++*halted;
  }
};

/// Succeeds with `base + id` (or fails with Failed{id}) on its `ticks`-th tick.
auto job(int id, int ticks, bool succeed = true, int* halted = nullptr) {
  return beet::node([=](int base) -> Task<Result<int, Failed>> {
    HaltProbe probe{halted};
    for (int i = 1; i < ticks; ++i) co_await beet::running;
    probe.finished = true;
    if (!succeed) co_return beet::make_unexpected(Failed{id});
    co_return base + id;
  });
}

template <class N>
int ticks_to_finish(beet::Runner<N>& r) {
  int ticks = 1;
  while (r.tick() == Status::Running) ++ticks;
  return ticks;
}

}  // namespace

TEST_CASE("parallel_all ticks children together and returns a tuple") {
  auto tree = beet::parallel_all(job(1, 3), job(2, 1), beet::node([](int x) { return std::to_string(x); }));
  static_assert(std::is_same_v<input_t<decltype(tree)>, int>);
  static_assert(std::is_same_v<output_t<decltype(tree)>, std::tuple<int, int, std::string>>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, Failed>);

  beet::Runner r{tree, 100};
  CHECK(ticks_to_finish(r) == 3);
  CHECK(r.result().value() == std::tuple{101, 102, std::string("100")});
}

TEST_CASE("parallel_all fails fast and halts the other children") {
  int halted = 0;
  auto tree = beet::parallel_all(job(1, 5, true, &halted), job(2, 2, false));
  beet::Runner r{tree, 0};
  CHECK(ticks_to_finish(r) == 2);
  CHECK(r.result().error().id == 2);
  CHECK(halted == 1);
}

TEST_CASE("parallel_any returns the first success as a child-indexed variant") {
  int halted = 0;
  auto tree = beet::parallel_any(job(1, 4, true, &halted), job(2, 2), job(3, 1, false));
  static_assert(std::is_same_v<output_t<decltype(tree)>, std::variant<int, int, int>>);

  beet::Runner r{tree, 10};
  CHECK(ticks_to_finish(r) == 2);
  CHECK(r.result().value().index() == 1);
  CHECK(std::get<1>(r.result().value()) == 12);
  CHECK(halted == 1);
}

TEST_CASE("parallel_any fails with the last child's error when all fail") {
  auto tree = beet::parallel_any(job(1, 1, false), job(2, 3, false));
  beet::Runner r{tree, 0};
  CHECK(ticks_to_finish(r) == 3);
  CHECK(r.result().error().id == 2);
}

TEST_CASE("parallel_n succeeds once K children succeed") {
  auto tree = beet::parallel_n<2>(job(1, 1), job(2, 5), job(3, 2));
  static_assert(std::is_same_v<output_t<decltype(tree)>,
                               std::tuple<std::optional<int>, std::optional<int>, std::optional<int>>>);
  beet::Runner r{tree, 0};
  CHECK(ticks_to_finish(r) == 2);
  auto [a, b, c] = r.result().value();
  CHECK(a == 1);
  CHECK_FALSE(b.has_value());
  CHECK(c == 3);
}

TEST_CASE("parallel_n fails once success is impossible") {
  auto tree = beet::parallel_n<2>(job(1, 1, false), job(2, 5), job(3, 2, false));
  beet::Runner r{tree, 0};
  CHECK(ticks_to_finish(r) == 2);
  CHECK(r.result().error().id == 3);
}

TEST_CASE("children that take no input ignore the parallel input") {
  auto tree = beet::parallel_all(job(1, 1), beet::node([] { return 'x'; }));
  static_assert(std::is_same_v<input_t<decltype(tree)>, int>);
  beet::Runner r{tree, 5};
  r.tick();
  CHECK(r.result().value() == std::tuple{6, 'x'});
}

TEST_CASE("a thread pool executor ticks children concurrently") {
  beet::ThreadPoolExecutor pool(4);
  std::atomic<int> inside{0};
  std::atomic<int> peak{0};
  auto busy = beet::node([&](int id) {
    int now = ++inside;
    for (int seen = peak; now > seen && !peak.compare_exchange_weak(seen, now);) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    --inside;
    return id;
  });
  auto tree = beet::parallel_all(pool, busy, busy, busy, busy);
  beet::Runner r{tree, 7};
  CHECK(r.tick() == Status::Success);
  CHECK(r.result().value() == std::tuple{7, 7, 7, 7});
  CHECK(peak > 1);
}

TEST_CASE("nested parallel nodes on one pool do not deadlock") {
  beet::ThreadPoolExecutor pool(2);
  auto leaf = beet::node([](int x) { return x + 1; });
  auto inner = beet::parallel_all(pool, leaf, leaf, leaf);
  auto tree = beet::parallel_all(pool, inner, inner, inner);
  beet::Runner r{tree, 0};
  CHECK(r.tick() == Status::Success);
  CHECK(std::get<2>(r.result().value()) == std::tuple{1, 1, 1});
}

TEST_CASE("exceptions in pool workers surface from tick()") {
  beet::ThreadPoolExecutor pool(2);
  auto boom = beet::node([](int) -> int { throw std::runtime_error("boom"); });
  auto tree = beet::parallel_all(pool, job(1, 1), boom);
  beet::Runner r{tree, 0};
  CHECK_THROWS_AS(r.tick(), std::runtime_error);
}
