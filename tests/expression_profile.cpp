#include <boost/test/unit_test.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <stop_token>
#include <type_traits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

import forge.expression.program;
import forge.expression.exceptions;
import forge.expression.artifact_codec;
import forge.asio.blocking;
import forge.asio.runtime;

namespace {

namespace expression = forge::expression;
namespace errors = expression::exceptions;
using expression::type;
using expression::variable;
using forge::mutable_variant_object;
using forge::variant;
using forge::variant_object;

static_assert(!std::is_constructible_v<expression::program, std::string_view>);

expression::runtime& shared_runtime() {
   static expression::runtime runtime(expression::runtime::options{
       .helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}, .heap_bytes = 64 * 1024 * 1024});
   return runtime;
}

boost::asio::awaitable<void> async_roundtrip(expression::runtime& runtime) {
   auto program = co_await expression::program::prepare_async(runtime, "x + 1", {variable{"x"}});
   const auto result = co_await program.evaluate_async(variant_object{"x", std::int64_t{9007199254740993LL}});
   BOOST_CHECK_EQUAL(result.value.as_int64(), 9007199254740994LL);
   const auto memory = co_await runtime.memory_async();
   BOOST_CHECK_LE(memory.used, memory.peak);
   BOOST_CHECK_LE(memory.peak, memory.capacity);
}

boost::asio::awaitable<void> injected_roundtrip(std::unique_ptr<expression::runtime> runtime) {
   for (int iteration = 0; iteration < 80; ++iteration) {
      auto program = co_await expression::program::prepare_async(*runtime, "x + 1", {variable{"x"}});
      BOOST_CHECK_EQUAL((co_await program.evaluate_async(variant_object{"x", iteration})).value.as_int64(),
                        iteration + 1);
      // Destruction here runs on the single injected IO worker, including more
      // than the native 64 slots. Automatic retirement must never block it.
   }
   const auto memory = co_await runtime->memory_async();
   BOOST_CHECK_EQUAL(memory.capacity, 64 * 1024 * 1024);
   BOOST_CHECK_LE(memory.used, memory.peak);
   BOOST_CHECK_LE(memory.peak, memory.capacity);
   co_await runtime->shutdown_async();
   runtime.reset(); // Already-drained wrapper destruction on the injected IO thread.
}

boost::asio::awaitable<void> pending_preparation(expression::runtime& runtime) {
   auto program = co_await expression::program::prepare_async(runtime, "1");
}

// The script only reports its identity, then replaces itself with the genuine
// native worker. It does not implement or alter the worker protocol.
class identified_worker {
 public:
   identified_worker() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-expression-cancel-XXXXXX").string();
      const auto* created = ::mkdtemp(pattern.data());
      BOOST_REQUIRE(created != nullptr);
      _directory = created;
      helper = _directory / "worker";
      _identity = _directory / "pid";
      std::ofstream output{helper};
      output << "#!/bin/sh\nprintf '%s\\n' \"$$\" > " << quoted(_identity.string()) << "\nexec "
             << quoted(FORGE_EXPRESSION_WORKER) << '\n';
      output.close();
      BOOST_REQUIRE(output.good());
      BOOST_REQUIRE_EQUAL(::chmod(helper.c_str(), 0700), 0);
   }

   ~identified_worker() {
      std::error_code ignored;
      std::filesystem::remove_all(_directory, ignored);
   }

   pid_t pid() const {
      std::ifstream input{_identity};
      pid_t value = -1;
      input >> value;
      BOOST_REQUIRE(value > 1);
      return value;
   }

   std::filesystem::path helper;

 private:
   static std::string quoted(std::string_view value) {
      std::string result{"'"};
      for (const auto character : value) {
         if (character == '\'')
            result += "'\\''";
         else
            result += character;
      }
      return result + "'";
   }
   std::filesystem::path _directory;
   std::filesystem::path _identity;
};

boost::asio::awaitable<void> cancel_after_started(boost::asio::cancellation_signal& cancellation) {
   boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, std::chrono::milliseconds{100});
   co_await timer.async_wait(boost::asio::use_awaitable);
   cancellation.emit(boost::asio::cancellation_type::terminal);
}

