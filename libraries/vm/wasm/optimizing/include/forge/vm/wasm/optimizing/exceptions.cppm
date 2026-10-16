module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.vm.wasm.optimizing.exceptions;

export import forge.exceptions;

export namespace forge::vm::wasm::optimizing::exceptions {

enum class code : std::uint32_t {
   invalid_module = 4040000,
   unsupported_target,
   unsupported_import,
   queue_full,
   compile_limit,
   cancelled,
   compiler_unavailable,
   invalid_artifact,
   cache_io,
   cache_corruption,
   shutdown_incomplete
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.vm.wasm.optimizing")

using invalid_module = ::forge::exceptions::coded_exception<code, code::invalid_module>;
using unsupported_target = ::forge::exceptions::coded_exception<code, code::unsupported_target>;
using unsupported_import = ::forge::exceptions::coded_exception<code, code::unsupported_import>;
using queue_full = ::forge::exceptions::coded_exception<code, code::queue_full>;
using compile_limit = ::forge::exceptions::coded_exception<code, code::compile_limit>;
using cancelled = ::forge::exceptions::coded_exception<code, code::cancelled>;
using compiler_unavailable = ::forge::exceptions::coded_exception<code, code::compiler_unavailable>;
using invalid_artifact = ::forge::exceptions::coded_exception<code, code::invalid_artifact>;
using cache_io = ::forge::exceptions::coded_exception<code, code::cache_io>;
using cache_corruption = ::forge::exceptions::coded_exception<code, code::cache_corruption>;
using shutdown_incomplete = ::forge::exceptions::coded_exception<code, code::shutdown_incomplete>;

} // namespace forge::vm::wasm::optimizing::exceptions
