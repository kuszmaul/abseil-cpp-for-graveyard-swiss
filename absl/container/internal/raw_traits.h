#ifndef ABSL_CONTAINER_INTERNAL_RAW_TRAITS_H_
#define ABSL_CONTAINER_INTERNAL_RAW_TRAITS_H_

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace container_internal {

template <class ContainerKey, class Hash, class Eq>
struct RequireUsableKey {
  template <class PassedKey, class... Args>
  std::pair<
      decltype(std::declval<const Hash&>()(std::declval<const PassedKey&>())),
      decltype(std::declval<const Eq&>()(std::declval<const ContainerKey&>(),
                                         std::declval<const PassedKey&>()))>*
  operator()(const PassedKey&, const Args&...) const;
};

template <class E, class Policy, class Hash, class Eq, class... Ts>
struct IsDecomposable : std::false_type {};

template <class Policy, class Hash, class Eq, class... Ts>
struct IsDecomposable<
    absl::void_t<decltype(Policy::apply(
        RequireUsableKey<typename Policy::key_type, Hash, Eq>(),
        std::declval<Ts>()...))>,
    Policy, Hash, Eq, Ts...> : std::true_type {};

// TODO(alkis): Switch to std::is_nothrow_swappable when gcc/clang supports it.
template <class T>
constexpr bool IsNoThrowSwappable(std::true_type = {} /* is_swappable */) {
  using std::swap;
  return noexcept(swap(std::declval<T&>(), std::declval<T&>()));
}
template <class T>
constexpr bool IsNoThrowSwappable(std::false_type /* is_swappable */) {
  return false;
}

}  // namespace container_internal
ABSL_NAMESPACE_END
}  // namespace absl

#endif  // ABSL_CONTAINER_INTERNAL_RAW_TRAITS_H_
