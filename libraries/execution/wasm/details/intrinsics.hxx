#pragma once

namespace forge::execution::wasm {

class intrinsics {
 public:
   intrinsics(const invocation&, const invocation_limits&);
   std::uint32_t action_data_size() const;
   std::uint32_t read_action_data(std::span<char>) const;
   void set_action_return_value(std::span<const char>);
   void eosio_assert_message(std::uint32_t, std::span<const char>) const;
   void prints_l(std::span<const char>);
   invocation_result result() &&;
   static void register_functions();

 private:
   const invocation& _call;
   const invocation_limits& _limits;
   invocation_result _result;
};

} // namespace forge::execution::wasm
