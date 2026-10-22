#include <boost/test/unit_test.hpp>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <type_traits>
#include "fixtures.hxx"

import forge.vm.wasm.interpret.backend;

namespace {
namespace vm = forge::vm::wasm::interpret;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

struct execution_configuration {
   std::uint32_t max_pages;
   std::uint32_t max_call_depth;
};
struct execution_options : execution_configuration {
   static constexpr bool forbid_export_mutable_globals = false;
   static constexpr bool allow_code_after_function_end = false;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
};
execution_options make_execution_options(const execution_configuration& configuration) {
   return {configuration};
}
struct section_options {
   static constexpr std::uint32_t max_section_elements = 7;
   static constexpr std::uint32_t max_type_section_elements = 2;
};
struct absent_checker_limits {
   static constexpr bool check_mutable_global_bytes = true;
   static constexpr bool check_func_local_bytes = true;
   static constexpr bool check_control_depth = true;
   static constexpr bool check_legacy_nested_structures = true;
};
struct present_checker_limits {
   static constexpr std::uint64_t max_mutable_global_bytes = 0;
   static constexpr std::uint64_t max_func_local_bytes = 0;
   static constexpr std::uint64_t max_control_depth = 0;
   static constexpr std::uint64_t max_legacy_nested_structures = 0;
};
struct disabled_checker_limits : present_checker_limits {
   static constexpr bool check_mutable_global_bytes = false;
   static constexpr bool check_func_local_bytes = false;
   static constexpr bool check_control_depth = false;
   static constexpr bool check_legacy_nested_structures = false;
};
struct wide_options {
   static constexpr std::uint64_t max_linear_memory_init = std::uint64_t{1} << 32;
   static constexpr std::uint64_t max_control_depth = (std::uint64_t{1} << 32) + 1;
   static constexpr std::uint64_t max_legacy_nested_structures = (std::uint64_t{1} << 32) + 2;
   static constexpr auto max_func_local_bytes_flags =
       vm::max_func_local_bytes_flags_t::params | vm::max_func_local_bytes_flags_t::locals;
   static constexpr bool forbid_export_mutable_globals = true;
   static constexpr bool allow_code_after_function_end = true;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
   static constexpr bool parse_custom_section_name = true;
};

struct caller_checker_options {
   std::uint64_t max_mutable_global_bytes = 0;
   std::uint64_t max_func_local_bytes = 0;
   std::uint64_t max_control_depth = 0;
   std::uint64_t max_legacy_nested_structures = 0;
   std::uint32_t max_pages = UINT32_MAX;
   bool check_mutable_global_bytes = false;
   bool check_func_local_bytes = false;
   bool check_control_depth = false;
   bool check_legacy_nested_structures = false;
};

vm::wasm_code admission_function(const fixtures::bytes& body) {
   auto code = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(code, 1, {1, 0x60, 0, 0});
   fixtures::section(code, 3, {1, 0});
   auto functions = fixtures::bytes{1};
   fixtures::size(functions, body.size());
   functions.insert(functions.end(), body.begin(), body.end());
   fixtures::section(code, 10, functions);
   return code;
}

vm::wasm_code admission_blocks(std::uint32_t depth) {
   auto body = fixtures::bytes{0};
   for (std::uint32_t i = 0; i < depth; ++i)
      body.insert(body.end(), {0x02, 0x40});
   body.insert(body.end(), depth + 1, 0x0b);
   return admission_function(body);
}

void check_caller_admission(vm::wasm_code code, bool caller_checker_options::* checker) {
   using typed_validator = vm::backend<std::nullptr_t, vm::null_backend, caller_checker_options>;
   using snapshot_validator = vm::backend<std::nullptr_t, vm::null_backend, vm::options>;
   const auto original = code;
   for (const auto enabled : {false, true}) {
      auto policy = caller_checker_options{};
      policy.*checker = enabled;
      const auto resolved = vm::resolve_options(policy);
      if (enabled) {
         BOOST_CHECK_THROW(typed_validator(code, nullptr, policy), vm::exceptions::parse);
         BOOST_CHECK_THROW(snapshot_validator(code, nullptr, resolved), vm::exceptions::parse);
      } else {
         BOOST_CHECK_NO_THROW(typed_validator(code, nullptr, policy));
         BOOST_CHECK_NO_THROW(snapshot_validator(code, nullptr, resolved));
      }
      BOOST_CHECK(code == original);
   }
}

struct consumer_host {
   unsigned calls = 0;
   std::function<void()> work;
   void effect() {
      ++calls;
      if (work)
         work();
   }
};
struct host_error : std::runtime_error {
   host_error() : std::runtime_error("Host failure") {}
};
using functions = vm::registered_host_functions<consumer_host>;
using backend_type = vm::backend<functions, vm::interpreter, vm::options>;
using context_type = vm::interpreter::context<functions>;

struct cached {
   std::unique_ptr<backend_type> owner;
   std::unique_ptr<context_type> context;