boost::asio::awaitable<void> stop_after_started(std::stop_source& cancellation) {
   boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor, std::chrono::milliseconds{100});
   co_await timer.async_wait(boost::asio::use_awaitable);
   cancellation.request_stop();
}

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
         BOOST_REQUIRE_NO_THROW(result = expression::program(shared_runtime(), item.source, item.variables)
                                             .evaluate(item.input)
                                             .value.as_int64());
         BOOST_CHECK_EQUAL(result, integer<std::int64_t>(item.expected));
      }
   }
}

BOOST_AUTO_TEST_CASE(expression_exact_arithmetic_oracle) {
   const std::array signed_variables = {variable{"left"}, variable{"right"}, variable{"divisor"}, variable{"mode"}};
   const std::array unsigned_variables = {variable{"left", type::uint64}, variable{"right", type::uint64},
                                          variable{"divisor", type::uint64}, variable{"mode"}};
   expression::program signed_program(shared_runtime(), "math.mul_div(left, right, divisor, mode)", signed_variables);
   expression::program unsigned_program(shared_runtime(), "math.mul_div(left, right, divisor, mode)",
                                        unsigned_variables);
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
   expression::program price(shared_runtime(), "duration * rates[resolution] + (audio ? 7 : 0)", variables);
   variant_object input = mutable_variant_object()("duration", 3)("resolution", "720p")("audio", true)(
       "rates", mutable_variant_object()("720p", 13)("1080p", 29));
   BOOST_CHECK_EQUAL(price.evaluate(input).value.as_int64(), 46);
   const std::array list_variables = {variable{"durations", type::list_int64}};
   expression::program references(shared_runtime(),
                                  "durations.size() > 0 ? math.mul_div(durations[0], 1, 1000, rounding.ceil) : 0",
                                  list_variables);
   BOOST_CHECK_EQUAL(references.evaluate(variant_object{"durations", forge::variants{}}).value.as_int64(), 0);
   BOOST_CHECK_EQUAL(references.evaluate(variant_object{"durations", forge::variants{variant{2001}}}).value.as_int64(),
                     3);
   BOOST_CHECK_EQUAL(
       expression::program(shared_runtime(), "false ? 1 / 0 : 9007199254740993").evaluate().value.as_int64(),
       9007199254740993LL);
   BOOST_CHECK_EQUAL(
       expression::program(shared_runtime(), "math.mul_div(-5, 1, 2, rounding.half_even)").evaluate().value.as_int64(),
       -2);
}

BOOST_AUTO_TEST_CASE(expression_forbidden_types_and_operations_in_all_branches) {
   for (std::string_view source :
        {"1.0", "true ? 1 : int(1.0)", "true ? 1 : int('1')", "true ? 1 : size(b'abc')",
         "true ? 1 : timestamp('2026-01-01T00:00:00Z').getFullYear()", "null", "int", "[1, 2]", "[1, 2] == [1, 2]",
         "'a' + 'b' == 'ab'", "'abc'.matches('.*')", "[1, 2].map(x, x + 1)", "[1, 2].all(x, x > 0)", "[[1]][0][0]",
         "true ? 1 : math.mul_div(1u, 1, 1, 0)"}) {
      BOOST_TEST_CONTEXT(source) {
         BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), source), forge::exceptions::base,
                               error_is<errors::code::invalid_program>);
      }
   }
}

BOOST_AUTO_TEST_CASE(expression_strict_inputs_and_runtime_errors) {
   const std::array variables = {variable{"x"}};
   expression::program program(shared_runtime(), "x + 1", variables);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(), forge::exceptions::base, error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", true}), forge::exceptions::base,
                         error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", std::uint64_t{1}}), forge::exceptions::base,
                         error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(mutable_variant_object()("x", 1)("unexpected", 2)),
                         forge::exceptions::base, error_is<errors::code::invalid_input>);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(variant_object{"x", std::numeric_limits<std::int64_t>::max()}),
                         forge::exceptions::base, error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1 / 0").evaluate(), forge::exceptions::base,
                         error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "{720: 3}[1080]").evaluate(),
                         forge::exceptions::base, error_is<errors::code::evaluation_failed>);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "[1][2]").evaluate(), forge::exceptions::base,
                         error_is<errors::code::evaluation_failed>);
   const std::array duplicate = {variable{"x"}, variable{"x"}};
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "x", duplicate), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   const std::array invalid = {variable{"rounding.ceil"}};
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1", invalid), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
}

