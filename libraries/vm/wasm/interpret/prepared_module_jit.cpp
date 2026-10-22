module;

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>
#include "details/engine_module.hxx"
#include "details/engine_limits.hxx"

module forge.vm.wasm.interpret.prepared_module;

#include "details/prepared_module_impl.hxx"

namespace forge::vm::wasm::interpret {

prepared_module::prepared_module(std::span<const std::uint8_t> code, const options& policy,
                                 std::vector<host_binding> bindings, purpose use, execution_mode mode)
    : prepared_module(code, policy, std::span{bindings}, use,
                      preparation{{std::uint64_t{1} << 30, std::uint64_t{1} << 30}}, mode) {}

prepared_module::prepared_module(std::span<const std::uint8_t> code, const options& policy,
                                 std::span<host_binding> bindings, purpose use, const preparation& allocation,
                                 execution_mode mode)
    : prepared_module(code, policy, bindings, use, allocation) {
   if (mode == execution_mode::interpreter)
      return;
   if (mode != execution_mode::jit && mode != execution_mode::jit_profile)
      throw exceptions::profile{"Unsupported WASM execution mode"};
   if (use == purpose::validation_only)
      throw exceptions::profile{"Validation-only preparation cannot compile native code"};
   allocation.check();
   validate();
   check_limits(policy);
   if (_impl->engine != _impl->source)
      throw exceptions::profile{"JIT preparation requires the admitted source module"};
   try {
      _impl->engine->compile_llvm_jit(mode == execution_mode::jit_profile);
      allocation.check();
      _impl->engine->native_stack_bytes();
      allocation.seal();
   } catch (const detail::engine_load_budget::error&) {
      allocation.check();
      throw;
   } catch (const std::bad_alloc&) {
      throw exceptions::execution_unavailable{"LLVM JIT preparation allocation failed"};
   } catch (const std::runtime_error&) {
      allocation.check();
      throw exceptions::execution_unavailable{"LLVM JIT preparation failed"};
   }
}

profile_instr_map prepared_module::get_debug() const noexcept {
   if (!_impl->engine->llvm_jit_profile_ready())
      return {};
   // Bind the engine address to the complete impl owner: bindings' PMR storage
   // and callback context must survive the final engine unload as well.
   return profile_instr_map{std::shared_ptr<const void>{_impl, _impl->engine.get()},
                            &detail::engine_module::translate_jit_pc};
}

} // namespace forge::vm::wasm::interpret
