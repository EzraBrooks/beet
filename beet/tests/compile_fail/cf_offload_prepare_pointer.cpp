#include "beet/beet.hpp"

int main() {
  static int shared = 0;
  auto tree = beet::offload([] { return &shared; }, [](int* p) { return *p; });
  (void)tree;
}
