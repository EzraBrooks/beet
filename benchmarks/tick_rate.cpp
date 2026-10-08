// Ticks a small tree for a fixed time and fails if the best observed tick rate
// falls below a minimum.
//
// Usage: tick_rate [min_ticks_per_second]

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "beet/beet.hpp"

namespace {

using beet::Result;
using beet::Task;

struct TooFar {
  int at;
};

Result<int, TooFar> check(int x) {
  if (x > 1000) return beet::make_unexpected(TooFar{x});
  return x;
}

Task<Result<int, beet::never>> approach(int x) {
  for (int i = 0; i < 3; ++i) {
    co_await beet::running;
    ++x;
  }
  co_return x;
}

auto make_tree() {
  return beet::recover<TooFar>(beet::sequence(check, approach),
                               [](TooFar f) { return f.at; });
}

// Restarts the tree whenever it finishes, so frame allocation is part of the
// measured cost.
double measure_ticks_per_second(std::chrono::duration<double> budget,
                                long long& sink) {
  using clock = std::chrono::steady_clock;
  beet::Runner runner{make_tree(), 0};
  long long ticks = 0;
  int input = 0;
  const auto start = clock::now();
  auto now = start;
  while (now - start < budget) {
    for (int i = 0; i < 1024; ++i) {
      ++ticks;
      if (runner.tick() != beet::Status::Running) {
        sink += *runner.result();
        runner.reset(++input & 2047);
      }
    }
    now = clock::now();
  }
  return static_cast<double>(ticks) /
         std::chrono::duration<double>(now - start).count();
}

}  // namespace

int main(int argc, char** argv) {
  const double minimum = argc > 1 ? std::atof(argv[1]) : 0;
  constexpr int trials = 5;

  long long sink = 0;
  measure_ticks_per_second(std::chrono::milliseconds(100), sink);
  double best = 0;
  for (int t = 0; t < trials; ++t) {
    const double rate =
        measure_ticks_per_second(std::chrono::milliseconds(200), sink);
    std::printf("trial %d: %.3g ticks/s\n", t + 1, rate);
    if (rate > best) best = rate;
  }
  std::printf(
      "best: %.3g ticks/s (%.1f ns/tick), minimum: %.3g ticks/s, checksum "
      "%lld\n",
      best, 1e9 / best, minimum, sink);

  if (best < minimum) {
    std::printf("FAIL: tick rate is below the minimum\n");
    return EXIT_FAILURE;
  }
}
