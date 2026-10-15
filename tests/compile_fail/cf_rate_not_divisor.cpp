#include "beet/beet.hpp"

int main() {
  beet::Runner r{
      beet::rate<600>(beet::rate<7>(beet::repeat([]() noexcept {})))};
  r.tick();
}
