#include "beet/beet.hpp"

int main() {
  beet::Runner runner{beet::node([](int x) { return x; }), 1};
  (void)runner.run();
}
