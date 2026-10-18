#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "export_name_fixtures.hxx"

import forge.vm.wasm.interpret.backend;

namespace {
namespace vm = forge::vm::wasm::interpret;
namespace fixture = export_name_fixtures;
template <typename Implementation> using runtime = vm::backend<std::nullptr_t, Implementation, vm::options>;

template <typename Implementation> void check_names(const std::vector<std::string>& names) {
   auto source = fixture::module(names);
   auto instance = runtime<Implementation>{source, nullptr};
   const auto projected = instance.get_module().function_exports();
   BOOST_REQUIRE_EQUAL(projected.size(), names.size());
   for (std::size_t i = 0; i < names.size(); ++i) {
      BOOST_CHECK_EQUAL(std::count(projected.begin(), projected.end(), names[i]), 1);
      BOOST_TEST(instance.get_module().get_exported_function(names[i]) == i);
   }
   // Both directions exercise repeated lookup without allowing prefix aliases.
   for (const auto reverse : {false, true}) {
      for (std::size_t i = 0; i < names.size(); ++i) {
         const auto index = reverse ? names.size() - i - 1 : i;
         const auto value = instance.call_with_return("env", names[index]);
         BOOST_REQUIRE(value.has_value());
         BOOST_TEST(value->to_ui32() == index + 1);
      }
   }
}

template <typename Implementation> void check_missing_prefixes() {
   auto source = fixture::module({std::string("foo\0a", 5), std::string("foo\0b", 5), std::string("\0a", 2)});
   auto instance = runtime<Implementation>{source, nullptr};
   for (const auto& missing : std::vector<std::string>{"foo", "", std::string(1, '\0'), std::string("foo\0c", 5)}) {
      BOOST_TEST(instance.get_module().get_exported_function(missing) == UINT32_MAX);
      BOOST_CHECK_THROW(instance.call_with_return("env", missing), vm::exceptions::interpreter);
   }
   const auto value = instance.call_with_return("env", std::string_view{"foo\0b", 5});
   BOOST_REQUIRE(value.has_value());
   BOOST_TEST(value->to_ui32() == 2);
}

template <typename Implementation> void check_duplicates() {
   using backend = runtime<Implementation>;
   for (const auto& duplicate : std::vector<std::string>{"", std::string(1, '\0'), "foo", std::string("foo\0a", 5)}) {
      auto small = fixture::module({duplicate, duplicate});
      BOOST_CHECK_THROW(backend(small, nullptr), vm::exceptions::parse);
      auto names = fixture::many_names();
      BOOST_REQUIRE_GT(names.size(), 32);
      names.push_back(duplicate);
      auto large = fixture::module(names);
      BOOST_CHECK_THROW(backend(large, nullptr), vm::exceptions::parse);
   }
}

template <typename Implementation> void check_symbol_limit() {
   using backend = runtime<Implementation>;
   for (const auto& name :
        std::vector<std::string>{std::string("foo\0ab", 6), "abcdef", std::string("\xc3\xa9\0xy", 5)}) {
      auto source = fixture::module({name});
      auto policy = vm::options{};
      policy.max_symbol_bytes = static_cast<std::uint32_t>(name.size());
      auto exact = backend{source, nullptr, policy};
      const auto value = exact.call_with_return("env", name);
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 1);
      --policy.max_symbol_bytes;
      BOOST_CHECK_THROW(backend(source, nullptr, policy), vm::exceptions::parse);
   }
}

template <typename Implementation> void check_hook_suffixes() {
   for (const std::string_view base : {"__post_instantiate", "__wasm_call_ctors", "_initialize"}) {
      const auto name = fixture::suffixed(base);
      auto source = fixture::hook(name);
      auto instance = runtime<Implementation>{source, nullptr};
      auto value = instance.call_with_return("env", "read");
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 0);
      BOOST_TEST(instance.get_module().get_exported_function(base) == UINT32_MAX);
      BOOST_CHECK_THROW(instance.call_with_return("env", base), vm::exceptions::interpreter);
      BOOST_CHECK(!instance.call_with_return("env", name).has_value());
      value = instance.call_with_return("env", "read");
      BOOST_REQUIRE(value.has_value());
      BOOST_TEST(value->to_ui32() == 1);
   }
}

template <typename Implementation> void check_active_hook_control() {
   // Unlike the bulk-memory/WASI hooks, this hook is active in the common profile.
   auto source = fixture::hook("__post_instantiate");
   auto instance = runtime<Implementation>{source, nullptr};
   auto value = instance.call_with_return("env", "read");
   BOOST_REQUIRE(value.has_value());
   BOOST_TEST(value->to_ui32() == 1);
   BOOST_CHECK(!instance.call_with_return("env", "__post_instantiate").has_value());
   value = instance.call_with_return("env", "read");
   BOOST_REQUIRE(value.has_value());
   BOOST_TEST(value->to_ui32() == 2);
}
} // namespace

BOOST_AUTO_TEST_CASE(public_interpreter_export_names_preserve_all_bytes) {
   check_names<vm::interpreter>(fixture::names());
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_use_large_duplicate_check_path) {
   const auto names = fixture::many_names();
   BOOST_REQUIRE_GT(names.size(), 32);
   check_names<vm::interpreter>(names);
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_do_not_alias_missing_prefixes) {
   check_missing_prefixes<vm::interpreter>();
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_reject_exact_duplicates) {
   check_duplicates<vm::interpreter>();
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_count_full_symbol_bytes) {
   check_symbol_limit<vm::interpreter>();
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_do_not_initialize_nul_suffixed_hooks) {
   check_hook_suffixes<vm::interpreter>();
}
BOOST_AUTO_TEST_CASE(public_interpreter_export_names_exact_hook_control_is_active) {
   check_active_hook_control<vm::interpreter>();
}

#if FORGE_VM_WASM_INTERPRET_HAS_JIT
BOOST_AUTO_TEST_CASE(public_jit_export_names_preserve_all_bytes) {
   check_names<vm::jit>(fixture::names());
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_use_large_duplicate_check_path) {
   const auto names = fixture::many_names();
   BOOST_REQUIRE_GT(names.size(), 32);
   check_names<vm::jit>(names);
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_do_not_alias_missing_prefixes) {
   check_missing_prefixes<vm::jit>();
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_reject_exact_duplicates) {
   check_duplicates<vm::jit>();
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_count_full_symbol_bytes) {
   check_symbol_limit<vm::jit>();
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_do_not_initialize_nul_suffixed_hooks) {
   check_hook_suffixes<vm::jit>();
}
BOOST_AUTO_TEST_CASE(public_jit_export_names_exact_hook_control_is_active) {
   check_active_hook_control<vm::jit>();
}
#endif
