#include "details/engine_module.hxx"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <wasm_runtime.h>
#if WASM_ENABLE_JIT != 0
#include <aot_llvm.h>
#include <wasm_loader.h>
#if defined(WASM_LOADER_HAS_LLVM_JIT_INSTRUCTION_PROFILE)
#include <aot_jit_profile.h>
#endif
#endif

namespace forge::vm::wasm::interpret::detail {
namespace {

#if WASM_ENABLE_JIT != 0
std::span<const std::uint32_t> stack_sizes(wasm_module_t source) {
   if (!source || source->module_type != Wasm_Module_Bytecode)
      throw std::invalid_argument("LLVM JIT requires admitted bytecode");
   const auto& module = *reinterpret_cast<const WASMModule*>(source);
   if (!module.function_count)
      return {};
   if (!module.comp_ctx || module.comp_ctx->func_ctx_count != module.function_count ||
       !module.comp_ctx->jit_stack_sizes)
      throw std::invalid_argument("LLVM JIT native stack metadata is absent");
   const std::span<const std::uint32_t> result{module.comp_ctx->jit_stack_sizes, module.function_count};
   if (std::find(result.begin(), result.end(), std::numeric_limits<std::uint32_t>::max()) != result.end())
      throw std::invalid_argument("LLVM JIT native stack metadata is incomplete");
   return result;
}
#endif

} // namespace

void engine_module::compile_llvm_jit(bool profiling) {
   if (_llvm_jit_ready) {
      if (_llvm_jit_profiling != profiling)
         throw std::invalid_argument("LLVM JIT profiling mode differs from prepared module identity");
      return;
   }
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT == 0 && WASM_ENABLE_LAZY_JIT == 0 &&                                  \
    defined(WASM_LOADER_HAS_BOUNDED_LLVM_JIT)
   if (!_module || _module->module_type != Wasm_Module_Bytecode)
      throw std::invalid_argument("LLVM JIT requires admitted bytecode");
   if (!_budget)
      throw std::invalid_argument("LLVM JIT requires a bounded preparation owner");
   // The same owner accounts WAMR storage and native mappings. LLVM compiler
   // heap is outside the preparation contract, not a second global quota.
   const engine_load_budget::scope allocation_scope{_budget};
   if (_budget)
      _budget->check();
   std::array<char, 512> diagnostic{};
   const auto hooks = native_load_budget();
   const auto native_limit = std::min<std::uint64_t>(_budget->limit_bytes(), std::uint64_t{1} << 30);
   const auto compile = [&] {
      if (!profiling)
         return wasm_loader_compile_llvm_jit_ex(reinterpret_cast<WASMModule*>(_module), &hooks, native_limit,
                                                diagnostic.data(), diagnostic.size());
#if defined(WASM_LOADER_HAS_LLVM_JIT_INSTRUCTION_PROFILE)
      return wasm_loader_compile_llvm_jit_profile_ex(reinterpret_cast<WASMModule*>(_module), &hooks, native_limit,
                                                     diagnostic.data(), diagnostic.size());
#else
      throw std::runtime_error("LLVM JIT instruction profiling is unavailable in this runtime profile");
#endif
   };
   if (!compile()) {
      if (_budget)
         _budget->check();
      throw std::runtime_error(std::string("LLVM JIT preparation failed: ") + diagnostic.data());
   }
   stack_sizes(_module);
   if (_budget)
      _budget->check();
   // Successful profile compilation already requires validated metadata to be
   // linked and published (zero-defined modules need no map). Readiness is not
   // interval cardinality: a complete empty map translates every PC as UNKNOWN.
   _llvm_jit_profiling = profiling;
   _llvm_jit_ready = true;
#else
   throw std::runtime_error("LLVM JIT is unavailable in this runtime profile");
#endif
}

bool engine_module::llvm_jit_ready() const noexcept {
   return _llvm_jit_ready;
}

bool engine_module::llvm_jit_profile_ready() const noexcept {
   return _llvm_jit_ready && _llvm_jit_profiling;
}

std::uint32_t engine_module::translate_jit_pc(const void* owner, std::uintptr_t pc) noexcept {
#if WASM_ENABLE_JIT != 0 && defined(WASM_LOADER_HAS_LLVM_JIT_INSTRUCTION_PROFILE)
   const auto& self = *static_cast<const engine_module*>(owner);
   return aot_jit_profile_translate(reinterpret_cast<const WASMModule*>(self._module)->comp_ctx, pc);
#else
   return UINT32_MAX;
#endif
}

std::span<const std::uint32_t> engine_module::jit_native_stack_bytes() const {
   if (!_llvm_jit_ready)
      throw std::invalid_argument("LLVM JIT module has not completed preparation");
#if WASM_ENABLE_JIT != 0
   return stack_sizes(_module);
#else
   throw std::invalid_argument("LLVM JIT is unavailable in this runtime profile");
#endif
}

} // namespace forge::vm::wasm::interpret::detail
