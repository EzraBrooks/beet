#include "beet/beet.hpp"

struct Known {};
struct Unknown {};

int main() {
  auto tree = beet::recover<Unknown>(
      [](int x) -> beet::Result<int, Known> { return x; },
      [](Unknown) { return 0; });
  (void)tree;
}
