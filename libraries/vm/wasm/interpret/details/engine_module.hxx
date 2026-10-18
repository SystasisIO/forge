#pragma once

#include "engine_runtime.hxx"
#include "engine_bindings.hxx"
#include "engine_load_budget.hxx"

#include <cstdint>
#include <span>
#include <optional>
#include <vector>
#include <wasm_export.h>

namespace forge::vm::wasm::interpret::detail {

class engine_module {
 public:
   engine_module(std::shared_ptr<engine_runtime> runtime, std::span<const std::uint8_t> code,
                 std::shared_ptr<const engine_bindings> bindings = {}, bool no_resolve = false,
                 bool allow_zero_blocktype = false, std::shared_ptr<engine_load_budget> budget = {});
   ~engine_module();
   engine_module(const engine_module&) = delete;
   engine_module& operator=(const engine_module&) = delete;
   wasm_module_t get() const noexcept;
   bool has_bindings() const noexcept;
   std::size_t owned_buffer_bytes() const noexcept;
   std::shared_ptr<engine_load_budget> load_budget() const noexcept;
   WASMLoadBudget native_load_budget() const noexcept;
   void compile_llvm_jit(bool profiling = false);
   bool llvm_jit_ready() const noexcept;
   bool llvm_jit_profile_ready() const noexcept;
   static std::uint32_t translate_jit_pc(const void*, std::uintptr_t) noexcept;
   std::span<const std::uint32_t> jit_native_stack_bytes() const;
   struct function_view {
      std::span<const std::uint8_t> parameters;
      std::span<const std::uint8_t> results;
   };
   function_view function_type(std::uint32_t) const;
   std::span<const std::uint32_t> native_stack_bytes() const;
   void check_memory_limit(std::uint32_t max_pages) const;
   std::optional<std::uint32_t> memory_page_limit(std::uint32_t caller_pages) const;

 private:
   std::shared_ptr<engine_runtime> _runtime;
   std::shared_ptr<const engine_bindings> _bindings;
   std::shared_ptr<engine_load_budget> _budget;
   std::unique_ptr<std::uint8_t, void (*)(void*)> _bytes{nullptr, &engine_load_budget::free};
   std::size_t _byte_count = 0;
   wasm_module_t _module{};
   bool _llvm_jit_ready = false;
   bool _llvm_jit_profiling = false;
};

} // namespace forge::vm::wasm::interpret::detail