   explicit cached(std::uint8_t initial = 1) {
      static const auto registered = [] {
         functions::add<&consumer_host::effect>("env", "effect");
         return true;
      }();
      static_cast<void>(registered);
      auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
      fixtures::section(wasm, 1, {2, 0x60, 3, 0x7e, 0x7e, 0x7e, 0, 0x60, 0, 0});
      fixtures::section(wasm, 2, {1, 3, 'e', 'n', 'v', 6, 'e', 'f', 'f', 'e', 'c', 't', 0, 1});
      fixtures::section(wasm, 3, {2, 0, 1});
      fixtures::section(wasm, 5, {1, 0, initial});
      fixtures::section(wasm, 7, {2, 5, 'a', 'p', 'p', 'l', 'y', 0, 1, 4, 'l', 'o', 'o', 'p', 0, 2});
      fixtures::section(wasm, 10, {2, 4, 0, 0x10, 0, 0x0b, 9, 0, 0x10, 0, 0x03, 0x40, 0x0c, 0, 0x0b, 0x0b});
      auto pointer = vm::wasm_code_ptr{wasm.data(), wasm.size()};
      owner = std::make_unique<backend_type>(pointer, wasm.size(), nullptr, vm::options{}, true, false);
      context = std::make_unique<context_type>(owner->get_module(), 251);
      context->initialize_globals();
      owner->set_context(context.get());
   }

   void run(vm::wasm_allocator& memory, consumer_host& host, clock_type::time_point deadline,
            vm::interrupt_options options = {}) {
      owner->set_wasm_allocator(&memory);
      owner->timed_run(deadline, options, [&] {
         owner->initialize(&host, vm::options{});
         if (context->linear_memory() != memory.get_base_ptr<char>() || context->linear_memory()[0] != 0)
            throw std::logic_error("Cached guest storage is not fresh");
         context->linear_memory()[0] = 42;
         (*owner)(host, "env", "apply", UINT64_C(1), UINT64_C(1), UINT64_C(1));
      });
   }
};
} // namespace

BOOST_AUTO_TEST_SUITE(consumer_api_contract)

BOOST_AUTO_TEST_CASE(execution_option_snapshot_preserves_the_current_consumer_call_and_identity) {
   struct block_configuration {
      execution_configuration wasm;
   };
   const auto block = block_configuration{{2, 17}};
   const auto snapshot = vm::resolve_options(make_execution_options(block.wasm));
   static_assert(std::is_same_v<std::remove_cv_t<decltype(snapshot)>, vm::options>);
   auto expected = vm::options{};
   expected.max_pages = 2;
   expected.max_call_depth = 17;
   expected.allow_u32_limits_flags = true;
   expected.allow_invalid_empty_local_set = true;
   expected.allow_zero_blocktype = true;
   expected.check_mutable_global_bytes = false;
   expected.check_func_local_bytes = false;
   expected.check_control_depth = false;
   expected.check_legacy_nested_structures = false;
   BOOST_CHECK(snapshot == expected);
   BOOST_CHECK(vm::resolve_options(snapshot) == snapshot);
   BOOST_CHECK(vm::resolve_options(make_execution_options({3, 17})) != snapshot);
   BOOST_CHECK(vm::resolve_options(make_execution_options({2, 18})) != snapshot);
}

