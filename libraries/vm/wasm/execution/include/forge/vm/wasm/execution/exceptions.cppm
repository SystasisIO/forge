module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.vm.wasm.execution.exceptions;
export import forge.exceptions;

export namespace forge::vm::wasm::execution::exceptions {

enum class code : std::uint16_t {
   invalid_options = 1,
   invalid_wasm,
   forbidden_import,
   invalid_apply,
   limit_exceeded,
   assertion_failed,
   trap,
   deadline_exceeded,
   canceled,
   busy,
   unusable,
   host_failure,
};

using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using invalid_wasm = forge::exceptions::coded_exception<code, code::invalid_wasm>;
using forbidden_import = forge::exceptions::coded_exception<code, code::forbidden_import>;
using invalid_apply = forge::exceptions::coded_exception<code, code::invalid_apply>;
using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
using assertion_failed = forge::exceptions::coded_exception<code, code::assertion_failed>;
using trap = forge::exceptions::coded_exception<code, code::trap>;
using deadline_exceeded = forge::exceptions::coded_exception<code, code::deadline_exceeded>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using busy = forge::exceptions::coded_exception<code, code::busy>;
using unusable = forge::exceptions::coded_exception<code, code::unusable>;
using host_failure = forge::exceptions::coded_exception<code, code::host_failure>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.vm.wasm.execution")

} // namespace forge::vm::wasm::execution::exceptions
