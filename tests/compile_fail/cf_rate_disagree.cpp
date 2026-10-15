#include "beet/beet.hpp"

int main() {
  beet::Runner r{beet::sequence(beet::rate<100>([]() noexcept {}),
                                beet::rate<50>([]() noexcept {}))};
  r.tick();
}