BOOST_AUTO_TEST_CASE(option_snapshot_preserves_checker_absence_and_explicit_checker_values) {
   const auto defaults = vm::resolve_options(vm::default_options{});
   BOOST_TEST(defaults.max_pages == std::numeric_limits<std::uint32_t>::max());
   BOOST_TEST(defaults.max_control_depth == std::numeric_limits<std::uint64_t>::max());
   BOOST_TEST(defaults.max_legacy_nested_structures == std::numeric_limits<std::uint64_t>::max());
   BOOST_TEST(!defaults.check_mutable_global_bytes);
   BOOST_TEST(!defaults.check_func_local_bytes);
   BOOST_TEST(!defaults.check_control_depth);
   BOOST_TEST(!defaults.check_legacy_nested_structures);
   BOOST_CHECK(vm::resolve_options(absent_checker_limits{}) == defaults);
   BOOST_CHECK(vm::resolve_options(defaults) == defaults);
   const auto present = vm::resolve_options(present_checker_limits{});
   BOOST_TEST(present.check_mutable_global_bytes);
   BOOST_TEST(present.check_func_local_bytes);
   BOOST_TEST(present.check_control_depth);
   BOOST_TEST(present.check_legacy_nested_structures);
   auto disabled = present;
   disabled.check_mutable_global_bytes = false;
   disabled.check_func_local_bytes = false;
   disabled.check_control_depth = false;
   disabled.check_legacy_nested_structures = false;
   BOOST_CHECK(vm::resolve_options(disabled_checker_limits{}) == disabled);
   BOOST_CHECK(vm::resolve_options(disabled) == disabled);
}

BOOST_AUTO_TEST_CASE(option_snapshot_preserves_section_inheritance_wide_limits_and_legacy_flags) {
   const auto sections = vm::resolve_options(section_options{});
   BOOST_TEST(sections.max_type_section_elements == 2U);
   BOOST_TEST(sections.max_import_section_elements == 7U);
   BOOST_TEST(sections.max_function_section_elements == 7U);
   BOOST_TEST(sections.max_global_section_elements == 7U);
   BOOST_TEST(sections.max_export_section_elements == 7U);
   BOOST_TEST(sections.max_element_section_elements == 7U);
   BOOST_TEST(sections.max_data_section_elements == 7U);
   const auto wide = vm::resolve_options(wide_options{});
   BOOST_TEST(wide.max_linear_memory_init == wide_options::max_linear_memory_init);
   BOOST_TEST(wide.max_control_depth == wide_options::max_control_depth);
   BOOST_TEST(wide.max_legacy_nested_structures == wide_options::max_legacy_nested_structures);
   BOOST_CHECK(wide.max_func_local_bytes_flags == wide_options::max_func_local_bytes_flags);
   BOOST_TEST(wide.forbid_export_mutable_globals);
   BOOST_TEST(wide.allow_code_after_function_end);
   BOOST_TEST(wide.allow_u32_limits_flags);
   BOOST_TEST(wide.allow_invalid_empty_local_set);
   BOOST_TEST(wide.allow_zero_blocktype);
   BOOST_TEST(wide.parse_custom_section_name);
   BOOST_CHECK(vm::resolve_options(wide) == wide);
}

BOOST_AUTO_TEST_CASE(option_snapshot_does_not_hide_explicit_page_profile_refusal_or_change_the_sentinel) {
   using typed_validator = vm::backend<std::nullptr_t, vm::null_backend, execution_options>;
   using snapshot_validator = vm::backend<std::nullptr_t, vm::null_backend, vm::options>;
   auto code = vm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   for (const auto pages : {65536U, UINT32_MAX - 1}) {
      const auto policy = make_execution_options({pages, 17});
      const auto snapshot = vm::resolve_options(policy);
      BOOST_TEST(snapshot.max_pages == pages);
      BOOST_CHECK_THROW(typed_validator(code, nullptr, policy), vm::exceptions::profile);
      BOOST_CHECK_THROW(snapshot_validator(code, nullptr, snapshot), vm::exceptions::profile);
   }
   for (const auto pages : {65535U, UINT32_MAX}) {
      const auto policy = make_execution_options({pages, 17});
      const auto snapshot = vm::resolve_options(policy);
      BOOST_TEST(snapshot.max_pages == pages);
      BOOST_CHECK_NO_THROW(typed_validator(code, nullptr, policy));
      BOOST_CHECK_NO_THROW(snapshot_validator(code, nullptr, snapshot));
   }
   using default_validator = vm::backend<std::nullptr_t, vm::null_backend>;
   BOOST_CHECK_NO_THROW(default_validator(code, nullptr));
}

BOOST_AUTO_TEST_CASE(mutable_global_caller_checker_is_optional_but_present_limits_default_to_enabled) {
   auto code = vm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   fixtures::section(code, 6, {1, 0x7f, 1, 0x41, 0, 0x0b});
   check_caller_admission(code, &caller_checker_options::check_mutable_global_bytes);
   using typed_validator = vm::backend<std::nullptr_t, vm::null_backend, present_checker_limits>;
   using snapshot_validator = vm::backend<std::nullptr_t, vm::null_backend, vm::options>;
   const auto policy = present_checker_limits{};
   const auto resolved = vm::resolve_options(policy);
   BOOST_CHECK_THROW(typed_validator(code, nullptr, policy), vm::exceptions::parse);
   BOOST_CHECK_THROW(snapshot_validator(code, nullptr, resolved), vm::exceptions::parse);
}

