#include <doctest/doctest.h>

#include <string>
#include <variant>

#include "beet/meta.hpp"

namespace {

struct A {};
struct B {};
struct C {};

using beet::error_remove_t;
using beet::error_subset_v;
using beet::error_union_t;
using beet::never;

static_assert(std::is_same_v<error_union_t<>, never>);
static_assert(std::is_same_v<error_union_t<never, never>, never>);
static_assert(std::is_same_v<error_union_t<A, never>, A>);
static_assert(std::is_same_v<error_union_t<A, A>, A>);
static_assert(std::is_same_v<error_union_t<A, B>, std::variant<A, B>>);
static_assert(
    std::is_same_v<error_union_t<std::variant<A, B>, std::variant<B, C>>,
                   std::variant<A, B, C>>);

static_assert(std::is_same_v<error_remove_t<std::variant<A, B, C>, B>,
                             std::variant<A, C>>);
static_assert(std::is_same_v<error_remove_t<std::variant<A, B>, A>, B>);
static_assert(std::is_same_v<error_remove_t<A, A>, never>);

static_assert(error_subset_v<never, A>);
static_assert(error_subset_v<A, std::variant<A, B>>);
static_assert(error_subset_v<std::variant<B, A>, std::variant<A, B, C>>);
static_assert(!error_subset_v<C, std::variant<A, B>>);

}  // namespace

TEST_CASE(
    "coerce widens a member into its set and a smaller set into a larger one") {
  using Set = std::variant<A, B, C>;
  CHECK(beet::detail::coerce<Set>(B{}).index() == 1);
  CHECK(beet::detail::coerce<Set>(std::variant<C, A>{C{}}).index() == 2);
  CHECK(beet::detail::coerce<std::string>("text") == "text");
}
