#include "details/engine_module.hxx"

#include <array>
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <aot_runtime.h>

namespace forge::vm::wasm::interpret::detail {

engine_module::engine_module(std::shared_ptr<engine_runtime> runtime, std::span<const std::uint8_t> code,
                             std::shared_ptr<const engine_bindings> bindings, bool no_resolve,
                             bool allow_zero_blocktype, std::shared_ptr<engine_load_budget> budget)
    : _runtime(std::move(runtime)), _bindings(std::move(bindings)),
      _budget(budget ? std::move(budget) : engine_load_budget::current()) {
   if (!_runtime || code.empty() || code.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::invalid_argument("Invalid WASM module input");
   }
   // The upstream loader may retain pointers into, and modify, this owned buffer.
   const engine_load_budget::scope allocation_scope{_budget};
   _bytes.reset(static_cast<std::uint8_t*>(engine_load_budget::allocate(code.size())));
   if (!_bytes) {
      if (_budget)
         _budget->check();
      throw std::bad_alloc{};
   }
   _byte_count = code.size();
   std::memcpy(_bytes.get(), code.data(), code.size());
   std::array<char, 512> diagnostic{};
   LoadArgs args{};
   args.no_resolve = no_resolve || static_cast<bool>(_bindings);
   args.block_type_mode = allow_zero_blocktype ? WASM_BLOCK_TYPES_NUMERIC_LEGACY_ZERO : WASM_BLOCK_TYPES_NUMERIC;
   args.defer_jit = true;
   args.load_budget = native_load_budget();
   _module = wasm_runtime_load_ex(_bytes.get(), static_cast<std::uint32_t>(_byte_count), &args, diagnostic.data(),
                                  diagnostic.size());
   if (!_module) {
      if (_budget) {
         if (!_budget->retry_cleanup())
            _runtime->retain_cleanup(_budget);
         _budget->check();
      }
      throw std::runtime_error(std::string("WASM load failed: ") + diagnostic.data());
   }
   try {
      if (_budget)
         _budget->check();
      if (_bindings)
         _bindings->bind(_module);
   } catch (...) {
      wasm_runtime_unload(_module);
      if (_budget && !_budget->retry_cleanup())
         _runtime->retain_cleanup(_budget);
      throw;
   }
}

engine_module::~engine_module() {
   const engine_load_budget::scope allocation_scope{_budget};
   wasm_runtime_unload(_module);
   if (_budget && !_budget->retry_cleanup())
      _runtime->retain_cleanup(_budget);
}

wasm_module_t engine_module::get() const noexcept {
   return _module;
}

bool engine_module::has_bindings() const noexcept {
   return static_cast<bool>(_bindings);
}
std::size_t engine_module::owned_buffer_bytes() const noexcept {
   return _byte_count;
}
std::shared_ptr<engine_load_budget> engine_module::load_budget() const noexcept {
   return _budget;
}

WASMLoadBudget engine_module::native_load_budget() const noexcept {
   auto hooks = WASMLoadBudget{};
   if (!_budget)
      return hooks;
   using owner_type = const std::shared_ptr<engine_load_budget>;
   hooks.context = const_cast<std::shared_ptr<engine_load_budget>*>(&_budget);
   hooks.begin_mapping = [](void* owner, std::uint64_t retained, std::uint64_t peak) {
      return engine_load_budget::begin_mapping(*static_cast<owner_type*>(owner), retained, peak, &os_munmap_checked);
   };
   hooks.complete_mapping = [](void* owner, void* token, void* address) {
      (*static_cast<owner_type*>(owner))->complete_mapping(token, address);
   };
   hooks.unmap = [](void* owner, void* address, std::uint64_t retained) {
      return (*static_cast<owner_type*>(owner))->unmap(address, retained);
   };
   hooks.check = [](void* owner) { return (*static_cast<owner_type*>(owner))->active(); };
   hooks.before_protect = [](void* owner) { return (*static_cast<owner_type*>(owner))->allow_protection(); };
   hooks.allocation_failed = [](void* owner) {
      (*static_cast<owner_type*>(owner))->fail(engine_load_budget::failure::allocation);
   };
   hooks.protection_failed = [](void* owner) {
      (*static_cast<owner_type*>(owner))->fail(engine_load_budget::failure::protection);
   };
   return hooks;
}

engine_module::function_view engine_module::function_type(std::uint32_t index) const {
   const std::uint8_t* types = nullptr;
   std::uint32_t parameters = 0, results = 0;
   if (_module->module_type == Wasm_Module_Bytecode) {
      const auto& module = *reinterpret_cast<const WASMModule*>(_module);
      const WASMFuncType* type = nullptr;
      if (index < module.import_function_count)
         type = module.import_functions[index].u.function.func_type;
      else if (index - module.import_function_count < module.function_count)
         type = module.functions[index - module.import_function_count]->func_type;
      if (type) {
         types = type->types;
         parameters = type->param_count;
         results = type->result_count;
      }
   } else if (_module->module_type == Wasm_Module_AoT) {
      const auto& module = *reinterpret_cast<const AOTModule*>(_module);
      const AOTFuncType* type = nullptr;
      if (index < module.import_func_count)
         type = module.import_funcs[index].func_type;
      else if (index - module.import_func_count < module.func_count)
         type = reinterpret_cast<const AOTFuncType*>(
             module.types[module.func_type_indexes[index - module.import_func_count]]);
      if (type) {
         types = type->types;
         parameters = type->param_count;
         results = type->result_count;
      }
   }
   if (!types)
      throw std::out_of_range("Invalid WASM function index");
   return {{types, parameters}, {types + parameters, results}};
}

std::span<const std::uint32_t> engine_module::native_stack_bytes() const {
   if (_module->module_type == Wasm_Module_Bytecode && llvm_jit_ready())
      return jit_native_stack_bytes();
   if (_module->module_type != Wasm_Module_AoT)
      return {};
   auto* module = reinterpret_cast<AOTModule*>(_module);
   std::uint32_t size = 0;
   const auto* data = aot_get_data_section_addr(module, ".aot_stack_sizes", &size);
   if (!data || size != static_cast<std::uint64_t>(module->func_count) * sizeof(std::uint32_t) ||
       reinterpret_cast<std::uintptr_t>(data) % alignof(std::uint32_t))
      throw std::invalid_argument("AOT native stack metadata is invalid");
   return {reinterpret_cast<const std::uint32_t*>(data), module->func_count};
}

void engine_module::check_memory_limit(std::uint32_t max_pages) const {
   // WAMR's max_memory_pages override silently raises a too-small cap to the
   // module minimum. Reject before instantiation, never after allocating it.
   if (_module->module_type == Wasm_Module_Bytecode) {
      const auto* module = reinterpret_cast<const WASMModule*>(_module);
      for (std::uint32_t i = 0; i < module->memory_count; ++i)
         if (static_cast<std::uint64_t>(module->memories[i].init_page_count) * module->memories[i].num_bytes_per_page >
             static_cast<std::uint64_t>(max_pages) * 65536)
            throw std::length_error("WASM initial memory exceeds the invocation page limit");
      for (std::uint32_t i = 0; i < module->import_memory_count; ++i)
         if (static_cast<std::uint64_t>(module->import_memories[i].u.memory.mem_type.init_page_count) *
                 module->import_memories[i].u.memory.mem_type.num_bytes_per_page >
             static_cast<std::uint64_t>(max_pages) * 65536)
            throw std::length_error("WASM imported memory exceeds the invocation page limit");
   } else if (_module->module_type == Wasm_Module_AoT) {
      const auto* module = reinterpret_cast<const AOTModule*>(_module);
      for (std::uint32_t i = 0; i < module->memory_count; ++i)
         if (static_cast<std::uint64_t>(module->memories[i].init_page_count) * module->memories[i].num_bytes_per_page >
             static_cast<std::uint64_t>(max_pages) * 65536)
            throw std::length_error("WASM initial memory exceeds the invocation page limit");
   } else
      throw std::invalid_argument("Unsupported WASM module kind");
}

std::optional<std::uint32_t> engine_module::memory_page_limit(std::uint32_t caller_pages) const {
   const auto limit = [caller_pages](const auto& memory) {
      const auto bytes = static_cast<std::uint64_t>(memory.max_page_count) * memory.num_bytes_per_page;
      return static_cast<std::uint32_t>(std::min<std::uint64_t>(caller_pages, (bytes + 65535) / 65536));
   };
   if (_module->module_type == Wasm_Module_Bytecode) {
      const auto& module = *reinterpret_cast<const WASMModule*>(_module);
      if (module.memory_count)
         return limit(module.memories[0]);
   } else if (_module->module_type == Wasm_Module_AoT) {
      const auto& module = *reinterpret_cast<const AOTModule*>(_module);
      if (module.memory_count)
         return limit(module.memories[0]);
   }
   return {};
}

} // namespace forge::vm::wasm::interpret::detail