BOOST_AUTO_TEST_CASE(function_storage_caller_checker_is_optional) {
   check_caller_admission(admission_function({1, 1, 0x7f, 0x0b}), &caller_checker_options::check_func_local_bytes);
}

BOOST_AUTO_TEST_CASE(control_depth_caller_checker_is_optional) {
   check_caller_admission(admission_blocks(2), &caller_checker_options::check_control_depth);
}

BOOST_AUTO_TEST_CASE(legacy_depth_caller_checker_is_optional) {
   check_caller_admission(admission_blocks(2), &caller_checker_options::check_legacy_nested_structures);
}

BOOST_AUTO_TEST_CASE(disabling_caller_checkers_cannot_disable_mandatory_profile_limits) {
   using typed_validator = vm::backend<std::nullptr_t, vm::null_backend, caller_checker_options>;
   using snapshot_validator = vm::backend<std::nullptr_t, vm::null_backend, vm::options>;
   const auto policy = caller_checker_options{};
   const auto resolved = vm::resolve_options(policy);
   auto locals = fixtures::bytes{1};
   fixtures::size(locals, vm::capacity_profile::max_function_storage_bytes / 4 + 1);
   locals.insert(locals.end(), {0x7f, 0x0b});
   for (auto code : {admission_function(locals), admission_blocks(vm::capacity_profile::max_control_depth + 1)}) {
      BOOST_CHECK_THROW(typed_validator(code, nullptr, policy), vm::exceptions::profile);
      BOOST_CHECK_THROW(snapshot_validator(code, nullptr, resolved), vm::exceptions::profile);
   }
   auto code = vm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   auto over_cap = policy;
   over_cap.max_pages = 65536;
   const auto snapshot = vm::resolve_options(over_cap);
   BOOST_CHECK_THROW(typed_validator(code, nullptr, over_cap), vm::exceptions::profile);
   BOOST_CHECK_THROW(snapshot_validator(code, nullptr, snapshot), vm::exceptions::profile);
}

BOOST_AUTO_TEST_CASE(spine_shaped_absolute_api_gates_work_and_preserves_priority) {
   for (unsigned mask = 0; mask != 8; ++mask) {
      auto memory = vm::wasm_allocator{};
      auto entry = cached{};
      auto host = consumer_host{};
      auto cancel = std::stop_source{};
      auto external = std::stop_source{};
      if (mask & 1)
         cancel.request_stop();
      if (mask & 4)
         external.request_stop();
      const auto deadline = mask & 2 ? clock_type::now() - 1ms : clock_type::time_point::max();
      auto observed = std::uint32_t{0};
      try {
         entry.run(memory, host, deadline, {cancel.get_token(), external.get_token()});
      } catch (const vm::exceptions::canceled&) {
         observed = 1;
      } catch (const vm::exceptions::timeout&) {
         observed = 2;
      } catch (const vm::exceptions::interrupted&) {
         observed = 4;
      }
      const auto expected = mask & 1 ? 1U : mask & 2 ? 2U : mask & 4 ? 4U : 0U;
      BOOST_TEST(observed == expected);
      BOOST_TEST(host.calls == (expected ? 0U : 1U));
      BOOST_TEST(entry.context->initialized() == !expected);
   }
}

BOOST_AUTO_TEST_CASE(typed_external_stop_interrupts_actual_guest_and_allows_fresh_reuse) {
   auto memory = vm::wasm_allocator{};
   auto entry = cached{};
   auto external = std::stop_source{};
   auto host = consumer_host{};
   std::barrier entered{2};
   host.work = [&] { entered.arrive_and_wait(); };
   entry.owner->set_wasm_allocator(&memory);
   std::jthread cancel([&] {
      entered.arrive_and_wait();
      external.request_stop();
   });
   BOOST_CHECK_THROW(entry.owner->timed_run(clock_type::now() + 2s, {{}, external.get_token()},
                                            [&] {
                                               entry.owner->initialize(&host, vm::options{});
                                               entry.owner->call(host, "env", "loop");
                                            }),
                     vm::exceptions::interrupted);
   cancel.join();
   host.work = {};
   BOOST_CHECK_NO_THROW(entry.run(memory, host, clock_type::now() + 2s));
   BOOST_TEST(host.calls == 2U);
}

