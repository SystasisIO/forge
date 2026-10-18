#include "details/engine_compiler.hxx"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <wasm_export.h>
#include <aot_export.h>
#include "../interpret/details/engine_limits.hxx"

import forge.vm.wasm.interpret.capacity_profile;

namespace forge::vm::wasm::compiler::detail {

std::vector<std::uint8_t> engine_compiler::compile(std::span<const std::uint8_t> wasm, std::uint32_t max_input_bytes,
                                                   std::uint32_t max_output_bytes) const {
   return compile(wasm, max_input_bytes, max_output_bytes, {});
}

std::vector<std::uint8_t> engine_compiler::compile(std::span<const std::uint8_t> wasm, std::uint32_t max_input_bytes,
                                                   std::uint32_t max_output_bytes,
                                                   const std::function<void(WASMModuleCommon*)>& admit,
                                                   bool allow_zero_blocktype) const {
   constexpr std::array<std::uint8_t, 8> header{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   if (!max_input_bytes || !max_output_bytes || wasm.size() > max_input_bytes ||
       wasm.size() > interpret::capacity_profile::max_module_bytes || wasm.size() < header.size() ||
       !std::equal(header.begin(), header.end(), wasm.begin())) {
      throw std::invalid_argument("Invalid or oversized WASM compiler input");
   }
   // WAMR compilation uses process-global runtime and diagnostic state.
   static std::mutex mutex;
   const std::lock_guard lock{mutex};
   RuntimeInitArgs init{};
   init.mem_alloc_type = Alloc_With_System_Allocator;
   if (!wasm_runtime_full_init(&init)) {
      throw std::runtime_error("WASM compiler initialization failed");
   }
   const auto destroy = [](void*) { wasm_runtime_destroy(); };
   const std::unique_ptr<void, decltype(destroy)> runtime(&init, destroy);
   std::vector<std::uint8_t> bytes(wasm.begin(), wasm.end());
   std::array<char, 256> diagnostic{};
   LoadArgs args{};
   args.block_type_mode = allow_zero_blocktype ? WASM_BLOCK_TYPES_NUMERIC_LEGACY_ZERO : WASM_BLOCK_TYPES_NUMERIC;
   const std::unique_ptr<WASMModuleCommon, decltype(&wasm_runtime_unload)> module(
       wasm_runtime_load_ex(bytes.data(), static_cast<std::uint32_t>(bytes.size()), &args, diagnostic.data(),
                            diagnostic.size()),
       wasm_runtime_unload);
   if (!module) {
      throw std::runtime_error("WASM compiler rejected the module");
   }
   const auto limits = interpret::detail::engine_limits{module.get()};
   limits.check_profile<interpret::capacity_profile>();
   limits.validate_instantiation();
   if (admit)
      admit(module.get());
   AOTCompOption options{};
#if defined(__aarch64__)
   char arch[] = "aarch64";
#elif defined(__x86_64__)
   char arch[] = "x86_64";
#else
#error Unqualified WAMR compiler architecture
#endif
   char cpu[] = "generic";
   options.target_arch = arch;
   options.target_cpu = cpu;
   options.opt_level = 3;
   options.size_level = 3;
   options.output_format = AOT_FORMAT_FILE;
   options.bounds_checks = 1;
   options.stack_bounds_checks = 1;
   options.enable_aux_stack_check = true;
   options.enable_thread_mgr = true;
   aot_call_stack_features_init_default(&options.call_stack_features);
   const std::unique_ptr<AOTCompData, decltype(&aot_destroy_comp_data)> data(
       aot_create_comp_data(module.get(), options.target_arch, false), aot_destroy_comp_data);
   if (!data) {
      throw std::runtime_error("WASM compiler could not prepare input");
   }
   const std::unique_ptr<AOTCompContext, decltype(&aot_destroy_comp_context)> context(
       aot_create_comp_context(data.get(), &options), aot_destroy_comp_context);
   if (!context || !aot_compile_wasm(context.get())) {
      throw std::runtime_error("WASM native compilation failed");
   }
   const std::unique_ptr<AOTObjectData, decltype(&aot_obj_data_destroy)> object(aot_obj_data_create(context.get()),
                                                                                aot_obj_data_destroy);
   if (!object) {
      throw std::runtime_error("WASM compiler could not prepare output");
   }
   const auto size = aot_get_aot_file_size(context.get(), data.get(), object.get());
   if (!size || size > max_output_bytes) {
      throw std::length_error("WASM compiler output exceeds the byte limit");
   }
   std::vector<std::uint8_t> result(size);
   if (!aot_emit_aot_file_buf_ex(context.get(), data.get(), object.get(), result.data(), size)) {
      throw std::runtime_error("WASM compiler could not emit output");
   }
   return result;
}

} // namespace forge::vm::wasm::compiler::detail