BOOST_AUTO_TEST_CASE(expression_source_node_depth_and_profile_budgets) {
   expression::limits budget;
   budget.source_bytes = 3;
   BOOST_CHECK_EQUAL(expression::program(shared_runtime(), "123", {}, budget).evaluate().value.as_int64(), 123);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1234", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.nodes = 3;
   BOOST_CHECK_EQUAL(expression::program(shared_runtime(), "1 + 2", {}, budget).evaluate().value.as_int64(), 3);
   budget.nodes = 2;
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1 + 2", {}, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   // Parsed qualified calls contain the namespace receiver, which CEL's
   // checker removes. Both parsed and checked trees must fit the allowance.
   budget.nodes = 6;
   BOOST_CHECK_EQUAL(
       expression::program(shared_runtime(), "math.mul_div(1, 2, 1, 0)", {}, budget).evaluate().value.as_int64(), 2);
   budget.nodes = 5;
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "math.mul_div(1, 2, 1, 0)", {}, budget),
                         forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.depth = 2;
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1 + (2 + (3 + 4))", {}, budget),
                         forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.nodes = 0;
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1", {}, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   budget = {};
   budget.variables = 0;
   const std::array variables = {variable{"x"}};
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "x", variables, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
   budget = {};
   ++budget.source_bytes;
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "1", {}, budget), forge::exceptions::base,
                         error_is<errors::code::invalid_profile>);
}

BOOST_AUTO_TEST_CASE(expression_input_and_literal_budgets) {
   expression::limits budget;
   budget.collection_elements = 2;
   const std::array variables = {variable{"items", type::list_int64}};
   expression::program program(shared_runtime(), "items.size()", variables, budget);
   BOOST_CHECK_EQUAL(
       program.evaluate(variant_object{"items", forge::variants{variant{1}, variant{2}}}).value.as_int64(), 2);
   BOOST_CHECK_EXCEPTION(
       (void)program.evaluate(variant_object{"items", forge::variants{variant{1}, variant{2}, variant{3}}}),
       forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "[1, 2, 3].size()", {}, budget),
                         forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.string_bytes = 3;
   const std::array strings = {variable{"x", type::string}};
   expression::program string_program(shared_runtime(), "x == 'abc'", strings, budget);
   BOOST_CHECK(string_program.evaluate(variant_object{"x", "abc"}).value.as_bool());
   BOOST_CHECK_EXCEPTION((void)string_program.evaluate(variant_object{"x", "abcd"}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "'abcd' == 'abc'", {}, budget),
                         forge::exceptions::base, error_is<errors::code::resource_exhausted>);
   const std::array maps = {variable{"x", type::map_int64}};
   BOOST_CHECK_EQUAL(expression::program(shared_runtime(), "x.abc", maps, budget)
                         .evaluate(variant_object{"x", variant_object{"abc", 1}})
                         .value.as_int64(),
                     1);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), "x.abcd", maps, budget), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   budget = {};
   budget.total_input_bytes = 8;
   const std::array integers = {variable{"x"}};
   expression::program integer_program(shared_runtime(), "x", integers, budget);
   // Input bytes include variable names as well as their typed scalar payload.
   BOOST_CHECK_EXCEPTION((void)integer_program.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
}

