#include <boost/test/unit_test.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <future>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.expression.program;
import forge.expression.exceptions;

namespace {

namespace expression = forge::expression;
namespace errors = expression::exceptions;
using expression::type;
using expression::variable;
using forge::mutable_variant_object;
using forge::variant;
using forge::variant_object;

template <typename T> T integer(std::string_view text) {
   T value{};
   const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
   BOOST_REQUIRE(result.ec == std::errc{} && result.ptr == text.data() + text.size());
   return value;
}

template <errors::code Code> bool error_is(const forge::exceptions::base& error) {
   return error.code() == errors::make_error_code(Code);
}

struct oracle_case {
   bool unsigned_values;
   const char* left;
   const char* right;
   const char* divisor;
   std::int64_t mode;
   int status;
   const char* result;
};

const oracle_case oracle_cases[] = {
#include "expression_arithmetic_cases.inc"
};

struct donor_case {
   std::string_view id;
   std::string_view source;
   std::vector<variable> variables;
   variant_object input;
   const char* expected;
};

} // namespace

BOOST_AUTO_TEST_CASE(expression_donor_formulas) {
   const donor_case donor_cases[] = {
#include "expression_donors.inc"
   };
   for (const auto& item : donor_cases) {
      BOOST_TEST_CONTEXT(item.id) {
         std::int64_t result = 0;
         BOOST_REQUIRE_NO_THROW(
             result = expression::program(item.source, item.variables).evaluate(item.input).value.as_int64());
         BOOST_CHECK_EQUAL(result, integer<std::int64_t>(item.expected));
      }
   }
}

