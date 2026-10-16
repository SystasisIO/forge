#include "test_prelude.hpp"

#include <cstdint>
#include <limits>
#include <type_traits>

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.types;

namespace vm = forge::vm::wasm::interpret;

namespace {
template <typename Options> using validator = vm::backend<std::nullptr_t, vm::null_backend, Options>;

struct inherited_sections {
   static constexpr std::uint32_t max_section_elements = 1;
   static constexpr std::uint32_t max_type_section_elements = 2;
};
struct wide_initialization {
   static constexpr std::uint64_t max_linear_memory_init = std::uint64_t{1} << 32;
};
struct dynamic_execution {
   std::uint32_t max_pages = 2;
   std::uint32_t max_call_depth = 17;
   static constexpr bool allow_u32_limits_flags = true;
   static constexpr bool allow_invalid_empty_local_set = true;
   static constexpr bool allow_zero_blocktype = true;
};
struct zero_globals {
   static constexpr std::uint32_t max_mutable_global_bytes = 0;
};
struct local_bytes_only {
   static constexpr std::uint32_t max_func_local_bytes = 4;
   static constexpr auto max_func_local_bytes_flags = vm::max_func_local_bytes_flags_t::locals;
};
struct locals_and_stack {
   static constexpr std::uint32_t max_func_local_bytes = 4;
};
struct shallow_control {
   static constexpr std::uint32_t max_control_depth = 1;
};
struct legacy_depth {
   static constexpr std::uint32_t max_legacy_nested_structures = 1;
};
struct wide_depth {
   static constexpr std::uint64_t max_control_depth = (std::uint64_t{1} << 32) + 1;
   static constexpr std::uint64_t max_legacy_nested_structures = (std::uint64_t{1} << 32) + 1;
};

vm::wasm_code nested_blocks() {
   return {0, 97, 115, 109, 1,  0, 0, 0, 1, 4,    1, 0x60, 0,    0,    3,
           2, 1,  0,   10,  10, 1, 8, 0, 2, 0x40, 2, 0x40, 0x0b, 0x0b, 0x0b};
}
template <typename Options> void parse_typed(vm::wasm_code& code, vm::module& mod, const Options& options) {
   mod.allocator.use_default_memory();
   vm::null_debug_info debug;
   vm::interpreter::parser<std::nullptr_t, Options, vm::null_debug_info>{mod.allocator, options}.parse_module(code, mod,
                                                                                                              debug);
   mod.finalize();
}
} // namespace

BOOST_AUTO_TEST_SUITE(options_preparation)

BOOST_AUTO_TEST_CASE(default_snapshot_preserves_absent_checkers) {
   const auto snapshot = vm::resolve_options(vm::default_options{});
   BOOST_TEST(snapshot.max_linear_memory_init == std::numeric_limits<std::uint64_t>::max());
   BOOST_TEST(snapshot.max_call_depth == 251U);
   BOOST_TEST(!snapshot.check_mutable_global_bytes);
   BOOST_TEST(!snapshot.check_func_local_bytes);
   BOOST_TEST(!snapshot.check_control_depth);
   BOOST_TEST(!snapshot.check_legacy_nested_structures);
   BOOST_CHECK(vm::resolve_options(snapshot) == snapshot);
   auto ignored = snapshot;
   ignored.max_control_depth = 0;
   ignored.max_legacy_nested_structures = 0;
   auto code = nested_blocks();
   BOOST_CHECK_NO_THROW(validator<vm::default_options>(code, nullptr));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, ignored));
}

BOOST_AUTO_TEST_CASE(section_fallback_inherits_source_limit) {
   const auto snapshot = vm::resolve_options(inherited_sections{});
   BOOST_TEST(snapshot.max_type_section_elements == 2U);
   BOOST_TEST(snapshot.max_function_section_elements == 1U);
   BOOST_TEST(snapshot.max_import_section_elements == 1U);
   BOOST_TEST(snapshot.max_global_section_elements == 1U);
   BOOST_TEST(snapshot.max_export_section_elements == 1U);
   BOOST_TEST(snapshot.max_element_section_elements == 1U);
   BOOST_TEST(snapshot.max_data_section_elements == 1U);
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 2, 0x60, 0, 0, 0x60, 0, 0};
   BOOST_CHECK_NO_THROW(validator<inherited_sections>(code, nullptr, inherited_sections{}));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, snapshot));
   auto stricter = snapshot;
   stricter.max_type_section_elements = 1;
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, stricter), vm::exceptions::parse);
}

