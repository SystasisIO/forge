#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string_view>
#include <type_traits>
#include <utility>

import forge.vm.wasm.interpret.backend;
import forge.vm.wasm.interpret.floating_point_environment;

namespace {
namespace vm = forge::vm::wasm::interpret;

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
   static constexpr std::uint64_t max_linear_memory_init = std::uint64_t{1} << 32;
   static constexpr std::uint64_t max_control_depth = (std::uint64_t{1} << 32) + 1;
   static constexpr std::uint64_t max_legacy_nested_structures = (std::uint64_t{1} << 32) + 2;
};
struct absent_checker_limits {
   static constexpr bool check_mutable_global_bytes = true;
   static constexpr bool check_func_local_bytes = true;
   static constexpr bool check_control_depth = true;
   static constexpr bool check_legacy_nested_structures = true;
};
struct disabled_checker_limits {
   static constexpr std::uint64_t max_mutable_global_bytes = 0;
   static constexpr std::uint64_t max_func_local_bytes = 0;
   static constexpr std::uint64_t max_control_depth = 0;
   static constexpr std::uint64_t max_legacy_nested_structures = 0;
   static constexpr bool check_mutable_global_bytes = false;
   static constexpr bool check_func_local_bytes = false;
   static constexpr bool check_control_depth = false;
   static constexpr bool check_legacy_nested_structures = false;
};

bool option_snapshot_contract() {
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
   if (snapshot != expected || vm::resolve_options(snapshot) != snapshot ||
       vm::resolve_options(make_execution_options({3, 17})) == snapshot)
      return false;
   const auto defaults = vm::resolve_options(vm::default_options{});
   if (defaults.max_pages != UINT32_MAX || defaults.max_control_depth != UINT64_MAX ||
       defaults.max_legacy_nested_structures != UINT64_MAX || defaults.check_mutable_global_bytes ||
       defaults.check_func_local_bytes || defaults.check_control_depth || defaults.check_legacy_nested_structures ||
       vm::resolve_options(absent_checker_limits{}) != defaults)
      return false;
   const auto disabled = vm::resolve_options(disabled_checker_limits{});
   if (disabled.check_mutable_global_bytes || disabled.check_func_local_bytes || disabled.check_control_depth ||
       disabled.check_legacy_nested_structures || vm::resolve_options(disabled) != disabled)
      return false;
   const auto sections = vm::resolve_options(section_options{});
   if (sections.max_type_section_elements != 2 || sections.max_import_section_elements != 7 ||
       sections.max_function_section_elements != 7 || sections.max_global_section_elements != 7 ||
       sections.max_export_section_elements != 7 || sections.max_element_section_elements != 7 ||
       sections.max_data_section_elements != 7 ||
       sections.max_linear_memory_init != section_options::max_linear_memory_init ||
       sections.max_control_depth != section_options::max_control_depth ||
       sections.max_legacy_nested_structures != section_options::max_legacy_nested_structures ||
       vm::resolve_options(sections) != sections)
      return false;
   using typed_validator = vm::backend<std::nullptr_t, vm::null_backend, execution_options>;
   using snapshot_validator = vm::backend<std::nullptr_t, vm::null_backend, vm::options>;
   auto code = vm::wasm_code{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   const auto policy = make_execution_options({65536, 17});
   const auto over_cap = vm::resolve_options(policy);
   if (over_cap.max_pages != 65536)
      return false;
   auto typed_rejected = false;
   try {
      typed_validator denied{code, nullptr, policy};
   } catch (const vm::exceptions::profile&) {
      typed_rejected = true;
   }
   auto snapshot_rejected = false;
   try {
      snapshot_validator denied{code, nullptr, over_cap};
   } catch (const vm::exceptions::profile&) {
      snapshot_rejected = true;
   }
   if (!typed_rejected || !snapshot_rejected)
      return false;
   const auto unlimited = vm::resolve_options(make_execution_options({UINT32_MAX, 17}));
   if (unlimited.max_pages != UINT32_MAX)
      return false;
   snapshot_validator admitted{code, nullptr, unlimited};
   return true;
}

struct host {
   unsigned calls = 0;
   std::int32_t ping(std::int32_t value) {
      ++calls;
      return value + 1;
   }

   void write_log(forge::vm::wasm::interpret::span<const char> text) {
      static_cast<void>(std::string_view{text.data(), text.size()});
   }
};

struct linear_memory {
   linear_memory(const linear_memory&) = delete;
   linear_memory& operator=(const linear_memory&) = delete;

   linear_memory() = default;

   forge::vm::wasm::interpret::wasm_allocator value;
};
} // namespace

