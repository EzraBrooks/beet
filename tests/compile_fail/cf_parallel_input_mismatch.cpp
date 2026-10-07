#include <string>

#include "beet/beet.hpp"

int main() {
  auto tree = beet::parallel_all([](int x) { return x; },
                                 [](std::string s) { return s; });
  (void)tree;
}