BOOST_AUTO_TEST_CASE(initialization_limit_retains_uint64_and_exact_four_gib_end) {
   static_assert(std::is_same_v<decltype(vm::options{}.max_linear_memory_init), std::uint64_t>);
   const auto snapshot = vm::resolve_options(wide_initialization{});
   BOOST_TEST(snapshot.max_linear_memory_init == (std::uint64_t{1} << 32));
   // Metadata-only validation: 65536 pages and a final byte at uint32 max.
   // No 4-GiB memory allocation or execution is involved.
   auto code =
       vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 5, 5, 1, 0, 0x80, 0x80, 4, 11, 7, 1, 0, 0x41, 0x7f, 0x0b, 1, 0x2a};
   BOOST_CHECK_NO_THROW(validator<wide_initialization>(code, nullptr, wide_initialization{}));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, snapshot));
   auto narrow = snapshot;
   narrow.max_linear_memory_init = std::numeric_limits<std::uint32_t>::max();
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, narrow), vm::exceptions::parse);
}

BOOST_AUTO_TEST_CASE(explicit_mutable_global_check_is_preserved) {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 6, 6, 1, 0x7f, 1, 0x41, 0, 0x0b};
   const auto present = vm::resolve_options(zero_globals{});
   BOOST_TEST(present.check_mutable_global_bytes);
   BOOST_CHECK_THROW(validator<zero_globals>(code, nullptr, zero_globals{}), vm::exceptions::parse);
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, present), vm::exceptions::parse);
   auto absent = vm::resolve_options(vm::default_options{});
   absent.max_mutable_global_bytes = 0;
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, absent));
}

BOOST_AUTO_TEST_CASE(local_and_stack_flags_match_static_traits) {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 1, 4,    1,    0x60, 0,    0,   3,
                             2, 1,  0,   10,  9, 1, 7, 1, 1, 0x7f, 0x41, 0,    0x1a, 0x0b};
   BOOST_CHECK_NO_THROW(validator<local_bytes_only>(code, nullptr, local_bytes_only{}));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, vm::resolve_options(local_bytes_only{})));
   BOOST_CHECK_THROW(validator<locals_and_stack>(code, nullptr, locals_and_stack{}), vm::exceptions::parse);
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, vm::resolve_options(locals_and_stack{})),
                     vm::exceptions::parse);
   auto absent = vm::resolve_options(vm::default_options{});
   absent.max_func_local_bytes = 0;
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, absent));
}

BOOST_AUTO_TEST_CASE(control_and_legacy_depth_checks_are_independent) {
   auto code = nested_blocks();
   const auto normal = vm::resolve_options(shallow_control{});
   BOOST_TEST(normal.check_control_depth);
   BOOST_TEST(!normal.check_legacy_nested_structures);
   BOOST_CHECK_THROW(validator<shallow_control>(code, nullptr, shallow_control{}), vm::exceptions::parse);
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, normal), vm::exceptions::parse);
   const auto legacy = vm::resolve_options(legacy_depth{});
   BOOST_TEST(!legacy.check_control_depth);
   BOOST_TEST(legacy.check_legacy_nested_structures);
   BOOST_CHECK_THROW(validator<legacy_depth>(code, nullptr, legacy_depth{}), vm::exceptions::parse);
   BOOST_CHECK_THROW(validator<vm::options>(code, nullptr, legacy), vm::exceptions::parse);
}

BOOST_AUTO_TEST_CASE(execution_snapshot_does_not_add_admission_caps) {
   const auto snapshot = vm::resolve_options(dynamic_execution{});
   BOOST_TEST(snapshot.max_pages == 2U);
   BOOST_TEST(snapshot.max_call_depth == 17U);
   BOOST_TEST(snapshot.max_code_bytes == std::numeric_limits<std::uint32_t>::max());
   BOOST_TEST(snapshot.allow_zero_blocktype);
   BOOST_TEST(snapshot.allow_u32_limits_flags);
   BOOST_TEST(snapshot.allow_invalid_empty_local_set);
   BOOST_TEST(!snapshot.check_func_local_bytes);
   auto code =
       vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 10, 7, 1, 5, 0, 2, 0, 0x0b, 0x0b};
   BOOST_CHECK_THROW(validator<vm::default_options>(code, nullptr), vm::exceptions::parse);
   BOOST_CHECK_NO_THROW(validator<dynamic_execution>(code, nullptr, dynamic_execution{}));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, snapshot));
   using original_runtime = vm::backend<std::nullptr_t, vm::interpreter, dynamic_execution>;
   using snapshot_runtime = vm::backend<std::nullptr_t, vm::interpreter, vm::options>;
   auto original = original_runtime{code, nullptr, dynamic_execution{}};
   auto resolved = snapshot_runtime{code, nullptr, snapshot};
   BOOST_CHECK_NO_THROW(original.call(static_cast<std::nullptr_t*>(nullptr), 0U));
   BOOST_CHECK_NO_THROW(resolved.call(static_cast<std::nullptr_t*>(nullptr), 0U));
}

