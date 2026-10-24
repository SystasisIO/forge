module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>
#include <tuple>
#include <type_traits>
#include <utility>

export module forge.contract.dispatcher;

export import forge.contract;
export import forge.contract.call;
export import forge.raw.codec;

export namespace forge::contract {

// Opt in as Contract::decode_policy. Contracts without this member retain CDT action decoding.
struct bounded_decode_policy {
   std::uint32_t max_action_bytes = 16U * 1024U;
   std::uint32_t max_call_bytes = 16U * 1024U;
   forge::raw::bounded_unpack_limits unpack{.allocations = {.max_container_elements = 1024U,
                                                            .max_total_container_elements = 4096U,
                                                            .max_bytes = 16U * 1024U,
                                                            .first_container_elements = 1024U},
                                            .max_total_bytes = 16U * 1024U,
                                            .reject_noncanonical_varints = true,
                                            .reject_unknown_enum_values = true};
};

namespace detail {

inline constexpr auto local_buffer_size = std::size_t{512};

template <typename Contract>
inline constexpr bool has_bounded_decode_policy_v = requires { Contract::decode_policy; };

template <typename Contract> auto invocation_input_stream(const std::uint8_t* data, std::size_t size) {
   if constexpr (has_bounded_decode_policy_v<Contract>) {
      static_assert(std::is_same_v<std::remove_cv_t<decltype(Contract::decode_policy)>, bounded_decode_policy>,
                    "Contract::decode_policy must be a constexpr bounded_decode_policy");
      return forge::raw::make_input_stream(data, size, Contract::decode_policy.unpack);
   } else {
      return forge::datastream<const std::uint8_t*>{data, size};
   }
}

template <typename Contract> constexpr std::uint32_t call_byte_limit() {
   if constexpr (has_bounded_decode_policy_v<Contract>) {
      return Contract::decode_policy.max_call_bytes;
   } else {
      return std::numeric_limits<std::uint32_t>::max();
   }
}

class invocation_buffer {
 public:
   explicit invocation_buffer(std::size_t size) : size_(size) {
      if (size_ > local_.size()) {
         data_ = static_cast<std::uint8_t*>(std::malloc(size_));
         check(data_ != nullptr, "contract invocation buffer allocation failed");
      }
   }

   invocation_buffer(const invocation_buffer&) = delete;
   invocation_buffer& operator=(const invocation_buffer&) = delete;

   ~invocation_buffer() {
      if (data_ != local_.data()) {
         std::free(data_);
      }
   }

   [[nodiscard]] std::uint8_t* data() noexcept {
      return data_;
   }

   [[nodiscard]] const std::uint8_t* data() const noexcept {
      return data_;
   }

   [[nodiscard]] std::size_t size() const noexcept {
      return size_;
   }

