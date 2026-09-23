module;

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

export module forge.api.core.server_supplied;

export import forge.api.core.exceptions;
export import forge.api.core.trusted_invocation;

import forge.reflect.reflect;

export namespace forge::api::core {

// Specialize only for values whose wire representation is supplied by the server.
template <typename T> struct server_supplied {
   using primary_specialization = void;
};

namespace detail {

template <typename T>
using server_supplied_type = std::remove_cvref_t<T>;

template <typename T>
concept declared_server_supplied = !requires {
   typename server_supplied<T>::primary_specialization;
};

template <typename T>
concept valid_server_supplied = requires(T& value, const trusted_invocation& trusted) {
   { server_supplied<T>::required } -> std::convertible_to<bool>;
   { server_supplied<T>::reset(value) } -> std::same_as<void>;
   { server_supplied<T>::apply(value, trusted) } -> std::same_as<bool>;
};

template <typename T> struct is_fixed_argument_tuple : std::false_type {};

template <typename... T>
struct is_fixed_argument_tuple<std::tuple<T...>> : std::true_type {};

template <typename T>
inline constexpr bool is_fixed_argument_tuple_v =
   is_fixed_argument_tuple<T>::value;

template <typename T> void reset_server_supplied_impl(T& value);
template <typename T> void apply_server_supplied_impl(T& value, const trusted_invocation& trusted);

template <typename T>
consteval void validate_server_supplied() {
   static_assert(valid_server_supplied<T>,
                 "server_supplied specializations require required, reset(T&) and bool apply(T&, const trusted_invocation&)");
}

template <typename T>
void apply_exact_server_supplied(T& value, const trusted_invocation& trusted) {
   validate_server_supplied<T>();
   if (!server_supplied<T>::apply(value, trusted) && server_supplied<T>::required) {
      throw exceptions::server_supplied_unavailable{"required server-supplied value is unavailable"};
   }
}

template <typename T>
void reset_server_supplied_impl(T& value) {
   using value_type = server_supplied_type<T>;
   if constexpr (declared_server_supplied<value_type>) {
      validate_server_supplied<value_type>();
      server_supplied<value_type>::reset(value);
   } else if constexpr (is_fixed_argument_tuple_v<value_type>) {
      std::apply([](auto&... items) { (reset_server_supplied_impl(items), ...); }, value);
   } else if constexpr (forge::reflect::is_described_object_v<value_type>) {
      forge::reflect::for_each_member<value_type>([&](const char*, auto member) {
         reset_server_supplied_impl(value.*member);
      });
   }
}

template <typename T>
void apply_server_supplied_impl(T& value, const trusted_invocation& trusted) {
   using value_type = server_supplied_type<T>;
   if constexpr (declared_server_supplied<value_type>) {
      apply_exact_server_supplied<value_type>(value, trusted);
   } else if constexpr (is_fixed_argument_tuple_v<value_type>) {
      std::apply([&](auto&... items) { (apply_server_supplied_impl(items, trusted), ...); }, value);
   } else if constexpr (forge::reflect::is_described_object_v<value_type>) {
      forge::reflect::for_each_member<value_type>([&](const char*, auto member) {
         apply_server_supplied_impl(value.*member, trusted);
      });
   }
}

} // namespace detail

template <typename T>
concept server_supplied_value = detail::declared_server_supplied<std::remove_cvref_t<T>>;

template <typename T> void reset_server_supplied(T& value) {
   detail::reset_server_supplied_impl(value);
}

template <typename T> void apply_server_supplied(T& value, const trusted_invocation& trusted) {
   detail::apply_server_supplied_impl(value, trusted);
}

} // namespace forge::api::core