BOOST_AUTO_TEST_CASE(wide_depth_limits_are_not_truncated_to_a_new_cap) {
   auto code = nested_blocks();
   const auto snapshot = vm::resolve_options(wide_depth{});
   BOOST_TEST(snapshot.max_control_depth == wide_depth::max_control_depth);
   BOOST_TEST(snapshot.max_legacy_nested_structures == wide_depth::max_legacy_nested_structures);
   BOOST_CHECK_NO_THROW(validator<wide_depth>(code, nullptr, wide_depth{}));
   BOOST_CHECK_NO_THROW(validator<vm::options>(code, nullptr, snapshot));
}

BOOST_AUTO_TEST_CASE(full_preparation_rejects_deferred_data_bounds) {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 5, 3, 1, 0, 1, 11, 7, 1, 0, 0x41, 0x7f, 0x0b, 1, 0x2a};
   vm::module parsed;
   BOOST_CHECK_NO_THROW(parse_typed(code, parsed, vm::default_options{}));
   BOOST_CHECK_THROW(parsed.validate_instantiation(), vm::exceptions::memory);
   BOOST_CHECK_THROW(validator<vm::default_options>(code, nullptr), vm::exceptions::memory);
}

BOOST_AUTO_TEST_CASE(full_preparation_rejects_deferred_element_bounds) {
   auto code = vm::wasm_code{0, 97,   115, 109, 1, 0, 0, 0, 1,    4, 1,    0x60, 0, 0,  3, 2, 1, 0, 4,   4,
                             1, 0x70, 0,   0,   9, 7, 1, 0, 0x41, 0, 0x0b, 1,    0, 10, 4, 1, 2, 0, 0x0b};
   vm::module parsed;
   BOOST_CHECK_NO_THROW(parse_typed(code, parsed, vm::default_options{}));
   BOOST_REQUIRE(parsed.error != nullptr);
   BOOST_CHECK_THROW(parsed.validate_instantiation(), vm::exceptions::interpreter);
   BOOST_CHECK_THROW(validator<vm::default_options>(code, nullptr), vm::exceptions::interpreter);
}

BOOST_AUTO_TEST_CASE(preparation_does_not_execute_start) {
   auto code =
       vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 1, 4, 1, 0x60, 0, 0, 3, 2, 1, 0, 8, 1, 0, 10, 5, 1, 3, 0, 0, 0x0b};
   vm::module parsed;
   parse_typed(code, parsed, vm::default_options{});
   BOOST_CHECK_NO_THROW(parsed.validate_instantiation());
   BOOST_CHECK_NO_THROW(validator<vm::default_options>(code, nullptr));
   using runtime = vm::backend<>;
   BOOST_CHECK_THROW(runtime(code, nullptr), vm::exceptions::interpreter);
}

BOOST_AUTO_TEST_CASE(preparation_uses_retained_jit_data_after_parser_arena_release) {
   auto code = vm::wasm_code{0, 97, 115, 109, 1, 0, 0, 0, 5, 3, 1, 0, 1, 11, 7, 1, 0, 0x41, 0, 0x0b, 1, 0x2a};
   vm::module parsed;
   parse_typed(code, parsed, vm::default_options{});
   parsed.make_jit_module();
   parsed.allocator.release_base_memory();
   BOOST_CHECK_NO_THROW(parsed.validate_instantiation());
   parsed.jit_mod->data[0].offset.value.i32 = -1;
   BOOST_CHECK_THROW(parsed.validate_instantiation(), vm::exceptions::memory);
}

BOOST_AUTO_TEST_SUITE_END()
