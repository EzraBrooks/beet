#include "beet/beet.hpp"

int main() {
  beet::Runner r{
      beet::rate<1>(beet::rate<600>(beet::repeat([]() noexcept {})))};
  r.tick();
}
