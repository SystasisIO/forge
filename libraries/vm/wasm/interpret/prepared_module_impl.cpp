module;

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>
#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"
#include <wasm_runtime.h>
#include <aot_runtime.h>

module forge.vm.wasm.interpret.prepared_module;

#include "details/prepared_module_impl.hxx"

namespace forge::vm::wasm::interpret {
namespace {

wasm_valkind_t kind(value_type type) {
   switch (type) {
   case types::i32:
      return WASM_I32;
   case types::i64:
      return WASM_I64;
   case types::f32:
      return WASM_F32;
   case types::f64:
      return WASM_F64;
   default:
      throw exceptions::link{"Host signature contains an unsupported value kind"};
   }
}

detail::engine_bindings::function binding(const host_binding& source, std::pmr::memory_resource* resource) {
   detail::engine_bindings::function result{resource};
   result.module = source.module;
   result.name = source.name;
   result.parameters.reserve(source.signature.params.size());
   for (auto parameter : source.signature.params)
      result.parameters.push_back(kind(parameter));
   if (source.signature.ret.size() > 1)
      throw exceptions::link{"Multiple host results are unsupported"};
   if (!source.signature.ret.empty())
      result.result = kind(source.signature.ret[0]);
   return result;
}

} // namespace

prepared_module::impl::resource::resource(const std::optional<preparation>& value) : _preparation(value) {}
void* prepared_module::impl::resource::do_allocate(std::size_t bytes, std::size_t alignment) {
   return _preparation ? _preparation->allocate(bytes, alignment)
                       : std::pmr::new_delete_resource()->allocate(bytes, alignment);
}
void prepared_module::impl::resource::do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) {
   if (_preparation)
      preparation::deallocate(pointer);
   else
      std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
}
bool prepared_module::impl::resource::do_is_equal(const std::pmr::memory_resource& other) const noexcept {
   return this == &other;
}
prepared_module::impl::~impl() {
   if (allocation)
      allocation->release(projection_bytes);
}

void prepared_module::impl::invoke(const void* context, std::size_t index, detail::engine_bindings::call& call) {
   const auto& self = *static_cast<const impl*>(context);
   auto operands = operand_stack{};
   for (std::size_t i = 0; i < call.parameter_count(); ++i) {
      switch (call.parameter_kind(i)) {
      case WASM_I32:
         operands.push(i32_const_t{call.argument<std::uint32_t>(i)});
         break;
      case WASM_I64:
         operands.push(i64_const_t{call.argument<std::uint64_t>(i)});
         break;
      case WASM_F32:
         operands.push(f32_const_t{call.argument<float>(i)});
         break;
      case WASM_F64:
         operands.push(f64_const_t{call.argument<double>(i)});
         break;
      default:
         throw exceptions::link{"Host signature contains an unsupported value kind"};
      }
   }
   const auto memory = call.memory();
   self.callbacks.at(index)(call.host(), execution_interface{memory.data(), memory.size(), &operands});
   if (const auto result = call.result_kind()) {
      if (operands.size() != 1)
         throw exceptions::interpreter{"Host result stack is invalid"};
      const auto value = operands.pop();
      switch (*result) {
      case WASM_I32:
         call.result(value.to_ui32());
         break;
      case WASM_I64:
         call.result(value.to_ui64());
         break;
      case WASM_F32:
         call.result(value.to_f32());
         break;
      case WASM_F64:
         call.result(value.to_f64());
         break;
      default:
         throw exceptions::link{"Host signature contains an unsupported value kind"};
      }
   }
}

