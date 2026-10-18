module;

#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <span>
#include <stop_token>
#include <system_error>
#include <utility>

module forge.vm.wasm.execution.compiled_module;
import forge.vm.wasm.interpret.backend;

#include "details/intrinsics.hxx"
#include "details/compiled_module_impl.hxx"

namespace forge::vm::wasm::execution {

compiled_module::impl::impl(std::span<const std::uint8_t> code, const compile_limits& policy) : limits(policy) {
   validate(limits);
   if (code.empty() || code.size() > limits.max_module_bytes) {
      throw exceptions::limit_exceeded{"WASM module exceeds the compilation limit"};
   }
   namespace vm = forge::vm::wasm::interpret;
   try {
      intrinsics::register_functions();
      auto owned = vm::wasm_code{code.begin(), code.end()};
      auto pointer = vm::wasm_code_ptr{owned.data(), owned.size()};
      engine = std::make_unique<backend>(pointer, owned.size(), nullptr, limits.vm, false, false);
      engine->validate();
      auto& parsed = engine->get_module();
      const auto apply = parsed.get_exported_function("apply");
      if (apply == std::numeric_limits<std::uint32_t>::max() ||
          !(vm::host_function{.params = {vm::types::i64, vm::types::i64, vm::types::i64}, .ret = {}} ==
            parsed.get_function_type(apply))) {
         throw exceptions::invalid_apply{"WASM requires an apply(i64,i64,i64) export"};
      }
      bytes = parsed.code_size() + sizeof(impl) + sizeof(compiled_module);
   } catch (const vm::exceptions::link&) {
      throw exceptions::forbidden_import{"WASM import is unavailable or has an incompatible signature"};
   } catch (const vm::exceptions::unsupported_import&) {
      throw exceptions::forbidden_import{"Only approved SDK function imports are allowed"};
   } catch (const vm::exceptions::allocation&) {
      throw exceptions::host_failure{"Host allocation failed while compiling WASM"};
   } catch (const forge::exceptions::base& error) {
      if (error.code().category() == exceptions::make_error_code(exceptions::code::invalid_wasm).category()) {
         throw;
      }
      if (error.code().category() == vm::exceptions::make_error_code(vm::exceptions::code::parse).category()) {
         throw exceptions::invalid_wasm{"WASM validation failed"};
      }
      throw exceptions::host_failure{"Host failed while compiling WASM"};
   } catch (const std::exception&) {
      throw exceptions::host_failure{"Host failed while compiling WASM"};
   } catch (...) {
      throw exceptions::host_failure{"Host failed while compiling WASM"};
   }
}

invocation_result compiled_module::impl::invoke(const invocation& call, const invocation_limits& policy,
                                                std::stop_token stop) {
   validate(policy);
   if (call.data.size() > policy.max_input_bytes) {
      throw exceptions::limit_exceeded{"Action input exceeds the invocation limit"};
   }
   namespace vm = forge::vm::wasm::interpret;
   auto imports = intrinsics{call, policy};
   try {
      engine->with_invocation(&imports, limits.vm, policy.budget, stop, [&] {
         engine->call(imports, "env", "apply", call.receiver.value, call.receiver.value, call.action.value);
      });
      return std::move(imports).result();
   } catch (const vm::exceptions::timeout&) {
      throw exceptions::deadline_exceeded{"WASM invocation deadline exceeded"};
   } catch (const vm::exceptions::canceled&) {
      throw exceptions::canceled{"WASM invocation canceled"};
   } catch (const vm::exceptions::busy&) {
      throw exceptions::busy{"WASM instance already has an active invocation"};
   } catch (const vm::exceptions::unusable&) {
      throw exceptions::unusable{"WASM instance must be discarded after a host failure"};
   } catch (const vm::exceptions::invocation_state&) {
      throw exceptions::host_failure{"Invalid host invocation state"};
   } catch (const vm::exceptions::shared_instance&) {
      throw exceptions::host_failure{"WASM code allocator is not exclusive"};
   } catch (const vm::exceptions::guest_trap& error) {
      if (error.kind == vm::exceptions::guest_trap_kind::unknown ||
          error.kind == vm::exceptions::guest_trap_kind::auxiliary_stack)
         throw exceptions::host_failure{"WASM trap classification is unavailable"};
      throw exceptions::trap{"Guest execution trapped"};
   } catch (const forge::exceptions::base& error) {
      if (error.code().category() == exceptions::make_error_code(exceptions::code::invalid_wasm).category()) {
         throw;
      }
      if (error.code().category() == vm::exceptions::make_error_code(vm::exceptions::code::parse).category()) {
         switch (static_cast<vm::exceptions::code>(error.code().value())) {
         case vm::exceptions::code::interpreter:
         case vm::exceptions::code::memory:
         case vm::exceptions::code::stack_memory:
         case vm::exceptions::code::pointer_out_of_bounds:
         case vm::exceptions::code::invalid_element:
            throw exceptions::trap{"Guest execution trapped"};
         default:
            break;
         }
      }
      throw exceptions::host_failure{"Host failed during WASM invocation"};
   } catch (const std::exception&) {
      throw exceptions::host_failure{"Host failed during WASM invocation"};
   } catch (...) {
      throw exceptions::host_failure{"Host failed during WASM invocation"};
   }
}

} // namespace forge::vm::wasm::execution
