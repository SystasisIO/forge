module;

#include <cstdint>

export module forge.vm.wasm.interpret.floating_point_environment;

export namespace forge::vm::wasm::interpret {

// SoftFloat scope only; native guest FP state belongs to the shared WAMR boundary.
class floating_point_environment {
 public:
   floating_point_environment() noexcept;
   ~floating_point_environment() noexcept;

   floating_point_environment(const floating_point_environment&) = delete;
   floating_point_environment& operator=(const floating_point_environment&) = delete;
   floating_point_environment(floating_point_environment&&) = delete;
   floating_point_environment& operator=(floating_point_environment&&) = delete;

 private:
   std::uint_fast8_t _rounding;
   std::uint_fast8_t _tininess;
   std::uint_fast8_t _flags;
   std::uint_fast8_t _precision;
};

} // namespace forge::vm::wasm::interpret
