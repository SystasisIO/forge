#include "test_prelude.hpp"

#include <cfenv>
#include <cstdint>
#include <type_traits>

extern "C" {
#include <softfloat.h>
}

import forge.vm.wasm.interpret.floating_point_environment;

namespace vm = forge::vm::wasm::interpret;

static_assert(std::is_nothrow_constructible_v<vm::floating_point_environment>);
static_assert(std::is_nothrow_destructible_v<vm::floating_point_environment>);
static_assert(!std::is_copy_constructible_v<vm::floating_point_environment>);
static_assert(!std::is_copy_assignable_v<vm::floating_point_environment>);
static_assert(!std::is_move_constructible_v<vm::floating_point_environment>);
static_assert(!std::is_move_assignable_v<vm::floating_point_environment>);

namespace {
struct state {
   std::uint_fast8_t rounding = softfloat_roundingMode;
   std::uint_fast8_t tininess = softfloat_detectTininess;
   std::uint_fast8_t flags = softfloat_exceptionFlags;
   std::uint_fast8_t precision = extF80_roundingPrecision;

   void apply() const {
      softfloat_roundingMode = rounding;
      softfloat_detectTininess = tininess;
      softfloat_exceptionFlags = flags;
      extF80_roundingPrecision = precision;
   }
   bool operator==(const state&) const = default;
};

constexpr auto canonical = state{softfloat_round_near_even, softfloat_tininess_afterRounding, 0, 80};
constexpr auto external = state{softfloat_round_min, softfloat_tininess_beforeRounding, softfloat_flag_infinite, 32};
constexpr auto changed = state{softfloat_round_max, softfloat_tininess_beforeRounding, softfloat_flag_invalid, 64};

struct restore_state {
   state saved;
   ~restore_state() {
      saved.apply();
   }
};

struct fixture_failure {};
} // namespace

BOOST_AUTO_TEST_SUITE(floating_point_environment)

BOOST_AUTO_TEST_CASE(nested_scope_restores_all_four_values_on_normal_and_exceptional_exit) {
   const auto restore = restore_state{};
   external.apply();
   {
      const auto outer = vm::floating_point_environment{};
      BOOST_TEST((state{} == canonical));
      changed.apply();
      auto caught = false;
      try {
         const auto inner = vm::floating_point_environment{};
         BOOST_TEST((state{} == canonical));
         BOOST_TEST(f32_add(float32_t{0x3f800000}, float32_t{0x33800000}).v == 0x3f800000U);
         BOOST_TEST(softfloat_exceptionFlags == softfloat_flag_inexact);
         throw fixture_failure{};
      } catch (const fixture_failure&) {
         caught = true;
      }
      BOOST_TEST(caught);
      BOOST_TEST((state{} == changed));
   }
   BOOST_TEST((state{} == external));
}

BOOST_AUTO_TEST_CASE(softfloat_guard_does_not_change_caller_native_rounding_or_flags) {
   const auto restore = restore_state{};
   auto original = std::fenv_t{};
   BOOST_REQUIRE_EQUAL(std::fegetenv(&original), 0);
   struct restore_native {
      std::fenv_t original;
      ~restore_native() {
         std::fesetenv(&original);
      }
   } native{original};
   BOOST_REQUIRE_EQUAL(std::fesetenv(FE_DFL_ENV), 0);
   BOOST_REQUIRE_EQUAL(std::fesetround(FE_DOWNWARD), 0);
   BOOST_REQUIRE_EQUAL(std::feraiseexcept(FE_INVALID), 0);
   const auto flags = std::fetestexcept(FE_ALL_EXCEPT);
   {
      const auto scope = vm::floating_point_environment{};
      BOOST_TEST(std::fegetround() == FE_DOWNWARD);
      BOOST_TEST(std::fetestexcept(FE_ALL_EXCEPT) == flags);
   }
   BOOST_TEST(std::fegetround() == FE_DOWNWARD);
   BOOST_TEST(std::fetestexcept(FE_ALL_EXCEPT) == flags);
}

BOOST_AUTO_TEST_SUITE_END()