BOOST_AUTO_TEST_CASE(expression_ast_budget_boundaries_reset_and_concurrency) {
   const std::array variables = {variable{"x"}};
   expression::program baseline(shared_runtime(), "x + 1", variables);
   const auto count = baseline.evaluate(variant_object{"x", 1}).ast_events;
   BOOST_REQUIRE(count > 1);
   expression::limits budget;
   budget.ast_events = count;
   expression::program exact(shared_runtime(), "x + 1", variables, budget);
   BOOST_CHECK_EQUAL(exact.evaluate(variant_object{"x", 1}).ast_events, count);
   BOOST_CHECK_EQUAL(exact.evaluate(variant_object{"x", 2}).ast_events, count);
   budget.ast_events = count - 1;
   expression::program too_small(shared_runtime(), "x + 1", variables, budget);
   BOOST_CHECK_EXCEPTION((void)too_small.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   BOOST_CHECK_EXCEPTION((void)too_small.evaluate(variant_object{"x", 2}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   expression::program pruned(shared_runtime(), "(x / 0 == 0) || true", variables);
   BOOST_CHECK(pruned.evaluate(variant_object{"x", 1}).value.as_bool());
   budget.ast_events = 1;
   expression::program interrupted(shared_runtime(), "(x / 0 == 0) || true", variables, budget);
   BOOST_CHECK_EXCEPTION((void)interrupted.evaluate(variant_object{"x", 1}), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   // Concurrent caller invocations must preserve independent input state and counters.
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
      return expression::program(shared_runtime(), source, variables);
   }();
   auto moved = std::move(program);
   BOOST_CHECK_EQUAL(moved.evaluate(variant_object{"x", 3}).value.as_int64(), 4);
   BOOST_CHECK_EXCEPTION((void)program.evaluate(), forge::exceptions::base, error_is<errors::code::invalid_profile>);
   const std::string secret = "customer-secret-marker";
   try {
      expression::program invalid(shared_runtime(), secret);
      BOOST_FAIL("invalid source was accepted");
   } catch (const forge::exceptions::base& error) {
      BOOST_CHECK(error_is<errors::code::invalid_program>(error));
      BOOST_CHECK(std::string(error.what()).find(secret) == std::string::npos);
   }
}

BOOST_AUTO_TEST_CASE(expression_runtime_memory_snapshot) {
   auto& runtime = shared_runtime();
   const auto before = runtime.memory();
   BOOST_CHECK_EQUAL(before.capacity, 64 * 1024 * 1024);
   BOOST_CHECK_LE(before.used, before.peak);
   BOOST_CHECK_LE(before.peak, before.capacity);
   {
      expression::program program(runtime, "math.mul_div(7, 1, 2, rounding.ceil)");
      BOOST_CHECK_EQUAL(program.evaluate().value.as_int64(), 4);
      const auto prepared = runtime.memory();
      BOOST_CHECK_EQUAL(prepared.capacity, before.capacity);
      BOOST_CHECK_GT(prepared.used, 0);
      BOOST_CHECK_LE(prepared.used, prepared.peak);
      BOOST_CHECK_LE(prepared.peak, prepared.capacity);
      BOOST_CHECK_GE(prepared.peak, before.peak);
      BOOST_CHECK_EQUAL(prepared.failures, before.failures);
   }
   const auto released = runtime.memory();
   BOOST_CHECK_EQUAL(released.capacity, before.capacity);
   BOOST_CHECK_LE(released.used, released.peak);
   BOOST_CHECK_LE(released.peak, released.capacity);
}

BOOST_AUTO_TEST_CASE(expression_async_api_and_injected_runtime_lifecycle) {
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-test"});
   forge::asio::blocking::run(caller, async_roundtrip(shared_runtime()));
   auto injected = std::make_unique<expression::runtime>(
       caller, expression::runtime::options{.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
   forge::asio::blocking::run(caller, injected_roundtrip(std::move(injected)));
}

BOOST_AUTO_TEST_CASE(expression_program_cannot_outlive_its_runtime_context) {
   std::optional<expression::program> previous;
   {
      expression::runtime runtime({.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
      previous.emplace(runtime, "42");
      BOOST_CHECK_EQUAL(previous->evaluate().value.as_int64(), 42);
   }
   BOOST_CHECK_EXCEPTION((void)previous->evaluate(), forge::exceptions::base,
                         error_is<errors::code::worker_unavailable>);
}

BOOST_AUTO_TEST_CASE(expression_late_prepare_cancellation_retires_the_accepted_program) {
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-cancel"});
   expression::runtime runtime({.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
   const auto initial = runtime.memory();
   boost::asio::cancellation_signal cancellation;
   const auto completion = std::make_shared<std::promise<std::exception_ptr>>();
   auto completed = completion->get_future();
   boost::asio::co_spawn(
       caller.context(), pending_preparation(runtime),
       boost::asio::bind_cancellation_slot(
           cancellation.slot(), [completion](std::exception_ptr error) { completion->set_value(std::move(error)); }));

   std::binary_semaphore resume{0};
   std::promise<void> entered;
   auto blocked = entered.get_future();
   // Test-only delivery barrier. The owned expression IO continues while the
   // public caller's completion is deliberately held on its own executor.
   boost::asio::post(caller.context(), [&] {
      entered.set_value();
      resume.acquire();
   });
   auto unblock = boost::scope::scope_exit{[&] { resume.release(); }};
   BOOST_REQUIRE(blocked.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   blocked.get();
   const auto accepted = runtime.memory(); // FIFO exchange completes after the prepare reply.
   BOOST_REQUIRE_GT(accepted.used, initial.used);
   cancellation.emit(boost::asio::cancellation_type::terminal);
   resume.release();
   unblock.set_active(false);
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   if (const auto error = completed.get()) {
      try {
         std::rethrow_exception(error);
      } catch (const forge::exceptions::base& failure) {
         BOOST_CHECK(error_is<errors::code::cancelled>(failure));
      } catch (const boost::system::system_error& failure) {
         BOOST_CHECK(failure.code() == boost::asio::error::operation_aborted);
      }
   }
   (void)runtime.memory(); // Join any accepted program retirement before checking all slots.
   std::vector<expression::program> all_slots;
   for (int index = 0; index < 64; ++index)
      all_slots.emplace_back(runtime, "1");
   BOOST_CHECK_EQUAL(all_slots.back().evaluate().value.as_int64(), 1);
}

BOOST_AUTO_TEST_CASE(expression_inflight_cancellation_is_typed_and_retires_the_worker) {
   identified_worker fixture;
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-cancel"});
   expression::runtime runtime(caller, {.helper = fixture.helper, .timeout = std::chrono::seconds{5}});
   expression::program program(runtime, "42");
   BOOST_REQUIRE_EQUAL(::kill(fixture.pid(), SIGSTOP), 0);

   boost::asio::cancellation_signal cancellation;
   const auto completion = std::make_shared<std::promise<std::exception_ptr>>();
   auto completed = completion->get_future();
   boost::asio::co_spawn(caller.context(), program.evaluate_async({}),
                         boost::asio::bind_cancellation_slot(
                             cancellation.slot(), [completion](std::exception_ptr error, expression::evaluation) {
                                completion->set_value(std::move(error));
                             }));
   // The same single IO worker runs the request and the cancellation timer.
   // A stopped real child cannot finish its response before cancellation.
   forge::asio::blocking::run(caller, cancel_after_started(cancellation));
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   const auto error = completed.get();
   BOOST_REQUIRE(error != nullptr);
   BOOST_CHECK_EXCEPTION(std::rethrow_exception(error), forge::exceptions::base, error_is<errors::code::cancelled>);
   BOOST_CHECK_EXCEPTION((void)runtime.memory(), forge::exceptions::base, error_is<errors::code::worker_unavailable>);
   forge::asio::blocking::run(caller, runtime.shutdown_async());
}

BOOST_AUTO_TEST_CASE(expression_artifact_survives_a_fresh_worker) {
   forge::raw::bytes saved;
   {
      expression::runtime runtime({.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
      const std::array variables = {variable{"x"}};
      expression::program compiled(runtime, "math.mul_div(x, 7, 3, rounding.ceil)", variables);
      saved = expression::encode_artifact(compiled.artifact());
   }
   // Both the original program and its complete native worker have gone away.
   expression::runtime runtime({.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
   const auto artifact = expression::decode_artifact(saved);
   expression::program loaded(runtime, artifact);
   BOOST_CHECK(loaded.artifact() == artifact);
   BOOST_CHECK_EQUAL(loaded.evaluate(variant_object{"x", std::int64_t{9007199254740993LL}}).value.as_int64(),
                     21016798261062317LL);
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-load"});
   auto asynchronous = forge::asio::blocking::run(caller, expression::program::load_async(runtime, artifact));
   BOOST_CHECK_EQUAL(asynchronous.evaluate(variant_object{"x", 3}).value.as_int64(), 7);
}

BOOST_AUTO_TEST_CASE(expression_artifact_checks_fresh_types_even_after_rehashing) {
   const std::array variables = {variable{"x"}};
   const auto original = expression::program(shared_runtime(), "x + 1", variables).artifact();
   auto forged = original;
   forged.result = type::boolean;
   forged.digest = expression::artifact_digest(forged);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), forged), forge::exceptions::base,
                         error_is<errors::code::invalid_program>);
   forged = original;
   forged.variables.front().kind = type::uint64;
   forged.digest = expression::artifact_digest(forged);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), forged), forge::exceptions::base,
                         error_is<errors::code::invalid_program>);
   forged = original;
   forged.budget.nodes = 2;
   forged.digest = expression::artifact_digest(forged);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), forged), forge::exceptions::base,
                         error_is<errors::code::resource_exhausted>);
   forged = original;
   forged.expression.push_back(0); // Invalid protobuf, independently rejected by the child.
   forged.digest = expression::artifact_digest(forged);
   BOOST_CHECK_EXCEPTION((void)expression::program(shared_runtime(), forged), forge::exceptions::base,
                         error_is<errors::code::invalid_program>);
   BOOST_CHECK_EQUAL(expression::program(shared_runtime(), original).evaluate(variant_object{"x", 41}).value.as_int64(),
                     42);
}

BOOST_AUTO_TEST_CASE(expression_operation_early_cancel_and_deadline_preserve_the_helper) {
   expression::runtime runtime({.helper = std::filesystem::path{FORGE_EXPRESSION_WORKER}});
   expression::program compiled(runtime, "42");
   std::stop_source cancelled;
   cancelled.request_stop();
   expression::operation_options operation{.cancel = cancelled.get_token()};
   BOOST_CHECK_EXCEPTION((void)compiled.evaluate({}, operation), forge::exceptions::base,
                         error_is<errors::code::cancelled>);
   BOOST_CHECK_EXCEPTION((void)expression::program(runtime, compiled.artifact(), operation), forge::exceptions::base,
                         error_is<errors::code::cancelled>);
   operation = {.deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds{1}};
   BOOST_CHECK_EXCEPTION((void)compiled.evaluate({}, operation), forge::exceptions::base,
                         error_is<errors::code::deadline_exceeded>);
   BOOST_CHECK_EQUAL(compiled.evaluate().value.as_int64(), 42);
}

BOOST_AUTO_TEST_CASE(expression_operation_cancel_while_queued_preserves_the_active_exchange) {
   identified_worker fixture;
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-queue"});
   expression::runtime runtime(caller, {.helper = fixture.helper, .timeout = std::chrono::seconds{5}});
   expression::program compiled(runtime, "42");
   BOOST_REQUIRE_EQUAL(::kill(fixture.pid(), SIGSTOP), 0);
   auto resume = boost::scope::scope_exit{[&] { ::kill(fixture.pid(), SIGCONT); }};
   auto active = boost::asio::co_spawn(caller.context(), compiled.evaluate_async(), boost::asio::use_future);
   std::stop_source stop;
   auto queued = boost::asio::co_spawn(caller.context(), compiled.evaluate_async({}, {.cancel = stop.get_token()}),
                                       boost::asio::use_future);
   forge::asio::blocking::run(caller, stop_after_started(stop));
   BOOST_REQUIRE(queued.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION((void)queued.get(), forge::exceptions::base, error_is<errors::code::cancelled>);
   BOOST_REQUIRE_EQUAL(::kill(fixture.pid(), SIGCONT), 0);
   resume.set_active(false);
   BOOST_REQUIRE(active.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_CHECK_EQUAL(active.get().value.as_int64(), 42);
   BOOST_CHECK_EQUAL(compiled.evaluate().value.as_int64(), 42);
   forge::asio::blocking::run(caller, runtime.shutdown_async());
}

BOOST_AUTO_TEST_CASE(expression_operation_absolute_deadline_bounds_a_dispatched_load) {
   const auto artifact = expression::program(shared_runtime(), "42").artifact();
   identified_worker fixture;
   forge::asio::runtime caller({.worker_threads = 1, .thread_name = "expression-load"});
   expression::runtime runtime(caller, {.helper = fixture.helper, .timeout = std::chrono::seconds{5}});
   BOOST_REQUIRE_EQUAL(::kill(fixture.pid(), SIGSTOP), 0);
   const auto started = std::chrono::steady_clock::now();
   const expression::operation_options operation{.deadline = started + std::chrono::milliseconds{100}};
   BOOST_CHECK_EXCEPTION(
       (void)forge::asio::blocking::run(caller, expression::program::load_async(runtime, artifact, operation)),
       forge::exceptions::base, error_is<errors::code::deadline_exceeded>);
   BOOST_CHECK_LT(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), 2000);
   BOOST_CHECK_EXCEPTION((void)runtime.memory(), forge::exceptions::base, error_is<errors::code::worker_unavailable>);
   forge::asio::blocking::run(caller, runtime.shutdown_async());
}
