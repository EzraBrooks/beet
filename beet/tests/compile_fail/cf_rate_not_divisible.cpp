#include "beet/beet.hpp"

int main() {
  auto tree = beet::rate<10>(beet::rate<3>([](int x) { return x; }));
  (void)tree;
}