BOOST_AUTO_TEST_CASE(expression_exact_arithmetic_oracle) {
   const std::array signed_variables = {variable{"left"}, variable{"right"}, variable{"divisor"}, variable{"mode"}};
   const std::array unsigned_variables = {variable{"left", type::uint64}, variable{"right", type::uint64},
                                          variable{"divisor", type::uint64}, variable{"mode"}};
   expression::program signed_program("math.mul_div(left, right, divisor, mode)", signed_variables);
   expression::program unsigned_program("math.mul_div(left, right, divisor, mode)", unsigned_variables);
   for (const auto& item : oracle_cases) {
      BOOST_TEST_CONTEXT(item.left << " * " << item.right << " / " << item.divisor << " mode=" << item.mode) {
         const auto number = [&](std::string_view text) {
            return item.unsigned_values ? variant{integer<std::uint64_t>(text)} : variant{integer<std::int64_t>(text)};
         };
         variant_object input = mutable_variant_object()("left", number(item.left))("right", number(item.right))(
             "divisor", number(item.divisor))("mode", item.mode);
         const auto& program = item.unsigned_values ? unsigned_program : signed_program;
         if (item.status != 0) {
            BOOST_CHECK_EXCEPTION((void)program.evaluate(input), forge::exceptions::base,
                                  error_is<errors::code::evaluation_failed>);
         } else {
            const auto result = program.evaluate(input);
            if (item.unsigned_values) {
               BOOST_REQUIRE(result.value.is_uint64());
               BOOST_CHECK_EQUAL(result.value.as_uint64(), integer<std::uint64_t>(item.result));
            } else {
               BOOST_REQUIRE(result.value.is_int64());
               BOOST_CHECK_EQUAL(result.value.as_int64(), integer<std::int64_t>(item.result));
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(expression_conditions_lookup_and_reference_inputs) {
   const std::array variables = {variable{"duration"}, variable{"resolution", type::string},
                                 variable{"audio", type::boolean}, variable{"rates", type::map_int64}};
   expression::program price("duration * rates[resolution] + (audio ? 7 : 0)", variables);
   variant_object input = mutable_variant_object()("duration", 3)("resolution", "720p")("audio", true)(
       "rates", mutable_variant_object()("720p", 13)("1080p", 29));
   BOOST_CHECK_EQUAL(price.evaluate(input).value.as_int64(), 46);
   const std::array list_variables = {variable{"durations", type::list_int64}};
   expression::program references("durations.size() > 0 ? math.mul_div(durations[0], 1, 1000, rounding.ceil) : 0",
                                  list_variables);
   BOOST_CHECK_EQUAL(references.evaluate(variant_object{"durations", forge::variants{}}).value.as_int64(), 0);
   BOOST_CHECK_EQUAL(references.evaluate(variant_object{"durations", forge::variants{variant{2001}}}).value.as_int64(),
                     3);
   BOOST_CHECK_EQUAL(expression::program("false ? 1 / 0 : 9007199254740993").evaluate().value.as_int64(),
                     9007199254740993LL);
   BOOST_CHECK_EQUAL(expression::program("math.mul_div(-5, 1, 2, rounding.half_even)").evaluate().value.as_int64(), -2);
}

BOOST_AUTO_TEST_CASE(expression_forbidden_types_and_operations_in_all_branches) {
   for (std::string_view source :
        {"1.0", "true ? 1 : int(1.0)", "true ? 1 : int('1')", "true ? 1 : size(b'abc')",
         "true ? 1 : timestamp('2026-01-01T00:00:00Z').getFullYear()", "null", "int", "[1, 2]", "[1, 2] == [1, 2]",
         "'a' + 'b' == 'ab'", "'abc'.matches('.*')", "[1, 2].map(x, x + 1)", "[1, 2].all(x, x > 0)", "[[1]][0][0]",
         "true ? 1 : math.mul_div(1u, 1, 1, 0)"}) {
      BOOST_TEST_CONTEXT(source) {
         BOOST_CHECK_EXCEPTION((void)expression::program(source), forge::exceptions::base,
                               error_is<errors::code::invalid_program>);
      }
   }
}

BOOST_AUTO_TEST_CASE(expression_strict_inputs_and_runtime_errors) {
   const std::array variables = {variable{"x"}};
   expression::program program("x + 1", variables);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(), forge::exceptions::base, error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", true}), forge::exceptions::base,
                         error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", std::uint64_t{1}}), forge::exceptions::base,
                         error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(mutable_variant_object()("x", 1)("unexpected", 2)),
                         forge::exceptions::base, error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", std::numeric_limits<std::int64_t>::max()}),
                         forge::exceptions::base, error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program("1 / 0").evaluate(), forge::exceptions::base,
                         error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program("{720: 3}[1080]").evaluate(), forge::exceptions::base,
                         error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program("[1][2]").evaluate(), forge::exceptions::base,
                         error_is<errors::code::evaluation_failed>);
   const std::array duplicate = {variable{"x"}, variable{"x"}};
   BOOST_CHECK_EXCEPTION((void)expression::program("x", duplicate), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   const std::array invalid = {variable{"rounding.ceil"}};
   BOOST_CHECK_EXCEPTION((void)expression::program("1", invalid), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
}

BOOST_AUTO_TEST_CASE(expression_source_node_depth_and_profile_budgets) {
   expression::limits budget;
   budget.source_bytes = 3;
   BOOST_CHECK_EQUAL(expression::program("123", {}, budget).evaluate().value.as_int64(), 123);
   BOOST_CHECK_EXCEPTION((void)expression::program("1234", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.nodes = 3;
   BOOST_CHECK_EQUAL(expression::program("1 + 2", {}, budget).evaluate().value.as_int64(), 3);
   budget.nodes = 2;
   BOOST_CHECK_EXCEPTION((void)expression::program("1 + 2", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   // Parsed qualified calls contain the namespace receiver, which CEL's
   // checker removes. Both parsed and checked trees must fit the allowance.
   budget.nodes = 6;
   BOOST_CHECK_EQUAL(expression::program("math.mul_div(1, 2, 1, 0)", {}, budget).evaluate().value.as_int64(), 2);
   budget.nodes = 5;
   BOOST_CHECK_EXCEPTION((void)expression::program("math.mul_div(1, 2, 1, 0)", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.depth = 2;
   BOOST_CHECK_EXCEPTION((void)expression::program("1 + (2 + (3 + 4))", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.nodes = 0;
   BOOST_CHECK_EXCEPTION((void)expression::program("1", {}, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   budget = {};
   budget.variables = 0;
   const std::array variables = {variable{"x"}};
   BOOST_CHECK_EXCEPTION((void)expression::program("x", variables, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   budget = {};
   ++budget.source_bytes;
   BOOST_CHECK_EXCEPTION((void)expression::program("1", {}, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
}

BOOST_AUTO_TEST_CASE(expression_input_and_literal_budgets) {
   expression::limits budget;
   budget.collection_elements = 2;
   const std::array variables = {variable{"items", type::list_int64}};
   expression::program program("items.size()", variables, budget);
   BOOST_CHECK_EQUAL(
       program.evaluate(variant_object{"items", forge::variants{variant{1}, variant{2}}}).value.as_int64(), 2);
   BOOST_CHECK_EXCEPTION(
       (void)program.evaluate(variant_object{"items", forge::variants{variant{1}, variant{2}, variant{3}}}),
       forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)expression::program("[1, 2, 3].size()", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.string_bytes = 3;
   const std::array strings = {variable{"x", type::string}};
   expression::program string_program("x == 'abc'", strings, budget);
   BOOST_CHECK(string_program.evaluate(variant_object{"x", "abc"}).value.as_bool());
   BOOST_CHECK_EXCEPTION((void)string_program.evaluate(variant_object{"x", "abcd"}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)expression::program("'abcd' == 'abc'", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   const std::array maps = {variable{"x", type::map_int64}};
   BOOST_CHECK_EQUAL(expression::program("x.abc", maps, budget)
                         .evaluate(variant_object{"x", variant_object{"abc", 1}})
                         .value.as_int64(),
                     1);
   BOOST_CHECK_EXCEPTION((void)expression::program("x.abcd", maps, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.total_input_bytes = 8;
   const std::array integers = {variable{"x"}};
   expression::program integer_program("x", integers, budget);
   // Input bytes include variable names as well as their typed scalar payload.
   BOOST_CHECK_EXCEPTION((void)integer_program.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
}

BOOST_AUTO_TEST_CASE(expression_ast_budget_boundaries_reset_and_concurrency) {
   const std::array variables = {variable{"x"}};
   expression::program baseline("x + 1", variables);
   const auto count = baseline.evaluate(variant_object{"x", 1}).ast_events;
   BOOST_REQUIRE(count > 1);
   expression::limits budget;
   budget.ast_events = count;
   expression::program exact("x + 1", variables, budget);
   BOOST_CHECK_EQUAL(exact.evaluate(variant_object{"x", 1}).ast_events, count);
   BOOST_CHECK_EQUAL(exact.evaluate(variant_object{"x", 2}).ast_events, count);
   budget.ast_events = count - 1;
   expression::program too_small("x + 1", variables, budget);
   BOOST_CHECK_EXCEPTION((void)too_small.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)too_small.evaluate(variant_object{"x", 2}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   expression::program pruned("(x / 0 == 0) || true", variables);
   BOOST_CHECK(pruned.evaluate(variant_object{"x", 1}).value.as_bool());
   budget.ast_events = 1;
   expression::program interrupted("(x / 0 == 0) || true", variables, budget);
   BOOST_CHECK_EXCEPTION((void)interrupted.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   // Concurrent invocations must not share input state, arenas or counters.
   std::vector<std::future<std::int64_t>> results;
   for (std::int64_t i = 0; i < 8; ++i) {
      results.push_back(std::async(std::launch::async,
                                   [&exact, i] { return exact.evaluate(variant_object{"x", i}).value.as_int64(); }));
   }
   for (std::size_t i = 0; i < results.size(); ++i) {
      BOOST_CHECK_EQUAL(results[i].get(), i + 1);
   }
}

BOOST_AUTO_TEST_CASE(expression_lifetimes_move_and_redacted_errors) {
   expression::program program = [] {
      const std::string source = "x + 1";
      const std::vector variables = {variable{"x"}};
      return expression::program(source, variables);
   }();
   auto moved = std::move(program);
   BOOST_CHECK_EQUAL(moved.evaluate(variant_object{"x", 3}).value.as_int64(), 4);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(), forge::exceptions::base, error_is<errors::code::invalid_profile>);
   const std::string secret = "customer-secret-marker";
   try {
      expression::program invalid(secret);
      BOOST_FAIL("invalid source was accepted");
   } catch (const forge::exceptions::base& error) {
      BOOST_CHECK(error_is<errors::code::invalid_program>(error));
      BOOST_CHECK(std::string(error.what()).find(secret) == std::string::npos);
   }
}
