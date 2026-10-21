#include "beet/beet.hpp"

int main() {
  beet::AnyNode<int, int, beet::Exception> tree =
      beet::rate<2>([](int x) { return x; });
  (void)tree;
}
