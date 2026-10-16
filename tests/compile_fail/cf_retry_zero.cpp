#include "beet/beet.hpp"

int main() {
  auto tree = beet::retry<0>([](int x) { return x; });
  (void)tree;
}
