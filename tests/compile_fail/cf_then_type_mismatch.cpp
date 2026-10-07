#include <string>

#include "beet/beet.hpp"

int main() {
  auto tree = beet::node([](int x) { return std::to_string(x); }).then([](int x) { return x; });
  (void)tree;
}
