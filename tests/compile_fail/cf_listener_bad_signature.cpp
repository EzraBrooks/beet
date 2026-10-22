#include <string>

#include "beet/beet.hpp"
#include "beet/observe.hpp"

int main() {
  beet::Runner runner{
      beet::node([](int x) { return x; }), 1,
      beet::Listeners{.on_start = [](const std::string& /*unused*/) {}}};
  (void)runner.tick();
}
