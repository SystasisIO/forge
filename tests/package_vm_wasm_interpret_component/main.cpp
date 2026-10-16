#include <array>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <thread>

#ifndef THREAD_LOCAL
#error "The installed SoftFloat owner did not propagate THREAD_LOCAL to C++"
#endif

extern "C" {
#include <softfloat.h>
void forge_package_softfloat_c_addresses(const void** addresses);
int forge_package_softfloat_c_defaults(void);
void forge_package_softfloat_c_set(uint_fast8_t rounding, uint_fast8_t tininess, uint_fast8_t flags,
                                   uint_fast8_t precision);
uint32_t forge_package_softfloat_c_add(uint32_t left, uint32_t right);
}

import forge.vm.wasm.interpret.backend;

namespace {
bool installed_softfloat_tls_abi() {
   const auto original = std::array<uint_fast8_t, 4>{softfloat_roundingMode, softfloat_detectTininess,
                                                     softfloat_exceptionFlags, extF80_roundingPrecision};
   forge_package_softfloat_c_set(softfloat_round_min, softfloat_tininess_beforeRounding, softfloat_flag_infinite, 32);
   struct observation {
      std::array<const void*, 4> addresses{};
      bool passed = false;
   };
   auto results = std::array<observation, 2>{};
   auto phase = std::barrier{3};
   const auto worker = [&](unsigned index) {
      auto& result = results[index];
      result.passed = forge_package_softfloat_c_defaults();
      forge_package_softfloat_c_addresses(result.addresses.data());
      result.passed &=
          result.addresses == std::array<const void*, 4>{&softfloat_roundingMode, &softfloat_detectTininess,
                                                         &softfloat_exceptionFlags, &extF80_roundingPrecision};
      const auto mode = index == 0 ? softfloat_round_near_even : softfloat_round_max;
      const auto tininess = index == 0 ? softfloat_tininess_afterRounding : softfloat_tininess_beforeRounding;
      const auto precision = index == 0 ? 64 : 32;
      forge_package_softfloat_c_set(mode, tininess, softfloat_flag_invalid, precision);
      phase.arrive_and_wait();
      result.passed &= softfloat_roundingMode == mode && softfloat_detectTininess == tininess &&
                       softfloat_exceptionFlags == softfloat_flag_invalid && extF80_roundingPrecision == precision;
      phase.arrive_and_wait();
      const auto expected = index == 0 ? 0x3f800000U : 0x3f800001U;
      for (unsigned i = 0; i < 10000; ++i) {
         softfloat_exceptionFlags = 0;
         result.passed &= forge_package_softfloat_c_add(0x3f800000U, 0x33800000U) == expected &&
                          softfloat_exceptionFlags == softfloat_flag_inexact;
         softfloat_exceptionFlags = 0;
         result.passed &= f32_add(float32_t{0x3f800000U}, float32_t{0x33800000U}).v == expected &&
                          softfloat_exceptionFlags == softfloat_flag_inexact && softfloat_roundingMode == mode &&
                          softfloat_detectTininess == tininess && extF80_roundingPrecision == precision;
      }
      phase.arrive_and_wait();
   };
   auto first = std::thread{worker, 0};
   auto second = std::thread{worker, 1};
   phase.arrive_and_wait();
   auto passed = true;
   const auto parent_addresses = std::array<const void*, 4>{&softfloat_roundingMode, &softfloat_detectTininess,
                                                            &softfloat_exceptionFlags, &extF80_roundingPrecision};
   for (unsigned i = 0; i < 4; ++i)
      passed &= results[0].addresses[i] != results[1].addresses[i] && results[0].addresses[i] != parent_addresses[i] &&
                results[1].addresses[i] != parent_addresses[i];
   phase.arrive_and_wait();
   phase.arrive_and_wait();
   first.join();
   second.join();
   passed &= results[0].passed && results[1].passed && softfloat_roundingMode == softfloat_round_min &&
             softfloat_detectTininess == softfloat_tininess_beforeRounding &&
             softfloat_exceptionFlags == softfloat_flag_infinite && extF80_roundingPrecision == 32;
   forge_package_softfloat_c_set(original[0], original[1], original[2], original[3]);
   return passed;
}

struct host {
   std::int32_t ping(std::int32_t value) {
      return value;
   }

   void write_log(forge::vm::wasm::interpret::span<const char> text) {
      static_cast<void>(std::string_view{text.data(), text.size()});
   }
};

struct linear_memory {
   linear_memory(const linear_memory&) = delete;
   linear_memory& operator=(const linear_memory&) = delete;

   linear_memory() = default;

   forge::vm::wasm::interpret::wasm_allocator value;
};
} // namespace

int main() {
   if (!installed_softfloat_tls_abi())
      return 1;
   using host_functions = forge::vm::wasm::interpret::registered_host_functions<host>;
   using engine = forge::vm::wasm::interpret::backend<host_functions, forge::vm::wasm::interpret::interpreter,
                                                      forge::vm::wasm::interpret::compatibility_options>;
   using validator = forge::vm::wasm::interpret::backend<std::nullptr_t, forge::vm::wasm::interpret::null_backend,
                                                         forge::vm::wasm::interpret::compatibility_options>;

   host_functions::add<&host::ping>("env", "ping");
   host_functions::add<&host::write_log>("env", "write_log");

   forge::vm::wasm::interpret::wasm_code code{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
   auto parsed = validator{code, static_cast<forge::vm::wasm::interpret::wasm_allocator*>(nullptr)};

   host instance;
   linear_memory memory;
   engine backend{code, instance, &memory.value};
   auto deadline = forge::vm::wasm::interpret::watchdog{std::chrono::milliseconds{10}};

   backend.timed_run(deadline, [] {});

   static_assert(!forge::vm::wasm::interpret::interpreter::is_jit);
   (void)parsed.get_module();
   (void)backend.get_module();
   return 0;
}