prepared_module::impl::impl(std::span<const std::uint8_t> code, const options& policy,
                            std::span<host_binding> functions, purpose use, std::optional<preparation> budget)
    : allocation(std::move(budget)), storage(allocation), callbacks(&storage), bytes(code.size()), use(use) {
   if (code.size() > capacity_profile::max_module_bytes)
      throw exceptions::profile{"WASM exceeds capacity profile v1 module bytes"};
   if (code.size() < 8 || code[0] != 0 || code[1] != 0x61 || code[2] != 0x73 || code[3] != 0x6d)
      throw exceptions::parse{"Expected WebAssembly bytecode"};
   try {
      if (allocation)
         allocation->check();
      if (use == purpose::execution) {
         std::pmr::vector<detail::engine_bindings::function> registered{&storage};
         registered.reserve(functions.size());
         callbacks.reserve(functions.size());
         for (auto& function : functions) {
            if (!function.invoke)
               throw exceptions::link{"Host registration has no callable"};
            registered.push_back(binding(function, &storage));
            callbacks.push_back(std::move(function.invoke));
            function.invoke = {};
         }
         bindings = make<detail::engine_bindings>(allocation, std::move(registered), this, &impl::invoke);
      }
      engine = make<detail::engine_module>(allocation, detail::engine_runtime::acquire(), code, bindings,
                                           use != purpose::execution, policy.allow_zero_blocktype,
                                           allocation ? load_budget(*allocation) : nullptr);
      source = engine;
      limits = make<detail::engine_limits>(allocation, engine->get());
   } catch (const detail::engine_load_budget::error&) {
      if (allocation)
         allocation->check();
      throw exceptions::execution_unavailable{"WASM preparation storage is unavailable"};
   } catch (const exceptions::execution_unavailable&) {
      throw;
   } catch (const exceptions::canceled&) {
      throw;
   } catch (const exceptions::link&) {
      throw;
   } catch (const detail::engine_bindings::link_error&) {
      throw exceptions::link{"WASM import is unavailable or has an incompatible signature"};
   } catch (const std::bad_alloc&) {
      throw exceptions::allocation{"WASM module allocation failed"};
   } catch (const std::exception&) {
      throw exceptions::parse{"WASM validation failed or exceeds the engine representation"};
   }
   try {
      limits->check_profile<capacity_profile>(policy);
   } catch (const detail::engine_limits::exceeded&) {
      throw exceptions::profile{"WASM exceeds capacity profile v1"};
   }
   try {
      limits->check_metadata(policy);
      limits->check_instructions(policy);
   } catch (const std::exception&) {
      throw exceptions::parse{"WASM exceeds a caller admission limit"};
   }
}

prepared_module::impl::impl(std::span<const std::uint8_t> code, std::span<const std::uint8_t> native,
                            const options& policy, std::span<host_binding> functions, std::optional<preparation> budget)
    : impl(code, policy, functions, purpose::execution, std::move(budget)) {
   limits->validate_instantiation();
   try {
      auto compiled = make<detail::engine_module>(allocation, detail::engine_runtime::acquire(), native, bindings,
                                                  false, false, allocation ? load_budget(*allocation) : nullptr);
      if (compiled->get()->module_type != Wasm_Module_AoT)
         throw exceptions::profile{"Expected a native AOT artifact"};
      const auto& wasm = *reinterpret_cast<const WASMModule*>(source->get());
      const auto& aot = *reinterpret_cast<const AOTModule*>(compiled->get());
      if (wasm.function_count != aot.func_count || wasm.import_function_count != aot.import_func_count ||
          wasm.type_count != aot.type_count || wasm.export_count != aot.export_count)
         throw exceptions::profile{"AOT artifact does not match the admitted source shape"};
      for (std::uint32_t i = 0; i < wasm.type_count; ++i) {
         const auto& original = *reinterpret_cast<const WASMFuncType*>(wasm.types[i]);
         const auto& compiled_type = *reinterpret_cast<const AOTFuncType*>(aot.types[i]);
         if (original.param_count != compiled_type.param_count || original.result_count != compiled_type.result_count ||
             !std::equal(original.types, original.types + original.param_count + original.result_count,
                         compiled_type.types))
            throw exceptions::profile{"AOT function types differ from the admitted source"};
      }
      compiled->check_memory_limit(policy.max_pages);
      engine = std::move(compiled);
   } catch (const detail::engine_load_budget::error&) {
      if (allocation)
         allocation->check();
      throw exceptions::execution_unavailable{"AOT preparation storage is unavailable"};
   } catch (const detail::engine_bindings::link_error&) {
      throw exceptions::link{"AOT import is unavailable or has an incompatible signature"};
   } catch (const std::bad_alloc&) {
      throw exceptions::allocation{"AOT preparation allocation failed"};
   }
}

} // namespace forge::vm::wasm::interpret
