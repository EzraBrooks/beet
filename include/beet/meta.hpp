#pragma once

#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <utility>
#include <variant>

#include "beet/result.hpp"

namespace beet {

template <class... Ts>
struct type_list {};

namespace detail {

template <class T>
struct is_variant : std::false_type {};
template <class... Ts>
struct is_variant<std::variant<Ts...>> : std::true_type {};
template <class T>
inline constexpr bool is_variant_v = is_variant<T>::value;

template <class T>
struct is_result : std::false_type {};
template <class T, class E>
struct is_result<tl::expected<T, E>> : std::true_type {};
template <class T>
inline constexpr bool is_result_v = is_result<T>::value;

template <class T, class List>
struct contains;
template <class T, class... Ts>
struct contains<T, type_list<Ts...>>
    : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};
template <class T, class List>
inline constexpr bool contains_v = contains<T, List>::value;

template <class... Lists>
struct concat {
  using type = type_list<>;
};
template <class... As>
struct concat<type_list<As...>> {
  using type = type_list<As...>;
};
template <class... As, class... Bs, class... Rest>
struct concat<type_list<As...>, type_list<Bs...>, Rest...>
    : concat<type_list<As..., Bs...>, Rest...> {};

template <class Done, class Todo>
struct dedup {
  using type = Done;
};
template <class... Ds, class T, class... Ts>
struct dedup<type_list<Ds...>, type_list<T, Ts...>>
    : dedup<std::conditional_t<contains_v<T, type_list<Ds...>>,
                               type_list<Ds...>, type_list<Ds..., T>>,
            type_list<Ts...>> {};

// An error set is spelled as `never` (empty), a single type, or a flat
// `std::variant` of 2+ types.
template <class E>
struct to_list {
  using type = type_list<E>;
};
template <>
struct to_list<never> {
  using type = type_list<>;
};
template <class... Ts>
struct to_list<std::variant<Ts...>> : concat<typename to_list<Ts>::type...> {};

template <class List>
struct from_list;
template <>
struct from_list<type_list<>> {
  using type = never;
};
template <class T>
struct from_list<type_list<T>> {
  using type = T;
};
template <class T, class U, class... Ts>
struct from_list<type_list<T, U, Ts...>> {
  using type = std::variant<T, U, Ts...>;
};

template <class List, class Remove>
struct remove_all;
template <class... Ts, class Remove>
struct remove_all<type_list<Ts...>, Remove>
    : concat<std::conditional_t<contains_v<Ts, Remove>, type_list<>,
                                type_list<Ts>>...> {};

template <class Sub, class Super>
struct is_subset;
template <class... Ts, class Super>
struct is_subset<type_list<Ts...>, Super>
    : std::bool_constant<(contains_v<Ts, Super> && ...)> {};

}  // namespace detail

/// The members of a type set, with nested variants flattened and `never`
/// dropped.
template <class E>
using set_list_t = typename detail::to_list<E>::type;

/// Flattened, deduplicated union of type sets. Collapses to `never`, a single
/// type, or a `std::variant`.
template <class... Es>
using union_t = typename detail::from_list<typename detail::dedup<
    type_list<>, typename detail::concat<set_list_t<Es>...>::type>::type>::type;

template <class... Es>
using error_union_t = union_t<Es...>;

template <class E, class... Remove>
using error_remove_t = typename detail::from_list<typename detail::remove_all<
    set_list_t<E>, type_list<Remove...>>::type>::type;

template <class Sub, class Super>
inline constexpr bool error_subset_v =
    detail::is_subset<set_list_t<Sub>, set_list_t<Super>>::value;

template <class E, class Set>
inline constexpr bool error_contains_v = detail::contains_v<E, set_list_t<Set>>;

namespace detail {

/// Converts a value into a wider type: a member into its set, or a smaller set
/// into a larger one.
template <class To, class From>
To coerce(From&& value) {
  using F = std::remove_cvref_t<From>;
  if constexpr (std::is_same_v<F, never>) {
    std::abort();
  } else if constexpr (std::is_same_v<F, To>) {
    return std::forward<From>(value);
  } else if constexpr (is_variant_v<To> && contains_v<F, set_list_t<To>>) {
    return To(std::in_place_type<F>, std::forward<From>(value));
  } else if constexpr (is_variant_v<F>) {
    return std::visit(
        [](auto&& alt) -> To {
          return coerce<To>(std::forward<decltype(alt)>(alt));
        },
        std::forward<From>(value));
  } else {
    return To(std::forward<From>(value));
  }
}

template <class F>
struct callable_traits : callable_traits<decltype(&F::operator())> {};
template <class R, class... A>
struct callable_traits<R(A...)> {
  using result = R;
  using args = type_list<A...>;
};
template <class R, class... A>
struct callable_traits<R(A...) noexcept> : callable_traits<R(A...)> {};
template <class R, class... A>
struct callable_traits<R (*)(A...)> : callable_traits<R(A...)> {};
template <class R, class... A>
struct callable_traits<R (*)(A...) noexcept> : callable_traits<R(A...)> {};
template <class C, class R, class... A>
struct callable_traits<R (C::*)(A...) const> : callable_traits<R(A...)> {};
template <class C, class R, class... A>
struct callable_traits<R (C::*)(A...) const noexcept>
    : callable_traits<R(A...)> {};

template <class Args>
struct input_from_args {
  static_assert(sizeof(Args) == 0,
                "beet: a node callable must take zero or one argument");
};
template <>
struct input_from_args<type_list<>> {
  using type = unit;
};
template <class A>
struct input_from_args<type_list<A>> {
  using type = std::decay_t<A>;
};

/// Input type of a non-generic callable: its single argument, or `unit` if it
/// takes none.
template <class F>
using callable_input_t =
    typename input_from_args<typename callable_traits<F>::args>::type;

}  // namespace detail
}  // namespace beet
