#include <boost/test/unit_test.hpp>
#include <chrono>
#include <stop_token>
#include <type_traits>
#include "fixtures.hxx"

import forge.execution.wasm.compiled_module;
import forge.execution.wasm.profile;

namespace execution = forge::execution::wasm;
using namespace std::chrono_literals;

BOOST_AUTO_TEST_CASE(default_policy_and_canonical_types) {
   static_assert(!std::is_copy_constructible_v<execution::compiled_module>);
   static_assert(std::is_same_v<decltype(execution::invocation::receiver), forge::chain::protocol::account_name>);
   static_assert(std::is_same_v<decltype(execution::invocation_result::data), forge::raw::bytes>);
   const auto compilation = execution::compile_limits{};
   const auto invocation = execution::invocation_limits{};
   BOOST_CHECK_EQUAL(compilation.max_module_bytes, 4 * 1024 * 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_pages, 256);
   BOOST_CHECK_EQUAL(compilation.vm.max_call_depth, 256);
   BOOST_CHECK_EQUAL(compilation.vm.max_import_section_elements, 64);
   BOOST_CHECK_EQUAL(compilation.vm.max_mutable_global_bytes, 1024 * 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_table_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_type_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_function_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_global_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_export_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_element_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_data_section_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_element_segment_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_data_segment_bytes, 4 * 1024 * 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_linear_memory_init, 256 * 65536);
   BOOST_CHECK_EQUAL(compilation.vm.max_func_local_bytes, 1024 * 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_local_sets, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_nested_structures, 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_br_table_elements, 16384);
   BOOST_CHECK_EQUAL(compilation.vm.max_symbol_bytes, 1024);
   BOOST_CHECK_EQUAL(compilation.vm.max_memory_offset, UINT32_MAX);
   BOOST_CHECK_EQUAL(compilation.vm.max_code_bytes, 4 * 1024 * 1024);
   BOOST_CHECK(!compilation.vm.forbid_export_mutable_globals);
   BOOST_CHECK(!compilation.vm.allow_code_after_function_end);
   BOOST_CHECK(!compilation.vm.allow_u32_limits_flags);
   BOOST_CHECK(!compilation.vm.allow_invalid_empty_local_set);
   BOOST_CHECK(!compilation.vm.allow_zero_blocktype);
   BOOST_CHECK_EQUAL(static_cast<int>(compilation.vm.max_func_local_bytes_flags), 6);
   BOOST_CHECK_EQUAL(invocation.budget.count(), 100);
   BOOST_CHECK_EQUAL(invocation.max_input_bytes, 2 * 1024 * 1024);
   BOOST_CHECK_EQUAL(invocation.max_output_bytes, 2 * 1024 * 1024);
   BOOST_CHECK_EQUAL(invocation.max_console_bytes, 0);
   BOOST_CHECK(execution::allows_intrinsic("env", "read_action_data"));
   BOOST_CHECK(!execution::allows_intrinsic("env", "require_auth"));
   auto invalid = invocation;
   invalid.budget = 0ms;
   BOOST_CHECK_THROW(execution::validate(invalid), execution::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(admission_never_runs_start_and_rejects_imports) {
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module({}, true, "require_auth")),
                     execution::exceptions::forbidden_import);
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module({}, true, "db_store_i64")),
                     execution::exceptions::forbidden_import);
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module({}, true, "action_data_size", true)),
                     execution::exceptions::forbidden_import);
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module({}, true, "action_data_size", false, "other")),
                     execution::exceptions::invalid_apply);
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module({}, true, "action_data_size", false, "apply", true)),
                     execution::exceptions::invalid_apply);
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::bytes{0, 1, 2}), execution::exceptions::invalid_wasm);
   // A well-formed imported memory must be rejected as profile policy, not malformed WASM.
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::bytes{0,0x61,0x73,0x6d,1,0,0,0,
      2,12,1,3,'e','n','v',3,'m','e','m',2,0,1}), execution::exceptions::forbidden_import);
   auto limits = execution::compile_limits{};
   limits.max_module_bytes = 1;
   BOOST_CHECK_THROW(execution::compiled_module(fixtures::module(), limits), execution::exceptions::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(owned_output_and_move) {
   const auto input = fixtures::bytes{1, 2, 3, 0, 255};
   auto result = execution::invocation_result{};
   {
      auto module = execution::compiled_module{fixtures::module()};
      BOOST_CHECK_GT(module.bytes(), 0);
      auto moved = std::move(module);
      BOOST_CHECK_EQUAL(module.bytes(), 0);
      BOOST_CHECK_THROW((void)module.invoke({}), execution::exceptions::unusable);
      result = moved.invoke({.data = input});
   }
   BOOST_CHECK(result.data == input);
   BOOST_CHECK(result.console.empty());
}

BOOST_AUTO_TEST_CASE(input_return_and_console_bounds) {
   const auto input = fixtures::bytes{'x', 'y'};
   auto module = execution::compiled_module{fixtures::module()};
   auto limits = execution::invocation_limits{};
   limits.max_input_bytes = 1;
   BOOST_CHECK_THROW((void)module.invoke({.data = input}, limits), execution::exceptions::limit_exceeded);
   limits.max_input_bytes = 2;
   limits.max_output_bytes = 1;
   BOOST_CHECK_THROW((void)module.invoke({.data = input}, limits), execution::exceptions::limit_exceeded);
   BOOST_CHECK_THROW((void)module.invoke({}), execution::exceptions::unusable);

   auto print = fixtures::echo();
   print.insert(print.end(), {0x41, 0, 0x10, 0, 0x10, 4});
   auto console = execution::compiled_module{fixtures::module(print)};
   BOOST_CHECK(console.invoke({.data = input}).console.empty());
   limits.max_output_bytes = 2;
   limits.max_console_bytes = 2;
   BOOST_CHECK_EQUAL(console.invoke({.data = input}, limits).console, "xy");
   limits.max_console_bytes = 1;
   BOOST_CHECK_THROW((void)console.invoke({.data = input}, limits), execution::exceptions::limit_exceeded);
}

BOOST_AUTO_TEST_CASE(memory_and_globals_start_fresh_each_time) {
   // Increment memory[0] and a mutable global, write the global to memory[1].
   const auto body = fixtures::bytes{0x41,0, 0x41,0, 0x2d,0,0, 0x41,1, 0x6a, 0x3a,0,0,
      0x23,0, 0x41,1, 0x6a, 0x24,0, 0x41,1, 0x23,0, 0x3a,0,0,
      0x41,0, 0x41,2, 0x10,2};
   auto module = execution::compiled_module{fixtures::module(body)};
   for (int i = 0; i != 8; ++i) BOOST_CHECK(module.invoke({}).data == fixtures::bytes({1, 1}));
}

BOOST_AUTO_TEST_CASE(typed_assertion_trap_and_start_deadline) {
   auto assertion = execution::compiled_module{fixtures::module({0x41,0, 0x41,0, 0x41,0, 0x10,3})};
   BOOST_CHECK_THROW((void)assertion.invoke({}), execution::exceptions::assertion_failed);
   BOOST_CHECK_THROW((void)assertion.invoke({}), execution::exceptions::unusable);
   auto trap = execution::compiled_module{fixtures::module({0})};
   BOOST_CHECK_THROW((void)trap.invoke({}), execution::exceptions::trap);
   BOOST_CHECK_THROW((void)trap.invoke({}), execution::exceptions::trap);
   auto start = execution::compiled_module{fixtures::module({}, true)};
   auto limits = execution::invocation_limits{.budget = 10ms};
   BOOST_CHECK_THROW((void)start.invoke({}, limits), execution::exceptions::deadline_exceeded);
   auto stop = std::stop_source{};
   stop.request_stop();
   BOOST_CHECK_THROW((void)start.invoke({}, limits, stop.get_token()), execution::exceptions::canceled);
}

BOOST_AUTO_TEST_CASE(call_depth_overflow_is_a_guest_trap_and_preserves_reuse) {
   // Apply recurses using action as a countdown, then echoes through host imports.
   auto body = fixtures::bytes{0x20, 2, 0x50, 0x04, 0x40};
   const auto echo = fixtures::echo();
   body.insert(body.end(), echo.begin(), echo.end());
   body.insert(body.end(), {0x05, 0x20, 0, 0x20, 1, 0x20, 2, 0x42, 1, 0x7d, 0x10, 5, 0x0b});
   const auto code = fixtures::module(body);
   auto limits = execution::compile_limits{};
   limits.vm.max_call_depth = 4;
   auto bounded = execution::compiled_module{code, limits};
   const auto input = fixtures::bytes{42, 17};
   // Eight recursive calls exhaust guest frames. Three fill the four frames
   // exactly, so the first host import must fail before entering its callback.
   for (const auto count : {8U, 3U}) {
      BOOST_CHECK_THROW((void)bounded.invoke({.action = forge::chain::protocol::action_name{count}, .data = input}),
                        execution::exceptions::trap);
      BOOST_CHECK(bounded.invoke({.action = forge::chain::protocol::action_name{1}, .data = input}).data == input);
   }
   limits.vm.max_call_depth = 16;
   auto sufficient = execution::compiled_module{code, limits};
   BOOST_CHECK(sufficient.invoke({.action = forge::chain::protocol::action_name{8}, .data = input}).data == input);
}

BOOST_AUTO_TEST_CASE(canceled_instance_and_sibling_can_execute_again) {
   auto body = fixtures::bytes{0x20,2, 0x50, 0x04,0x40, 0x03,0x40, 0x0c,0, 0x0b, 0x0b};
   const auto echo = fixtures::echo();
   body.insert(body.end(), echo.begin(), echo.end());
   const auto code = fixtures::module(body);
   auto first = execution::compiled_module{code};
   auto second = execution::compiled_module{code};
   auto stop = std::stop_source{};
   // Real concurrent overlap is proven by the lower-level invocation suite's
   // host-entry barriers. This facade check pins typed cancellation and reuse.
   stop.request_stop();
   BOOST_CHECK_THROW((void)first.invoke({}, {.budget = 2s}, stop.get_token()), execution::exceptions::canceled);
   const auto input = fixtures::bytes{42};
   for (int i = 0; i != 16; ++i)
      BOOST_CHECK(second.invoke({.action = forge::chain::protocol::action_name{1}, .data = input}).data == input);
   BOOST_CHECK(first.invoke({.action = forge::chain::protocol::action_name{1}, .data = input}).data == input);
}
