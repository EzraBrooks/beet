#include "beet/beet.hpp"

struct Known {};
struct Unknown {};

int main() {
  auto tree = beet::node([](int x) -> beet::Result<int, Known> { return x; }).recover<Unknown>([](Unknown) {
    return 0;
  });
  (void)tree;
}