 private:
   std::array<std::uint8_t, local_buffer_size> local_{};
   std::uint8_t* data_ = local_.data();
   std::size_t size_ = 0;
};

template <typename Contract> void set_action_execution_type(Contract& instance) {
   if constexpr (requires { typename Contract::exec_type_t; }) {
      using exec_type = typename Contract::exec_type_t;
      if constexpr (requires { instance.set_exec_type(exec_type::action); }) {
         instance.set_exec_type(exec_type::action);
      }
   }
}

template <typename Contract> void set_call_execution_type(Contract& instance) {
   if constexpr (requires { typename Contract::exec_type_t; }) {
      using exec_type = typename Contract::exec_type_t;
      if constexpr (requires { instance.set_exec_type(exec_type::call); }) {
         instance.set_exec_type(exec_type::call);
      }
   }
}

template <typename Result, typename Setter>
void set_packed_result(const Result& result, const char* size_error, Setter&& setter) {
   auto bytes = forge::raw::pack(result);
   check(bytes.size() <= std::numeric_limits<std::uint32_t>::max(), size_error);
   setter(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
}

template <typename Contract, typename Result, typename... Arguments, typename Method>
void execute_action_impl(chain::protocol::name self, chain::protocol::name first_receiver, Method method) {
   const auto size = action_data_size();
   if constexpr (has_bounded_decode_policy_v<Contract>) {
      check(size <= Contract::decode_policy.max_action_bytes, "action data exceeds contract decode byte limit");
   }
   auto bytes = invocation_buffer{size};
   if (size != 0U) {
      check(read_action_data(bytes.data(), size) == size, "failed to read complete action data");
   }

   auto arguments = std::tuple<std::decay_t<Arguments>...>{};
   auto stream = invocation_input_stream<Contract>(bytes.data(), bytes.size());
   forge::raw::unpack(stream, arguments);
   if constexpr (has_bounded_decode_policy_v<Contract>) {
      check(stream.remaining() == 0U, "action data contains trailing bytes");
   }

   auto contract_stream = typename Contract::stream_type{
       reinterpret_cast<const char*>(bytes.data()),
       bytes.size(),
   };
   auto instance = Contract{self, first_receiver, contract_stream};
   set_action_execution_type(instance);
   if constexpr (std::is_void_v<Result>) {
      std::apply([&](auto&&... value) { (instance.*method)(std::forward<decltype(value)>(value)...); }, arguments);
   } else {
      auto result = std::apply(
          [&](auto&&... value) { return (instance.*method)(std::forward<decltype(value)>(value)...); }, arguments);
      set_packed_result(result, "action return value is too large",
                        [](void* data, std::uint32_t size) { set_action_return_value(data, size); });
   }
}

} // namespace detail

template <typename Contract, typename Result, typename... Arguments>
void execute_action(chain::protocol::name self, chain::protocol::name first_receiver,
                    Result (Contract::*method)(Arguments...)) {
   detail::execute_action_impl<Contract, Result, Arguments...>(self, first_receiver, method);
}

namespace detail {

template <typename Contract, typename Result, typename... Arguments, typename Method>
void execute_call_impl(chain::protocol::name sender, chain::protocol::name receiver, Method method) {
   static_cast<void>(sender);
   const auto size = get_call_data(nullptr, 0U);
   check(size <= call_byte_limit<Contract>(), "sync call data exceeds contract decode byte limit");
   auto bytes = invocation_buffer{size};
   if (size != 0U) {
      check(get_call_data(bytes.data(), size) == size, "failed to read complete sync call data");
   }
   auto stream = invocation_input_stream<Contract>(bytes.data(), bytes.size());
   auto header = call_data_header{};
   forge::raw::unpack(stream, header);
   check(header.version == 0U, "unsupported sync call data version");

   auto arguments = std::tuple<std::decay_t<Arguments>...>{};
   forge::raw::unpack(stream, arguments);
   check(stream.remaining() == 0U, "sync call data contains trailing bytes");

   auto contract_stream = typename Contract::stream_type{
       reinterpret_cast<const char*>(bytes.data()),
       bytes.size(),
   };
   auto instance = Contract{receiver, receiver, contract_stream};
   set_call_execution_type(instance);
   if constexpr (std::is_void_v<Result>) {
      std::apply([&](auto&&... value) { (instance.*method)(std::forward<decltype(value)>(value)...); }, arguments);
   } else {
      auto result = std::apply(
          [&](auto&&... value) { return (instance.*method)(std::forward<decltype(value)>(value)...); }, arguments);
      set_packed_result(result, "sync call return value is too large",
                        [](void* data, std::uint32_t size) { set_call_return_value(data, size); });
   }
}

} // namespace detail

template <typename Contract, typename Result, typename... Arguments>
void execute_call(chain::protocol::name sender, chain::protocol::name receiver,
                  Result (Contract::*method)(Arguments...)) {
   detail::execute_call_impl<Contract, Result, Arguments...>(sender, receiver, method);
}

template <typename Contract, typename Result, typename... Arguments>
void execute_call(chain::protocol::name sender, chain::protocol::name receiver,
                  Result (Contract::*method)(Arguments...) const) {
   detail::execute_call_impl<Contract, Result, Arguments...>(sender, receiver, method);
}

template <typename Contract, typename Result, typename... Arguments>
void execute_action(chain::protocol::name self, chain::protocol::name first_receiver,
                    Result (Contract::*method)(Arguments...) const) {
   detail::execute_action_impl<Contract, Result, Arguments...>(self, first_receiver, method);
}

struct dispatch_entry {
   std::uint64_t name = 0;
   void (*invoke)(chain::protocol::name, chain::protocol::name) = nullptr;
};

struct notification_entry {
   std::uint64_t code = 0;
   std::uint64_t action = 0;
   void (*invoke)(chain::protocol::name, chain::protocol::name) = nullptr;
};

struct call_entry {
   std::uint64_t id = 0;
   void (*invoke)(chain::protocol::name, chain::protocol::name) = nullptr;
};

template <typename Contract, auto Method> constexpr dispatch_entry make_dispatch_entry(std::uint64_t name) {
   return {
       name,
       [](chain::protocol::name self, chain::protocol::name first_receiver) {
          execute_action<Contract>(self, first_receiver, Method);
       },
   };
}

template <typename Contract, auto Method>
constexpr notification_entry make_notification_entry(std::uint64_t code, std::uint64_t action) {
   return {
       code,
       action,
       [](chain::protocol::name self, chain::protocol::name first_receiver) {
          execute_action<Contract>(self, first_receiver, Method);
       },
   };
}

template <typename Contract, auto Method> constexpr call_entry make_call_entry(std::uint64_t id) {
   return {
       id,
       [](chain::protocol::name sender, chain::protocol::name receiver) {
          execute_call<Contract>(sender, receiver, Method);
       },
   };
}

namespace detail {

template <typename Deferred, typename Contract, auto Method>
constexpr dispatch_entry make_deferred_dispatch_entry(std::uint64_t name) {
   static_cast<void>(sizeof(Deferred*));
   return make_dispatch_entry<Contract, Method>(name);
}

} // namespace detail

void dispatch(chain::protocol::name receiver, chain::protocol::name code, std::uint64_t action,
              const dispatch_entry* entries, std::size_t size, const notification_entry* notifications = nullptr,
              std::size_t notification_size = 0U);

void dispatch_call(chain::protocol::name sender, chain::protocol::name receiver, const call_entry* entries,
                   std::size_t size);

template <std::size_t Size>
void dispatch(chain::protocol::name receiver, chain::protocol::name code, std::uint64_t action,
              const dispatch_entry (&entries)[Size]) {
   dispatch(receiver, code, action, entries, Size, nullptr, 0U);
}

template <std::size_t ActionSize, std::size_t NotificationSize>
void dispatch(chain::protocol::name receiver, chain::protocol::name code, std::uint64_t action,
              const dispatch_entry (&entries)[ActionSize],
              const notification_entry (&notifications)[NotificationSize]) {
   dispatch(receiver, code, action, entries, ActionSize, notifications, NotificationSize);
}

inline void dispatch(chain::protocol::name receiver, chain::protocol::name code, std::uint64_t action) {
   dispatch(receiver, code, action, nullptr, 0U, nullptr, 0U);
}

} // namespace forge::contract
