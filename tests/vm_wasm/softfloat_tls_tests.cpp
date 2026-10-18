#include "test_prelude.hpp"

#include <array>
#include <barrier>
#include <bit>
#include <cstdint>
#include <exception>
#include <thread>

extern "C" {
#include <softfloat.h>
}

import forge.vm.wasm.interpret.backend;

namespace vm = forge::vm::wasm::interpret;

namespace {
struct saved_state {
   uint_fast8_t rounding = softfloat_roundingMode;
   uint_fast8_t tininess = softfloat_detectTininess;
   uint_fast8_t flags = softfloat_exceptionFlags;
   uint_fast8_t precision = extF80_roundingPrecision;

   ~saved_state() {
      softfloat_roundingMode = rounding;
      softfloat_detectTininess = tininess;
      softfloat_exceptionFlags = flags;
      extF80_roundingPrecision = precision;
   }
};

struct observation {
   std::array<std::uintptr_t, 4> addresses{};
   bool defaults = false;
   bool state_local = false;
   bool halfway = false;
   bool flags_local = false;
   bool parallel = false;
   std::exception_ptr failure;
};

vm::wasm_code add_fixture() {
   return {0, 97, 115, 109, 1,   0,   0,   0, 1, 7,  1, 0x60, 2, 0x7d, 0x7d, 1, 0x7d, 3, 2,    1,   0,
           7, 7,  1,   3,   'a', 'd', 'd', 0, 0, 10, 9, 1,    7, 0,    0x20, 0, 0x20, 1, 0x92, 0x0b};
}
} // namespace

BOOST_AUTO_TEST_SUITE(softfloat_tls)

BOOST_AUTO_TEST_CASE(barrier_controlled_state_addresses_defaults_rounding_and_flags) {
   const auto restore = saved_state{};
   softfloat_roundingMode = softfloat_round_min;
   softfloat_detectTininess = softfloat_tininess_beforeRounding;
   softfloat_exceptionFlags = softfloat_flag_infinite;
   extF80_roundingPrecision = 32;
   auto states = std::array<observation, 2>{};
   auto phase = std::barrier{3};
   const auto worker = [&](unsigned index) {
      auto& result = states[index];
      try {
         result.addresses = {reinterpret_cast<std::uintptr_t>(&softfloat_roundingMode),
                             reinterpret_cast<std::uintptr_t>(&softfloat_detectTininess),
                             reinterpret_cast<std::uintptr_t>(&softfloat_exceptionFlags),
                             reinterpret_cast<std::uintptr_t>(&extF80_roundingPrecision)};
         result.defaults = softfloat_roundingMode == softfloat_round_near_even &&
                           softfloat_detectTininess == softfloat_tininess_afterRounding &&
                           softfloat_exceptionFlags == 0 && extF80_roundingPrecision == 80;
         phase.arrive_and_wait();
         // Refuse shared storage before the test itself could race on it.
         for (unsigned i = 0; i < 4; ++i) {
            if (states[0].addresses[i] == states[1].addresses[i]) {
               phase.arrive_and_drop();
               return;
            }
         }
         const auto mode = index == 0 ? softfloat_round_near_even : softfloat_round_max;
         const auto tiny = index == 0 ? softfloat_tininess_afterRounding : softfloat_tininess_beforeRounding;
         const auto precision = index == 0 ? 64 : 32;
         softfloat_roundingMode = mode;
         softfloat_detectTininess = tiny;
         extF80_roundingPrecision = precision;
         softfloat_exceptionFlags = 0;
         phase.arrive_and_wait();
         result.state_local = softfloat_roundingMode == mode && softfloat_detectTininess == tiny &&
                              extF80_roundingPrecision == precision;
         const auto expected = index == 0 ? 0x3f800000U : 0x3f800001U;
         result.halfway = f32_add(float32_t{0x3f800000}, float32_t{0x33800000}).v == expected &&
                          softfloat_exceptionFlags == softfloat_flag_inexact;
         phase.arrive_and_wait();
         softfloat_exceptionFlags = 0;
         if (index == 0) {
            static_cast<void>(f32_add(float32_t{0x7f800001}, float32_t{0x3f800000}));
         } else {
            static_cast<void>(f32_add(float32_t{0x3f800000}, float32_t{0}));
         }
         phase.arrive_and_wait();
         result.flags_local = softfloat_exceptionFlags == (index == 0 ? softfloat_flag_invalid : 0);
         auto code = add_fixture();
         auto instance = vm::backend<>{code, static_cast<vm::wasm_allocator*>(nullptr)};
         result.parallel = true;
         phase.arrive_and_wait();
         for (unsigned i = 0; i < 20000; ++i) {
            softfloat_exceptionFlags = 0;
            auto output = instance.call_with_return("env", "add", std::bit_cast<float>(0x3f800000U),
                                                    std::bit_cast<float>(0x33800000U));
            // Guest invocation is canonical, then restores this thread's host state.
            result.parallel &= output && output->to_fui32() == 0x3f800000U && softfloat_roundingMode == mode &&
                               softfloat_detectTininess == tiny && extF80_roundingPrecision == precision &&
                               softfloat_exceptionFlags == 0;
         }
         phase.arrive_and_wait();
      } catch (...) {
         result.failure = std::current_exception();
         phase.arrive_and_drop();
      }
   };
   auto first = std::jthread{worker, 0};
   auto second = std::jthread{};
   try {
      second = std::jthread{worker, 1};
   } catch (...) {
      phase.arrive_and_drop(); // Missing second worker.
      phase.arrive_and_drop(); // Main no longer participates.
      throw;
   }
   for (unsigned i = 0; i < 6; ++i)
      phase.arrive_and_wait();
   first.join();
   second.join();
   for (unsigned i = 0; i < 4; ++i) {
      BOOST_TEST(states[0].addresses[i] != states[1].addresses[i]);
   }
   for (const auto& result : states) {
      if (result.failure)
         BOOST_CHECK_NO_THROW(std::rethrow_exception(result.failure));
      BOOST_TEST(result.defaults);
      BOOST_TEST(result.state_local);
      BOOST_TEST(result.halfway);
      BOOST_TEST(result.flags_local);
      BOOST_TEST(result.parallel);
   }
   BOOST_TEST(softfloat_roundingMode == softfloat_round_min);
   BOOST_TEST(softfloat_detectTininess == softfloat_tininess_beforeRounding);
   BOOST_TEST(softfloat_exceptionFlags == softfloat_flag_infinite);
   BOOST_TEST(extF80_roundingPrecision == 32U);
}

BOOST_AUTO_TEST_SUITE_END()
