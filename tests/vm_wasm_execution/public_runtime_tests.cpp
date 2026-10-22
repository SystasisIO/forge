#include <boost/test/unit_test.hpp>
#include <array>
#include <cstdint>
#include <string_view>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.prepared_module;
import forge.vm.wasm.interpret.backend;

namespace {
namespace vm = forge::vm::wasm::interpret;

fixtures::bytes capacity_module(std::uint32_t storage) {
   fixtures::bytes result{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(result, 1, {1, 0x60, 3, 0x7e, 0x7e, 0x7e, 0});
   fixtures::section(result, 3, {1, 0});
   fixtures::section(result, 7, {1, 5, 'a', 'p', 'p', 'l', 'y', 0, 0});
   fixtures::bytes body{1};
   fixtures::size(body, (storage - 28) / 4);
   body.insert(body.end(), {0x7f, 0x41, 1, 0x1a, 0x0b});
   fixtures::bytes code{1};
   fixtures::size(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   fixtures::section(result, 10, code);
   return result;
}

fixtures::bytes recursive_module(std::uint32_t storage) {
   fixtures::bytes result{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(result, 1, {1, 0x60, 1, 0x7f, 1, 0x7f});
   fixtures::section(result, 3, {1, 0});
   fixtures::section(result, 7, {1, 3, 'r', 'u', 'n', 0, 0});
   fixtures::bytes body{1};
   fixtures::size(body, (storage - 12) / 4);
   body.insert(body.end(), {0x7f, 0x20, 0, 0x04, 0x7f, 0x20, 0, 0x41, 1, 0x6b, 0x10, 0, 0x05, 0x41, 0, 0x0b, 0x0b});
   fixtures::bytes code{1};
   fixtures::size(code, body.size());
   code.insert(code.end(), body.begin(), body.end());
   fixtures::section(result, 10, code);
   return result;
}
} // namespace

BOOST_AUTO_TEST_CASE(public_capacity_counts_parameters_and_cannot_be_disabled_by_caller_flags) {
   auto policy = vm::resolve_options(vm::default_options{});
   policy.max_func_local_bytes = 0;
   policy.max_func_local_bytes_flags = static_cast<vm::max_func_local_bytes_flags_t>(0);
   auto exact = capacity_module(vm::capacity_profile::max_function_storage_bytes);
   auto admitted = vm::prepared_module{exact, policy};
   BOOST_CHECK_NO_THROW(admitted.validate());
   auto excess = capacity_module(vm::capacity_profile::max_function_storage_bytes + 4);
   BOOST_CHECK_THROW(vm::prepared_module(excess, policy), vm::exceptions::profile);
   auto context = vm::runtime_context{admitted, 1024};
   context.initialize(nullptr);
   const std::array<vm::operand_stack_elem, 3> args{vm::i64_const_t{UINT64_C(0)}, vm::i64_const_t{UINT64_C(0)},
                                                    vm::i64_const_t{UINT64_C(0)}};
   BOOST_CHECK(!context.call(nullptr, "apply", args).has_value());
   const auto accounting = admitted.accounted_module_memory();
   BOOST_TEST(accounting.source_buffer_bytes >= exact.size());
   BOOST_TEST(accounting.upstream_source_bytes > 0);
}

BOOST_AUTO_TEST_CASE(public_interpreter_provisions_admitted_frames_at_configured_depth_and_recovers) {
   for (const auto storage : {8192U, 16384U, vm::capacity_profile::max_function_storage_bytes}) {
      for (const auto depth : {251U, 1024U}) {
         auto policy = vm::resolve_options(vm::default_options{});
         policy.max_call_depth = depth;
         auto source = recursive_module(storage);
         auto admitted = vm::prepared_module{source, policy};
         auto context = vm::runtime_context{admitted, depth};
         context.initialize(nullptr);
         std::array<vm::operand_stack_elem, 1> args{vm::i32_const_t{depth - 1}};
         BOOST_TEST(context.call(nullptr, "run", args)->to_ui32() == 0);
         args[0] = vm::i32_const_t{depth};
         BOOST_CHECK_THROW(context.call(nullptr, "run", args), vm::exceptions::interpreter);
         args[0] = vm::i32_const_t{0};
         BOOST_TEST(context.call(nullptr, "run", args)->to_ui32() == 0);
      }
   }
}

BOOST_AUTO_TEST_CASE(public_validation_projects_unlinked_imports_without_enabling_execution) {
   auto source = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(source, 1, {1, 0x60, 3, 0x7e, 0x7e, 0x7e, 0});
   fixtures::section(source, 2, {1, 3, 'e', 'n', 'v', 7, 'u', 'n', 'k', 'n', 'o', 'w', 'n', 0, 0});
   fixtures::section(source, 3, {1, 0});
   fixtures::section(source, 7, {1, 5, 'a', 'p', 'p', 'l', 'y', 0, 1});
   fixtures::section(source, 10, {1, 2, 0, 0x0b});
   using validator = vm::backend<std::nullptr_t, vm::null_backend>;
   auto validated = validator{source, nullptr};
   const auto text = [](const auto& value) {
      return std::string_view{reinterpret_cast<const char*>(value.data()), value.size()};
   };
   auto& module = validated.get_module();
   BOOST_REQUIRE_EQUAL(module.imports.size(), 1);
   BOOST_TEST(text(module.imports[0].module_str) == "env");
   BOOST_TEST(text(module.imports[0].field_str) == "unknown");
   const auto expected = vm::host_function{{vm::types::i64, vm::types::i64, vm::types::i64}, {}};
   BOOST_CHECK(expected == module.get_function_type(module.get_exported_function("apply")));
   using context_type = vm::interpreter::context<std::nullptr_t>;
   auto context = context_type{module, 251};
   BOOST_CHECK_NO_THROW(context.initialize_globals());
   BOOST_CHECK_THROW(context.initialize(nullptr), vm::exceptions::invocation_state);
   using runtime = vm::backend<std::nullptr_t, vm::interpreter>;
   auto executable = runtime{source, nullptr};
   BOOST_CHECK_NO_THROW(executable.call("env", "apply", UINT64_C(0), UINT64_C(0), UINT64_C(0)));
   BOOST_CHECK_THROW(executable.call(nullptr, 0, UINT64_C(0), UINT64_C(0), UINT64_C(0)), vm::exceptions::interpreter);
   BOOST_CHECK_THROW(vm::prepared_module(source, vm::options{}), vm::exceptions::link);
}

BOOST_AUTO_TEST_CASE(public_default_page_budget_intersects_capacity) {
   auto policy = vm::options{};
   auto source = capacity_module(32);
   auto prepared = vm::prepared_module{source, policy};
   BOOST_CHECK_NO_THROW(prepared.check_limits(policy));
   auto context = vm::runtime_context{prepared, policy.max_call_depth};
   BOOST_CHECK_NO_THROW(context.set_max_pages(policy.max_pages));
   BOOST_CHECK_NO_THROW(context.initialize(nullptr));
}

BOOST_AUTO_TEST_CASE(public_admission_rejects_multiple_results_before_projection) {
   auto source = fixtures::multiple_results();
   BOOST_CHECK_THROW(vm::prepared_module(source, vm::options{}), vm::exceptions::profile);
   using validator = vm::backend<std::nullptr_t, vm::null_backend>;
   BOOST_CHECK_THROW(validator(source, nullptr), vm::exceptions::profile);
}

BOOST_AUTO_TEST_CASE(public_page_profile_rejects_unrepresentable_source_and_explicit_budgets_before_allocation) {
   static_assert(vm::capacity_profile::max_pages == 65535);
   const auto source = [](std::uint32_t initial, bool maximum) {
      auto bytes = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      auto memory = fixtures::bytes{1, static_cast<std::uint8_t>(maximum)};
      fixtures::size(memory, initial);
      if (maximum)
         fixtures::size(memory, initial);
      fixtures::section(bytes, 5, memory);
      return bytes;
   };
   using purpose = vm::prepared_module::purpose;
   for (const auto maximum : {false, true}) {
      BOOST_CHECK_NO_THROW(vm::prepared_module(source(65535, maximum), vm::options{}, {}, purpose::validation_only));
      BOOST_CHECK_THROW(vm::prepared_module(source(65536, maximum), vm::options{}, {}, purpose::validation_only),
                        vm::exceptions::profile);
      BOOST_CHECK_NO_THROW(vm::prepared_module(source(0, maximum), vm::options{}, {}, purpose::validation_only));
   }
   auto policy = vm::options{};
   policy.max_pages = 65536;
   BOOST_CHECK_THROW(vm::prepared_module(source(0, false), policy, {}, purpose::validation_only),
                     vm::exceptions::profile);
   auto module = vm::prepared_module{source(0, false), vm::options{}};
   BOOST_CHECK_THROW(module.check_limits(policy), vm::exceptions::profile);
   auto context = vm::runtime_context{module, 251};
   BOOST_CHECK_THROW(context.set_max_pages(65536), vm::exceptions::profile);
   BOOST_CHECK_THROW(context.set_max_pages(UINT32_MAX - 1), vm::exceptions::profile);
   BOOST_CHECK_NO_THROW(context.set_max_pages(65535));
   BOOST_CHECK_NO_THROW(context.set_max_pages(UINT32_MAX));
}

BOOST_AUTO_TEST_CASE(public_interpreter_dispatches_unexported_table_functions_and_imports) {
   auto source = fixtures::private_table();
   std::uint32_t calls = 0;
   auto binding = vm::host_binding{
       "env", "bump", {{vm::types::i32}, {vm::types::i32}}, [](void* host, vm::execution_interface interface) {
          ++*static_cast<std::uint32_t*>(host);
          const auto argument = interface.pop_operand().to_ui32();
          interface.push_operand(vm::i32_const_t{argument + 1});
       }};
   auto prepared = vm::prepared_module{source, vm::options{}, {binding}};
   BOOST_CHECK(prepared.function_exports().empty());
   auto context = vm::runtime_context{prepared, 251};
   context.initialize(&calls);
   const std::array<vm::operand_stack_elem, 1> args{vm::i32_const_t{10}};
   BOOST_TEST(context.call_indirect(&calls, 0, args)->to_ui32() == 52);
   BOOST_TEST(context.call_indirect(&calls, 1, args)->to_ui32() == 11);
   BOOST_TEST(context.call(&calls, 1, args)->to_ui32() == 52);
   BOOST_TEST(context.call(&calls, 0, args)->to_ui32() == 11);
   BOOST_TEST(calls == 2);
}

BOOST_AUTO_TEST_CASE(public_profile_rejects_signed_instance_metadata_extent_without_allocating_the_table) {
   const auto source = [](std::uint32_t entries) {
      auto bytes = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      auto table = fixtures::bytes{1, 0x70, 0};
      fixtures::size(table, entries);
      fixtures::section(bytes, 4, table);
      return bytes;
   };
   auto low = std::uint32_t{0};
   auto high = vm::capacity_profile::max_instance_metadata_bytes / sizeof(void*) + 1;
   while (high - low > 1) {
      const auto middle = static_cast<std::uint32_t>(low + (high - low) / 2);
      try {
         auto module =
             vm::prepared_module{source(middle), vm::options{}, {}, vm::prepared_module::purpose::validation_only};
         low = middle;
      } catch (const vm::exceptions::profile&) {
         high = middle;
      }
   }
   BOOST_TEST(low > 0);
   BOOST_CHECK_NO_THROW(
       vm::prepared_module(source(low), vm::options{}, {}, vm::prepared_module::purpose::validation_only));
   BOOST_CHECK_THROW(
       vm::prepared_module(source(high), vm::options{}, {}, vm::prepared_module::purpose::validation_only),
       vm::exceptions::profile);
   using validator = vm::backend<std::nullptr_t, vm::null_backend>;
   BOOST_CHECK_THROW(validator(source(high), nullptr), vm::exceptions::profile);
}

BOOST_AUTO_TEST_CASE(public_logical_depth_trap_keeps_base_exception_compatibility) {
   auto source = recursive_module(32);
   auto prepared = vm::prepared_module{source, vm::options{}};
   auto context = vm::runtime_context{prepared, 2};
   context.initialize(nullptr);
   const std::array<vm::operand_stack_elem, 1> args{vm::i32_const_t{2}};
   BOOST_CHECK_EXCEPTION(context.call(nullptr, "run", args), vm::exceptions::guest_trap, [](const auto& error) {
      return error.kind == vm::exceptions::guest_trap_kind::logical_depth;
   });
   BOOST_CHECK_THROW(context.call(nullptr, "run", args), vm::exceptions::interpreter);
}

BOOST_AUTO_TEST_CASE(public_profile_rejects_malformed_legacy_permissions_with_either_requested_value) {
   const auto function = [](fixtures::bytes body) {
      auto source = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      fixtures::section(source, 1, {1, 0x60, 0, 0});
      fixtures::section(source, 3, {1, 0});
      auto code = fixtures::bytes{1};
      fixtures::size(code, body.size());
      code.insert(code.end(), body.begin(), body.end());
      fixtures::section(source, 10, code);
      return source;
   };
   std::vector<fixtures::bytes> malformed{function({0, 0x0b, 0x01}), function({1, 0, 0, 0x0b})};
   for (const auto table : {false, true}) {
      for (const auto maximum : {false, true}) {
         auto source = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
         auto limits = fixtures::bytes{1};
         if (table)
            limits.push_back(0x70);
         fixtures::size(limits, maximum ? 0x3fff : 0x3ffe);
         limits.push_back(0);
         if (maximum)
            limits.push_back(1);
         fixtures::section(source, table ? 4 : 5, limits);
         malformed.push_back(std::move(source));
      }
   }
   for (const auto requested : {false, true}) {
      auto policy = vm::options{};
      policy.allow_code_after_function_end = requested;
      policy.allow_invalid_empty_local_set = requested;
      policy.allow_u32_limits_flags = requested;
      for (const auto& source : malformed)
         BOOST_CHECK_THROW(vm::prepared_module(source, policy, {}, vm::prepared_module::purpose::validation_only),
                           vm::exceptions::parse);
   }
   using validator = vm::backend<std::nullptr_t, vm::null_backend, vm::compatibility_options>;
   BOOST_CHECK_NO_THROW(validator(capacity_module(32), nullptr));
}
