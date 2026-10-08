#include <string>

#include "beet/beet.hpp"

static_assert(!BEET_EXCEPTIONS);

namespace {

struct Bad {};

beet::Result<int, Bad> check(int x) {
  if (x < 0) return beet::make_unexpected(Bad{});
  return x;
}

beet::Task<beet::Result<int, beet::never>> slow(int x) {
  co_await beet::running;
  co_return x + 1;
}

}  // namespace

int main() {
  auto tree = beet::recover(
      beet::sequence(check, slow, [](int x) { return std::to_string(x); }),
      [](Bad) { return std::string("bad"); });
  static_assert(std::is_same_v<beet::error_t<decltype(tree)>, beet::never>);

  beet::Runner ok{tree, 1};
  while (ok.tick() == beet::Status::Running) {
  }
  beet::Runner bad{tree, -1};
  while (bad.tick() == beet::Status::Running) {
  }
  return ok.result().value() == "2" && bad.result().value() == "bad" ? 0 : 1;
}
