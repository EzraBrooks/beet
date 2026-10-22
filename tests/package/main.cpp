// Smoke test for the installed CMake package. It is deliberately small: the
// point is to prove that find_package(beet) gives you usable headers and pulls
// in tl::expected and Threads, not to retest behavior the main suite already
// covers.

#include <format>
#include <iostream>
#include <string>

#include "beet/beet.hpp"

namespace {

struct TooSmall {
  int value;
};

// Spans a tick, so the consumer exercises the coroutine machinery too.
beet::Task<beet::Result<int, TooSmall>> check(int value) {
  co_await beet::running;
  if (value < 10) {
    co_return beet::make_unexpected(TooSmall{value});
  }
  co_return value;
}

std::string describe(int value) { return "ok: " + std::to_string(value); }

std::string run(int input) {
  // offload exercises the Threads dependency; the recover arms exercise
  // tl::expected.
  const auto tree = beet::recover(
      beet::recover<TooSmall>(
          beet::sequence(beet::offload([](int value) { return value * 4; }),
                         check, describe),
          [](TooSmall error) {
            return "too small: " + std::to_string(error.value);
          }),
      [](const beet::Exception& error) { return "failed: " + error.what(); });

  beet::Runner runner{tree, input};
  while (runner.tick() == beet::Status::Running) {
  }
  return *runner.result();
}

}  // namespace

int main() {
  const std::string ok = run(4);
  const std::string recovered = run(2);
  std::cout << std::format("{} / {}\n", ok, recovered);
  return (ok == "ok: 16" && recovered == "too small: 8") ? 0 : 1;
}
