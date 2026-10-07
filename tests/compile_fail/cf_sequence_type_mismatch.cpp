#include <string>

#include "beet/beet.hpp"

int main() {
  auto tree = beet::sequence([](int x) { return std::to_string(x); }, [](int x) { return x; });
  (void)tree;
}
