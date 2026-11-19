#include "beet/beet.hpp"

int main() {
  auto tree = beet::rate<2>([](int x) { return x; });
  auto task = tree(1);
  (void)task;
}
