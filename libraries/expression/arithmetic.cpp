#include "details/arithmetic.hxx"

#include <cstdint>
#include <limits>
#include <type_traits>

#include "checker/type_checker_builder.h"
#include "common/decl.h"
#include "common/type.h"
#include "common/value.h"
#include "runtime/function_adapter.h"
#include "runtime/function_registry.h"

namespace forge::expression::detail {
namespace {

enum class rounding_mode : std::int64_t { trunc = 0, floor = 1, ceil = 2, half_even = 3 };

static_assert(sizeof(__int128) == 16);
static_assert(sizeof(unsigned __int128) == 16);
static_assert(std::is_signed_v<__int128>);
static_assert(!std::is_signed_v<unsigned __int128>);

bool valid_mode(std::int64_t mode) {
   return mode >= static_cast<std::int64_t>(rounding_mode::trunc) &&
          mode <= static_cast<std::int64_t>(rounding_mode::half_even);
}

template <typename Wide> Wide rounded_quotient(Wide product, Wide divisor, rounding_mode mode) {
   auto quotient = product / divisor;
   const auto remainder = product % divisor;
   if (remainder == 0) {
      return quotient;
   }

   auto negative = false;
   if constexpr (std::is_signed_v<Wide>) {
      negative = product < 0;
   }
   const auto magnitude = negative ? -remainder : remainder;
   switch (mode) {
   case rounding_mode::trunc:
      break;
   case rounding_mode::floor:
      if (negative) {
         --quotient;
      }
      break;
   case rounding_mode::ceil:
      if (!negative) {
         ++quotient;
      }
      break;
   case rounding_mode::half_even:
      // The remainder is smaller than the 64-bit divisor, so doubling it fits Wide.
      if (magnitude * 2 > divisor || (magnitude * 2 == divisor && quotient % 2 != 0)) {
         if (negative) {
            --quotient;
         } else {
            ++quotient;
         }
      }
      break;
   }
   return quotient;
}

cel::Value multiply_divide(std::int64_t left, std::int64_t right, std::int64_t divisor, std::int64_t mode,
                           const cel::Function::InvokeContext& context) {
   if (!valid_mode(mode)) {
      return cel::ErrorValue::From(absl::InvalidArgumentError("invalid math.mul_div rounding mode"), context.arena());
   }
   if (divisor == 0) {
      return cel::ErrorValue::From(absl::InvalidArgumentError("divide by zero"), context.arena());
   }

   // Every int64 x int64 product fits signed 128 bits, including INT64_MIN squared.
   auto product = static_cast<__int128>(left) * static_cast<__int128>(right);
   auto denominator = static_cast<__int128>(divisor);
   if (denominator < 0) {
      product = -product;
      denominator = -denominator;
   }
   const auto result = rounded_quotient(product, denominator, static_cast<rounding_mode>(mode));
   if (result < static_cast<__int128>(std::numeric_limits<std::int64_t>::min()) ||
       result > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
      return cel::ErrorValue::From(absl::OutOfRangeError("integer overflow"), context.arena());
   }
   return cel::IntValue(static_cast<std::int64_t>(result));
}

cel::Value multiply_divide(std::uint64_t left, std::uint64_t right, std::uint64_t divisor, std::int64_t mode,
                           const cel::Function::InvokeContext& context) {
   if (!valid_mode(mode)) {
      return cel::ErrorValue::From(absl::InvalidArgumentError("invalid math.mul_div rounding mode"), context.arena());
   }
   if (divisor == 0) {
      return cel::ErrorValue::From(absl::InvalidArgumentError("divide by zero"), context.arena());
   }

   const auto product = static_cast<unsigned __int128>(left) * static_cast<unsigned __int128>(right);
   const auto result =
       rounded_quotient(product, static_cast<unsigned __int128>(divisor), static_cast<rounding_mode>(mode));
   if (result > static_cast<unsigned __int128>(std::numeric_limits<std::uint64_t>::max())) {
      return cel::ErrorValue::From(absl::OutOfRangeError("unsigned integer overflow"), context.arena());
   }
   return cel::UintValue(static_cast<std::uint64_t>(result));
}

} // namespace

absl::Status add_arithmetic_declarations(cel::TypeCheckerBuilder& builder) {
   auto declaration = cel::MakeFunctionDecl(
       "math.mul_div",
       cel::MakeOverloadDecl(cel::IntType(), cel::IntType(), cel::IntType(), cel::IntType(), cel::IntType()),
       cel::MakeOverloadDecl(cel::UintType(), cel::UintType(), cel::UintType(), cel::UintType(), cel::IntType()));
   if (!declaration.ok()) {
      return declaration.status();
   }
   return builder.AddFunction(*declaration);
}

absl::Status register_arithmetic_functions(cel::FunctionRegistry& registry) {
   auto status = cel::QuaternaryFunctionAdapter<cel::Value, std::int64_t, std::int64_t, std::int64_t, std::int64_t>::
       RegisterGlobalOverload(
           "math.mul_div",
           [](std::int64_t left, std::int64_t right, std::int64_t divisor, std::int64_t mode,
              const cel::Function::InvokeContext& context) {
              return multiply_divide(left, right, divisor, mode, context);
           },
           registry);
   if (!status.ok()) {
      return status;
   }
   return cel::QuaternaryFunctionAdapter<cel::Value, std::uint64_t, std::uint64_t, std::uint64_t, std::int64_t>::
       RegisterGlobalOverload(
           "math.mul_div",
           [](std::uint64_t left, std::uint64_t right, std::uint64_t divisor, std::int64_t mode,
              const cel::Function::InvokeContext& context) {
              return multiply_divide(left, right, divisor, mode, context);
           },
           registry);
}

} // namespace forge::expression::detail
