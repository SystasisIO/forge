module;

#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.contract.action;

export import forge.chain.protocol.action;
export import forge.chain.protocol.code_hash_result;
export import forge.chain.protocol.time;
export import forge.contract.fixed_bytes;

import forge.contract.datastream;
import forge.contract.intrinsics;
import forge.contract.ignore;
import forge.contract.varint;

export namespace forge::contract {

namespace detail {

template <typename T> struct action_argument {
   using type = T;
};
template <typename T> struct action_argument<ignore<T>> {
   using type = ignore_wrapper<T>;
};

template <typename> struct action_method;
template <typename Class, typename Result, typename... Args> struct action_method<Result (Class::*)(Args...)> {
   using arguments = std::tuple<typename action_argument<std::decay_t<Args>>::type...>;
};
template <typename Class, typename Result, typename... Args>
struct action_method<Result (Class::*)(Args...) const> : action_method<Result (Class::*)(Args...)> {};

template <std::size_t Index, auto First, auto... Rest> consteval auto nth_action() {
   static_assert(Index < 1U + sizeof...(Rest), "variant action index is out of range");
   if constexpr (Index == 0U) {
      return First;
   } else {
      return nth_action<Index - 1U, Rest...>();
   }
}

} // namespace detail

using chain::protocol::account_name;
using chain::protocol::action_name;
using chain::protocol::name;
using chain::protocol::permission_level;

using chain::protocol::code_hash_result;

template <typename T> [[nodiscard]] T unpack_action_data() {
   const auto size = action_data_size();
   auto bytes = std::vector<std::uint8_t>(size);
   if (size != 0U) {
      check(read_action_data(bytes.data(), size) == size, "failed to read complete action data");
   }
   return ::forge::raw::unpack_exact<T>(bytes);
}

void require_recipient(name account);

template <typename... Accounts> void require_recipient(name account, Accounts... remaining) {
   require_recipient(account);
   (require_recipient(remaining), ...);
}

void require_auth(name account);
void require_auth(const permission_level& permission);
[[nodiscard]] bool has_auth(name account);
[[nodiscard]] bool is_account(name account);
[[nodiscard]] chain::protocol::time_point publication_time();
checksum256 get_code_hash(name account, code_hash_result* full_result = nullptr);

struct action : chain::protocol::action {
   action() = default;
   using chain::protocol::action::action;

   explicit action(chain::protocol::action value) : chain::protocol::action(std::move(value)) {}

   template <typename T> [[nodiscard]] T data_as() const {
      return ::forge::raw::unpack_exact<T>(data);
   }

   void send() const;
   void send_context_free() const;
};

template <typename Stream> void raw_pack(Stream& stream, const action& value) {
   chain::protocol::raw_pack(stream, static_cast<const chain::protocol::action&>(value));
}

template <typename Stream> void raw_unpack(Stream& stream, action& value) {
   chain::protocol::raw_unpack(stream, static_cast<chain::protocol::action&>(value));
}

template <name::raw Name, auto Method> struct action_wrapper {
   template <typename Code>
   constexpr action_wrapper(Code&& code, std::vector<permission_level> permissions = {})
       : code(std::forward<Code>(code)), permissions(std::move(permissions)) {}

   template <typename Code>
   constexpr action_wrapper(Code&& code, permission_level permission)
       : code(std::forward<Code>(code)), permissions{permission} {}

   static constexpr name action_name{Name};
   static constexpr auto get_mem_ptr() noexcept {
      return Method;
   }
   name code{};
   std::vector<permission_level> permissions;

   template <typename... Args> [[nodiscard]] action to_action(Args&&... args) const {
      using arguments = typename detail::action_method<decltype(Method)>::arguments;
      static_assert(std::constructible_from<arguments, Args...>, "inline action arguments do not match the method");
      return action{permissions, code, action_name, arguments{std::forward<Args>(args)...}};
   }

   template <typename... Args> void send(Args&&... args) const {
      to_action(std::forward<Args>(args)...).send();
   }

   template <typename... Args> void send_context_free(Args&&... args) const {
      to_action(std::forward<Args>(args)...).send_context_free();
   }
};

template <name::raw Name, auto... Methods> struct variant_action_wrapper {
   template <typename Code>
   constexpr variant_action_wrapper(Code&& code, std::vector<permission_level> permissions)
       : code(std::forward<Code>(code)), permissions(std::move(permissions)) {}

   template <typename Code>
   constexpr variant_action_wrapper(Code&& code, permission_level permission)
       : code(std::forward<Code>(code)), permissions{permission} {}

   static constexpr name action_name{Name};
   name code{};
   std::vector<permission_level> permissions;

   template <std::size_t Variant> static consteval auto get_mem_ptr() {
      return detail::nth_action<Variant, Methods...>();
   }

   template <std::size_t Variant, typename... Args> [[nodiscard]] action to_action(Args&&... args) const {
      constexpr auto method = get_mem_ptr<Variant>();
      using arguments = typename detail::action_method<std::remove_cv_t<decltype(method)>>::arguments;
      static_assert(std::constructible_from<arguments, Args...>, "variant action arguments do not match the method");
      return action{permissions, code, action_name,
                    std::tuple_cat(std::tuple{unsigned_int{static_cast<std::uint32_t>(Variant)}},
                                   arguments{std::forward<Args>(args)...})};
   }

   template <std::size_t Variant, typename... Args> void send(Args&&... args) const {
      to_action<Variant>(std::forward<Args>(args)...).send();
   }

   template <std::size_t Variant, typename... Args> void send_context_free(Args&&... args) const {
      to_action<Variant>(std::forward<Args>(args)...).send_context_free();
   }
};

template <typename... Args>
void dispatch_inline(name code, name action_name, std::vector<permission_level> permissions,
                     std::tuple<Args...> arguments) {
   action{std::move(permissions), code, action_name, std::move(arguments)}.send();
}

template <typename, name::raw> struct inline_dispatcher;

template <typename Contract, name::raw Name, typename... Args>
struct inline_dispatcher<void (Contract::*)(Args...), Name> {
   static void call(name code, const permission_level& permission, std::tuple<Args...> arguments) {
      dispatch_inline(code, name{Name}, std::vector<permission_level>{permission}, std::move(arguments));
   }

   static void call(name code, std::vector<permission_level> permissions, std::tuple<Args...> arguments) {
      dispatch_inline(code, name{Name}, std::move(permissions), std::move(arguments));
   }
};

} // namespace forge::contract
