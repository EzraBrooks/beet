// Runs a tree without ever attaching listeners. CTest checks that none of
// beet's tracing symbols end up in this binary.

#include <chrono>
#include <string>
#include <tuple>
#include <type_traits>

#include "beet/beet.hpp"

namespace {

struct TooFar {
  int steps;
};
struct Tired {
  int steps;
};

beet::Result<int, TooFar> plan(int steps) {
  if (steps > 3) {
    return beet::make_unexpected(TooFar{steps});
  }
  return steps;
}

beet::Task<beet::Result<int, beet::never>> walk(int steps) {
  for (int i = 0; i < steps; ++i) {
    co_await beet::running;
  }
  co_return steps;
}

beet::Task<beet::Result<int, Tired>> stamina(int steps) {
  co_await beet::running;
  if (steps > 2) {
    co_return beet::make_unexpected(Tired{steps});
  }
  co_return steps;
}

std::string report(std::tuple<int, int> done) {
  return "walked " + std::to_string(std::get<0>(done));
}

std::string describe(const TooFar& error) {
  return "too far: " + std::to_string(error.steps);
}
std::string describe(const beet::Exception& error) {
  return "failed: " + error.what();
}

std::string run(int steps) {
  using namespace std::chrono_literals;
  const auto tree = beet::rate<10>(beet::recover(
      beet::recover<Tired>(
          beet::sequence(beet::named<"plan">(plan),
                         beet::named<"walk">(beet::parallel_all(walk, stamina)),
                         beet::named<"report">(report)),
          [](Tired error) {
            return "tired after " + std::to_string(error.steps);
          }),
      [](const auto& error) noexcept { return describe(error); }));
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, beet::never>);

  beet::Runner runner{tree, steps};
  runner.run(10ms);  // NOLINT(misc-include-cleaner)
  return *runner.result();
}

}  // namespace

int main() {
  return run(2) == "walked 2" && run(3) == "tired after 3" &&
                 run(5) == "too far: 5"
             ? 0
             : 1;
}
