#include <doctest/doctest.h>

#include <optional>
#include <variant>

#include "beet/beet.hpp"

namespace {

using beet::never;
using beet::Result;
using beet::Status;
using beet::Task;

struct Bad {
  int code;
};

using Doubler = beet::Channel<int, Result<int, Bad>>;

}  // namespace

TEST_CASE("call waits for the server's reply, including typed errors") {
  auto call = Doubler::call();
  static_assert(std::is_same_v<beet::input_t<decltype(call)>, beet::Call<int, Result<int, Bad>>>);
  static_assert(std::is_same_v<beet::output_t<decltype(call)>, int>);
  static_assert(std::is_same_v<beet::error_t<decltype(call)>, Bad>);

  Doubler ch;
  CHECK_FALSE(ch.try_receive());

  beet::Runner ok{call, {ch, 21}};
  CHECK(ok.tick() == Status::Running);
  auto req = ch.try_receive();
  REQUIRE(req);
  CHECK(req->value() == 21);
  CHECK(ok.tick() == Status::Running);
  req->reply(req->value() * 2);
  CHECK(ok.tick() == Status::Success);
  CHECK(ok.result().value() == 42);

  beet::Runner fails{call, {ch, 3}};
  fails.tick();
  ch.try_receive()->reply(beet::make_unexpected(Bad{3}));
  CHECK(fails.tick() == Status::Failure);
  CHECK(fails.result().error().code == 3);
}

TEST_CASE("halting a call cancels its request") {
  Doubler ch;
  beet::Runner held{Doubler::call(), {ch, 1}};
  held.tick();
  auto req = ch.try_receive();
  REQUIRE(req);
  CHECK_FALSE(req->cancelled());
  held.halt();
  CHECK(req->cancelled());

  beet::Runner dropped{Doubler::call(), {ch, 2}};
  dropped.tick();
  dropped.halt();
  CHECK_FALSE(ch.try_receive());
}

TEST_CASE("a leaf can create a channel shared by a caller and a server that never finishes") {
  auto server = [](Doubler ch) -> Task<Result<never, never>> {
    for (;;) {
      if (auto req = ch.try_receive()) req->reply(req->value() * 2);
      co_await beet::running;
    }
  };
  auto client = beet::sequence([](Doubler ch) { return beet::Call<int, Result<int, Bad>>{ch, 5}; }, Doubler::call(),
                               [](int x) { return x + 1; });

  auto tree = beet::sequence([] { return Doubler{}; }, beet::parallel_any(client, server));
  static_assert(std::is_same_v<beet::input_t<decltype(tree)>, beet::unit>);

  beet::Runner runner{tree};
  Status status = runner.tick();
  for (int i = 0; status == Status::Running && i < 10; ++i) status = runner.tick();
  REQUIRE(status == Status::Success);
  CHECK(std::get<0>(runner.result().value()) == 11);
}
