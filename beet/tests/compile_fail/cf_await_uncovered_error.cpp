#include "beet/beet.hpp"

struct ChildError {};
struct ParentError {};

beet::Task<beet::Result<int, ChildError>> child(int x) { co_return x; }

beet::Task<beet::Result<int, ParentError>> parent(int x) {
  co_return co_await child(x);
}

int main() {
  beet::Runner r{beet::node(parent), 1};
  r.tick();
}