BOOST_AUTO_TEST_CASE(typed_host_failure_outranks_external_stop_even_when_body_catches_it) {
   auto memory = vm::wasm_allocator{};
   auto entry = cached{};
   auto external = std::stop_source{};
   auto host = consumer_host{};
   host.work = [&] {
      external.request_stop();
      throw host_error{};
   };
   entry.owner->set_wasm_allocator(&memory);
   BOOST_CHECK_THROW(entry.owner->timed_run(clock_type::now() + 2s, {{}, external.get_token()},
                                            [&] {
                                               entry.owner->initialize(&host, vm::options{});
                                               try {
                                                  entry.owner->call(host, "env", "apply", UINT64_C(1), UINT64_C(1),
                                                                    UINT64_C(1));
                                               } catch (const host_error&) {
                                               }
                                            }),
                     host_error);
   const auto* const module = &entry.owner->get_module();
   const auto* const context = entry.context.get();
   entry.context->reset();
   BOOST_TEST(!entry.context->initialized());
   BOOST_CHECK(entry.context->linear_memory() == nullptr);
   BOOST_CHECK(!entry.context->host_failure());
   auto next_host = consumer_host{};
   BOOST_CHECK_NO_THROW(entry.run(memory, next_host, clock_type::now() + 2s));
   BOOST_CHECK(&entry.owner->get_module() == module);
   BOOST_CHECK(entry.context.get() == context);
   BOOST_TEST(next_host.calls == 1U);
   BOOST_TEST(host.calls == 1U);
}

BOOST_AUTO_TEST_CASE(typed_nested_same_module_rejects_before_work_and_actual_work_errors_are_preserved) {
   auto entry = cached{};
   auto calls = 0U;
   BOOST_CHECK_NO_THROW(entry.owner->timed_run(clock_type::now() + 2s, {}, [&] {
      BOOST_CHECK_THROW(entry.owner->timed_run(clock_type::now() + 2s, {}, [&] { ++calls; }),
                        vm::exceptions::execution_unavailable);
   }));
   BOOST_TEST(calls == 0U);
   auto external = std::stop_source{};
   BOOST_CHECK_THROW(entry.owner->timed_run(clock_type::now() + 2s, {{}, external.get_token()},
                                            [&] {
                                               external.request_stop();
                                               throw std::logic_error("Actual Work failure");
                                            }),
                     std::logic_error);
}

BOOST_AUTO_TEST_CASE(cached_a_reset_b_reset_a_must_reuse_one_allocator_without_stale_instances) {
   auto memory = vm::wasm_allocator{};
   auto a = cached{1};
   auto b = cached{2};
   auto host = consumer_host{};
   BOOST_CHECK_NO_THROW(a.run(memory, host, clock_type::now() + 2s));
   memory.reset();
   BOOST_CHECK_NO_THROW(b.run(memory, host, clock_type::now() + 2s));
   memory.reset();
   BOOST_CHECK_NO_THROW(a.run(memory, host, clock_type::now() + 2s));
   memory.reset();
   BOOST_TEST(host.calls == 3U);
}

BOOST_AUTO_TEST_CASE(cached_backend_rejects_foreign_context_without_replacing_its_context) {
   auto memory = vm::wasm_allocator{};
   auto a = cached{};
   auto b = cached{};
   auto host = consumer_host{};
   BOOST_TEST(a.context->matches(a.owner->get_module(), vm::execution_mode::interpreter));
   BOOST_TEST(!a.context->matches(b.owner->get_module(), vm::execution_mode::interpreter));
   BOOST_TEST(!a.context->matches(a.owner->get_module(), vm::execution_mode::jit));
   BOOST_CHECK_THROW(a.owner->set_context(b.context.get()), vm::exceptions::invocation_state);
   BOOST_TEST(&a.owner->get_context() == a.context.get());
   BOOST_CHECK_NO_THROW(a.run(memory, host, clock_type::now() + 2s));
   a.owner->set_context(nullptr);
   BOOST_CHECK_THROW(a.owner->get_context(), vm::exceptions::interpreter);
   a.owner->set_context(a.context.get());
   BOOST_CHECK_NO_THROW(a.run(memory, host, clock_type::now() + 2s));
   BOOST_TEST(host.calls == 2U);
}

BOOST_AUTO_TEST_SUITE_END()