int main() {
   if (!option_snapshot_contract())
      return 10;
   {
      const auto environment = vm::floating_point_environment{};
      const auto arguments = std::array{std::bit_cast<vm::expr_value>(std::uint64_t{0x3f800000}),
                                        std::bit_cast<vm::expr_value>(std::uint64_t{0x33800000})};
      if (std::bit_cast<std::uint64_t>(vm::execute_numeric(vm::f32_add, arguments)) != 0x3f800000)
         return 11;
      auto rejected = false;
      try {
         static_cast<void>(vm::execute_numeric(vm::f32_add, {}));
      } catch (const vm::exceptions::numeric_arity&) {
         rejected = true;
      }
      if (!rejected)
         return 12;
   }
   using host_functions = forge::vm::wasm::interpret::registered_host_functions<host>;
   using engine = forge::vm::wasm::interpret::backend<host_functions, forge::vm::wasm::interpret::interpreter,
                                                      forge::vm::wasm::interpret::compatibility_options>;
   using validator = forge::vm::wasm::interpret::backend<std::nullptr_t, forge::vm::wasm::interpret::null_backend,
                                                         forge::vm::wasm::interpret::compatibility_options>;

   host_functions::add<&host::ping>("env", "ping");
   host_functions::add<&host::write_log>("env", "write_log");

   // run(i32)->i32 calls the imported env.ping and returns its result.
   forge::vm::wasm::interpret::wasm_code code{0, 0x61, 0x73, 0x6d, 1, 0, 0,    0,   1,    6, 1,   0x60, 1,   0x7f,
                                              1, 0x7f, 2,    12,   1, 3, 'e',  'n', 'v',  4, 'p', 'i',  'n', 'g',
                                              0, 0,    3,    2,    1, 0, 7,    7,   1,    3, 'r', 'u',  'n', 0,
                                              1, 10,   8,    1,    6, 0, 0x20, 0,   0x10, 0, 0x0b};
   auto parsed = validator{code, static_cast<forge::vm::wasm::interpret::wasm_allocator*>(nullptr)};

   host instance;
   linear_memory memory;
   engine backend{code, instance, &memory.value};
   auto correct = false;
   const auto invoke = [&] {
      const auto result = backend.call_with_return(instance, "env", "run", std::int32_t{41});
      correct = result && result->to_ui32() == 42;
   };
   backend.timed_run(forge::vm::wasm::interpret::watchdog{std::chrono::seconds{2}}, invoke);
   if (!correct || instance.calls != 1)
      return 1;

   auto stop = std::stop_source{};
   stop.request_stop();
   auto canceled = false;
   try {
      backend.timed_run(std::chrono::steady_clock::now() + std::chrono::seconds{2},
                        forge::vm::wasm::interpret::interrupt_options{.cancel = stop.get_token()}, invoke);
   } catch (const forge::vm::wasm::interpret::exceptions::canceled&) {
      canceled = true;
   }
   if (!canceled || instance.calls != 1)
      return 2;
   correct = false;
   backend.timed_run(forge::vm::wasm::interpret::watchdog{std::chrono::seconds{2}}, invoke);
   if (!correct || instance.calls != 2)
      return 3;

   using native_engine = forge::vm::wasm::interpret::backend<host_functions, forge::vm::wasm::interpret::jit,
                                                             forge::vm::wasm::interpret::compatibility_options>;
   using profiled_engine = forge::vm::wasm::interpret::backend<host_functions, forge::vm::wasm::interpret::jit_profile,
                                                               forge::vm::wasm::interpret::compatibility_options,
                                                               forge::vm::wasm::interpret::profile_instr_map>;
   host native_host;
   linear_memory native_memory;
#if FORGE_VM_WASM_INTERPRET_HAS_JIT
   native_engine native{code, native_host, &native_memory.value};
   const auto native_result = native.call_with_return(native_host, "env", "run", std::int32_t{41});
   if (!native_result || native_result->to_ui32() != 42 || native_host.calls != 1)
      return 4;
   auto native_canceled = false;
   try {
      native.timed_run(std::chrono::steady_clock::now() + std::chrono::seconds{2},
                       forge::vm::wasm::interpret::interrupt_options{.cancel = stop.get_token()},
                       [&] { native.call(native_host, "env", "run", std::int32_t{41}); });
   } catch (const forge::vm::wasm::interpret::exceptions::canceled&) {
      native_canceled = true;
   }
   if (!native_canceled || native_host.calls != 1)
      return 5;
   const auto recovered = native.call_with_return(native_host, "env", "run", std::int32_t{41});
   if (!recovered || recovered->to_ui32() != 42 || native_host.calls != 2)
      return 6;

   forge::vm::wasm::interpret::profile_instr_map observer;
   host profiled_host;
   {
      linear_memory profiled_memory;
      profiled_engine profiled{code, profiled_host, &profiled_memory.value};
      const auto& debug = profiled.get_debug();
      if (debug.translate(nullptr) != UINT32_MAX || debug.translate(code.data()) != UINT32_MAX ||
          debug.translate(reinterpret_cast<const void*>(UINTPTR_MAX)) != UINT32_MAX)
         return 7;
      observer = profiled.get_module().get_debug();
      const auto result = profiled.call_with_return(profiled_host, "env", "run", std::int32_t{41});
      if (!result || result->to_ui32() != 42 || profiled_host.calls != 1)
         return 8;
   }
   // A value observer outlives the backend/context; native sampling is covered
   // by the canonical public-profile tests, not a second package sampler.
   auto retained = std::move(observer);
   if (observer.translate(nullptr) != UINT32_MAX || retained.translate(nullptr) != UINT32_MAX ||
       retained.translate(code.data()) != UINT32_MAX ||
       retained.translate(reinterpret_cast<const void*>(UINTPTR_MAX)) != UINT32_MAX)
      return 9;
   retained = forge::vm::wasm::interpret::profile_instr_map{};
#else
   auto unavailable = false;
   try {
      native_engine native{code, native_host, &native_memory.value};
   } catch (const forge::vm::wasm::interpret::exceptions::execution_unavailable&) {
      unavailable = true;
   }
   if (!unavailable || native_host.calls)
      return 4;
   auto profile_unavailable = false;
   try {
      profiled_engine profiled{code, native_host, &native_memory.value};
   } catch (const forge::vm::wasm::interpret::exceptions::execution_unavailable&) {
      profile_unavailable = true;
   }
   if (!profile_unavailable || native_host.calls)
      return 5;
#endif

   static_assert(!forge::vm::wasm::interpret::interpreter::is_jit);
   (void)parsed.get_module();
   (void)backend.get_module();
   return 0;
}
