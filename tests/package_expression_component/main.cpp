#include <array>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <vector>

import forge.expression.program;
import forge.expression.exceptions;

namespace {

namespace expression = forge::expression;
namespace errors = expression::exceptions;

void require(bool condition, std::string_view message) {
   if (!condition) {
      std::cerr << "Installed expression assertion failed: " << message << '\n';
      std::exit(1);
   }
}

template <typename Function> void expect_error(errors::code code, Function&& action) {
   try {
      action();
   } catch (const forge::exceptions::base& error) {
      require(error.code() == errors::make_error_code(code), "wrong expression error category or code");
      return;
   }
   require(false, "expected a typed expression error");
}

void arithmetic(expression::runtime& runtime) {
   require(expression::program(runtime, "9007199254740993 + 2").evaluate().value.as_int64() == 9007199254740995LL,
           "integer arithmetic above 2^53 lost precision");
   require(
       expression::program(runtime, "math.mul_div(1000000000000000, 1000000000000000, 1000000000000000, rounding.ceil)")
               .evaluate()
               .value.as_int64() == 1000000000000000LL,
       "wide intermediate arithmetic failed");
   const auto unsigned_result =
       expression::program(runtime, "math.mul_div(18446744073709551615u, 2u, 2u, rounding.trunc)").evaluate().value;
   require(unsigned_result.is_uint64() && unsigned_result.as_uint64() == std::numeric_limits<std::uint64_t>::max(),
           "unsigned arithmetic changed the value or kind");

   const std::array sources = {"math.mul_div(-5, 1, 2, rounding.trunc)", "math.mul_div(-5, 1, 2, rounding.floor)",
                               "math.mul_div(-5, 1, 2, rounding.ceil)", "math.mul_div(-5, 1, 2, rounding.half_even)"};
   const std::array<std::int64_t, 4> expected = {-2, -3, -2, -2};
   for (std::size_t index = 0; index < sources.size(); ++index) {
      require(expression::program(runtime, sources[index]).evaluate().value.as_int64() == expected[index],
              "named rounding constant failed");
   }
   require(expression::program(runtime, "math.mul_div(7, 1, 2, rounding.half_even)").evaluate().value.as_int64() == 4,
           "ties-to-even rounding failed");
   require(expression::program(runtime, "false ? 1 / 0 : 9007199254740993").evaluate().value.as_int64() ==
               9007199254740993LL,
           "lazy branch evaluation failed");
}

void typed_errors(expression::runtime& runtime) {
   expect_error(errors::code::invalid_program, [&] { (void)expression::program(runtime, "true ? 1 : int(1.0)"); });
   expect_error(errors::code::evaluation_failed, [&] { (void)expression::program(runtime, "1 / 0").evaluate(); });
   expect_error(errors::code::evaluation_failed,
                [&] { (void)expression::program(runtime, "9223372036854775807 + 1").evaluate(); });
   const std::array variables = {expression::variable{"amount"}};
   expression::program program(runtime, "amount + 1", variables);
   expect_error(errors::code::invalid_input, [&] { (void)program.evaluate(); });
   expect_error(errors::code::invalid_input,
                [&] { (void)program.evaluate(forge::variant_object{"amount", std::uint64_t{1}}); });
   expression::limits limits;
   limits.source_bytes = 3;
   expect_error(errors::code::resource_exhausted, [&] { (void)expression::program(runtime, "1234", {}, limits); });
   limits.nodes = 0;
   expect_error(errors::code::invalid_profile, [&] { (void)expression::program(runtime, "1", {}, limits); });
}

void concurrent_evaluation(expression::runtime& runtime) {
   const std::array variables = {expression::variable{"amount"}};
   const expression::program program(runtime, "math.mul_div(amount, 1000000000000000, 1000000000000000, rounding.ceil)",
                                     variables);
   std::atomic<bool> passed = true;
   {
      std::vector<std::jthread> threads;
      for (int index = 0; index < 4; ++index) {
         threads.emplace_back([&] {
            try {
               for (int iteration = 0; iteration < 16; ++iteration) {
                  const forge::variant_object input{"amount", std::int64_t{9007199254740993LL}};
                  if (program.evaluate(input).value.as_int64() != 9007199254740993LL) {
                     passed = false;
                  }
               }
            } catch (...) {
               passed = false;
            }
         });
      }
   }
   require(passed, "concurrent evaluation failed");
}

} // namespace

int main() {
   try {
      expression::runtime runtime(expression::runtime::options{.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER},
                                                               .heap_bytes = 64 * 1024 * 1024});
      const expression::program retained(runtime, "1");
      require(retained.evaluate().value.as_int64() == 1, "persistent worker preparation failed");
      arithmetic(runtime);
      typed_errors(runtime);
      concurrent_evaluation(runtime);
      const auto memory = runtime.memory();
      require(memory.capacity == 64 * 1024 * 1024 && memory.used <= memory.peak && memory.peak <= memory.capacity,
              "worker memory snapshot exceeded its capacity");
      require(memory.used > 0 && memory.failures == 0, "ordinary installed calls exhausted worker memory");
      std::cout << "Installed expression: exact arithmetic, named rounding, typed errors, concurrency and worker "
                   "memory snapshot PASS\n";
      return 0;
   } catch (const std::exception& error) {
      std::cerr << "Installed expression consumer failed: " << error.what() << '\n';
      return 1;
   }
}
