module;

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <forge/exceptions/macros.hpp>

export module forge.net.yamux.exceptions;

export import forge.exceptions;

export namespace forge::net::yamux::exceptions {

enum class code : std::uint16_t {
   invalid_options = 1,
   protocol_error = 2,
   resource_limit = 3,
   stream_reset = 4,
   closed = 5,
   canceled = 6,
};

using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using protocol_error = forge::exceptions::coded_exception<code, code::protocol_error>;
using resource_limit = forge::exceptions::coded_exception<code, code::resource_limit>;
using stream_reset = forge::exceptions::coded_exception<code, code::stream_reset>;
using closed = forge::exceptions::coded_exception<code, code::closed>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;

class end_of_stream : public closed {
 public:
   using closed::closed;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.net.yamux")

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& value) noexcept {
   const auto& actual = value.code();
   if (!actual || std::string_view{actual.category().name()} != "forge.net.yamux") {
      return std::nullopt;
   }
   if (actual == forge::exceptions::make_error_code(code::invalid_options)) {
      return code::invalid_options;
   }
   if (actual == forge::exceptions::make_error_code(code::protocol_error)) {
      return code::protocol_error;
   }
   if (actual == forge::exceptions::make_error_code(code::resource_limit)) {
      return code::resource_limit;
   }
   if (actual == forge::exceptions::make_error_code(code::stream_reset)) {
      return code::stream_reset;
   }
   if (actual == forge::exceptions::make_error_code(code::closed)) {
      return code::closed;
   }
   if (actual == forge::exceptions::make_error_code(code::canceled)) {
      return code::canceled;
   }
   return std::nullopt;
}

[[nodiscard]] inline bool is(const forge::exceptions::base& value, code expected) noexcept {
   return value.code() == forge::exceptions::make_error_code(expected);
}

} // namespace forge::net::yamux::exceptions
