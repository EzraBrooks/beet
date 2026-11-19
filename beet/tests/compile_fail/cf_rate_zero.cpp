#include "beet/beet.hpp"

int main() {
  auto tree = beet::rate<0>([](int x) { return x; });
  (void)tree;
}
