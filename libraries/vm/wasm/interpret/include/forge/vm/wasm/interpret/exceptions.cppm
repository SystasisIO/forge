module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>
#include <source_location>
#include <string>
#include <utility>

export module forge.vm.wasm.interpret.exceptions;

export import forge.exceptions;

export namespace forge::vm::wasm::interpret::exceptions {

enum class code : std::uint32_t {
   interpreter = 4000000,
   section_length = 4000001,
   allocation = 4000002,
   double_free = 4000003,
   vector_out_of_bounds = 4000004,
   unsupported_import = 4000005,
   parse = 4000006,
   memory = 4000007,
   stack_memory = 4000008,
   invalid_element = 4000009,
   link = 4000010,
   execution_unavailable = 4000011,
   pointer_out_of_bounds = 4010000,
   timeout = 4010001,
   exit = 4010002,
   canceled = 4010003,
   busy = 4010004,
   shared_instance = 4010005,
   unusable = 4010006,
   invocation_state = 4010007,
   interrupted = 4010008,
   span = 4020000,
   profile = 4030000,
   numeric_arity = 4040000,
   unsupported_numeric = 4040001,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.vm.wasm.interpret")

using interpreter = ::forge::exceptions::coded_exception<code, code::interpreter>;
using section_length = ::forge::exceptions::coded_exception<code, code::section_length>;
using allocation = ::forge::exceptions::coded_exception<code, code::allocation>;
using double_free = ::forge::exceptions::coded_exception<code, code::double_free>;
using vector_out_of_bounds = ::forge::exceptions::coded_exception<code, code::vector_out_of_bounds>;
using unsupported_import = ::forge::exceptions::coded_exception<code, code::unsupported_import>;
using parse = ::forge::exceptions::coded_exception<code, code::parse>;
using memory = ::forge::exceptions::coded_exception<code, code::memory>;
using stack_memory = ::forge::exceptions::coded_exception<code, code::stack_memory>;
using invalid_element = ::forge::exceptions::coded_exception<code, code::invalid_element>;
using link = ::forge::exceptions::coded_exception<code, code::link>;
// Local execution machinery is unavailable; this is not a deterministic guest trap.
using execution_unavailable = ::forge::exceptions::coded_exception<code, code::execution_unavailable>;
using pointer_out_of_bounds = ::forge::exceptions::coded_exception<code, code::pointer_out_of_bounds>;
using timeout = ::forge::exceptions::coded_exception<code, code::timeout>;
using exit = ::forge::exceptions::coded_exception<code, code::exit>;
using canceled = ::forge::exceptions::coded_exception<code, code::canceled>;
using busy = ::forge::exceptions::coded_exception<code, code::busy>;
using shared_instance = ::forge::exceptions::coded_exception<code, code::shared_instance>;
using unusable = ::forge::exceptions::coded_exception<code, code::unusable>;
using invocation_state = ::forge::exceptions::coded_exception<code, code::invocation_state>;
using interrupted = ::forge::exceptions::coded_exception<code, code::interrupted>;
using span = ::forge::exceptions::coded_exception<code, code::span>;
using profile = ::forge::exceptions::coded_exception<code, code::profile>;
using numeric_arity = ::forge::exceptions::coded_exception<code, code::numeric_arity>;
using unsupported_numeric = ::forge::exceptions::coded_exception<code, code::unsupported_numeric>;

} // namespace forge::vm::wasm::interpret::exceptions

export namespace forge::vm::wasm::interpret::detail {

template <typename Exception, typename Message>
[[noreturn]] inline void fail(Message&& message, std::source_location location = std::source_location::current()) {
   throw Exception{std::string{std::forward<Message>(message)}, {}, location};
}

template <typename Exception, typename Message>
inline void check(bool expression, Message&& message, std::source_location location = std::source_location::current()) {
   if (!expression) [[unlikely]] {
      fail<Exception>(std::forward<Message>(message), location);
   }
}

} // namespace forge::vm::wasm::interpret::detail
