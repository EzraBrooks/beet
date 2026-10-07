#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <thread>

#include "beet/beet.hpp"

namespace {

using beet::Result;
using beet::Status;

struct Bad {
  int code;
};

template <class R>
Status tick_until_done(R& runner) {
  Status status = runner.tick();
  for (int i = 0; status == Status::Running && i < 100000; ++i) {
    std::this_thread::yield();
    status = runner.tick();
  }
  return status;
}

}  // namespace

TEST_CASE("offload runs a function off the tick and yields its result") {
  auto tree = beet::sequence(beet::offload([](int x) { return x * 2; }),
                             [](int x) { return x + 1; });
  static_assert(std::is_same_v<beet::input_t<decltype(tree)>, int>);
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, beet::never>);

  beet::Runner runner{tree, 20};
  CHECK(tick_until_done(runner) == Status::Success);
  CHECK(runner.result().value() == 41);
}

TEST_CASE("offload propagates typed errors and exceptions") {
  auto failing = beet::offload([](int code) -> Result<int, Bad> {
    return beet::make_unexpected(Bad{code});
  });
  static_assert(std::is_same_v<beet::error_t<decltype(failing)>, Bad>);
  beet::Runner fails{failing, 7};
  CHECK(tick_until_done(fails) == Status::Failure);
  CHECK(fails.result().error().code == 7);

  beet::Runner throws{beet::offload([] { throw std::runtime_error("boom"); }),
                      beet::unit{}};
  CHECK_THROWS_AS(tick_until_done(throws), std::runtime_error);
}

TEST_CASE("offload keeps ticking while the job is still working") {
  std::promise<void> release;
  auto gate = release.get_future().share();
  beet::Runner runner{beet::offload([gate](int x) {
                        gate.wait();
                        return x;
                      }),
                      5};

  for (int i = 0; i < 10; ++i) CHECK(runner.tick() == Status::Running);
  release.set_value();
  CHECK(tick_until_done(runner) == Status::Success);
  CHECK(runner.result().value() == 5);
}

TEST_CASE("halting an offloaded job requests a stop") {
  auto stopped = std::make_shared<std::atomic<bool>>(false);
  beet::Runner runner{beet::offload([stopped](int, std::stop_token stop) {
                        while (!stop.stop_requested())
                          std::this_thread::yield();
                        stopped->store(true);
                        return 0;
                      }),
                      1};

  CHECK(runner.tick() == Status::Running);
  runner.halt();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!stopped->load() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  CHECK(stopped->load());
}
