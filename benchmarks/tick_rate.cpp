// Ticks small trees for a fixed time and fails if the best observed tick rate
// of the mixed tree falls below a minimum.
//
// Usage: tick_rate [min_ticks_per_second]

#include <chrono>
#include <cstdlib>
#include <format>
#include <iostream>

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

int step(int x) noexcept { return x + 1; }

auto make_mixed_tree() {
  return beet::recover<TooFar>(beet::sequence(check, approach),
                               [](TooFar f) { return f.at; });
}

// Every node is a plain function, so the whole tree finishes in one tick.
auto make_sync_tree() {
  return beet::recover<TooFar>(beet::sequence(check, step, step, step),
                               [](TooFar f) { return f.at; });
}

// Restarts the tree whenever it finishes, so frame allocation is part of the
// measured cost.
template <class Tree>
double measure_ticks_per_second(const Tree& tree,
                                std::chrono::duration<double> budget,
                                long long& sink) {
  using clock = std::chrono::steady_clock;
  beet::Runner runner{tree, 0};
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

template <class Tree>
double best_rate(const char* name, const Tree& tree, long long& sink) {
  constexpr int trials = 5;
  measure_ticks_per_second(tree, std::chrono::milliseconds(100), sink);
  double best = 0;
  for (int t = 0; t < trials; ++t) {
    const double rate =
        measure_ticks_per_second(tree, std::chrono::milliseconds(200), sink);
    std::cout << std::format("{} trial {}: {:.3g} ticks/s\n", name, t + 1,
                             rate);
    if (rate > best) best = rate;
  }
  std::cout << std::format("{} best: {:.3g} ticks/s ({:.1f} ns/tick)\n", name,
                           best, 1e9 / best);
  return best;
}

}  // namespace

int main(int argc, char** argv) {
  const double minimum = argc > 1 ? std::atof(argv[1]) : 0;

  long long sink = 0;
  const double best = best_rate("mixed", make_mixed_tree(), sink);
  best_rate("sync", make_sync_tree(), sink);
  std::cout << std::format("minimum (mixed): {:.3g} ticks/s, checksum {}\n",
                           minimum, sink);

  if (best < minimum) {
    std::cerr << "FAIL: tick rate is below the minimum\n";
    return EXIT_FAILURE;
  }
}
