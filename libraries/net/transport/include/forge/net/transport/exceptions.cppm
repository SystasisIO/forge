module;

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <forge/exceptions/macros.hpp>

export module forge.net.transport.exceptions;

export import forge.exceptions;

export namespace forge::net::transport::exceptions {

enum class code : std::uint16_t {
   invalid_endpoint = 1,
   closed = 2,
   canceled = 3,
   frame_too_large = 4,
   protocol_error = 5,
   unsupported_protocol = 6,
   duplicate_registration = 7,
   invalid_buffer = 8,
};

using invalid_endpoint = forge::exceptions::coded_exception<code, code::invalid_endpoint>;
using closed = forge::exceptions::coded_exception<code, code::closed>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using frame_too_large = forge::exceptions::coded_exception<code, code::frame_too_large>;
using protocol_error = forge::exceptions::coded_exception<code, code::protocol_error>;
using unsupported_protocol = forge::exceptions::coded_exception<code, code::unsupported_protocol>;
using duplicate_registration = forge::exceptions::coded_exception<code, code::duplicate_registration>;
using invalid_buffer = forge::exceptions::coded_exception<code, code::invalid_buffer>;

class end_of_stream : public closed {
 public:
   using closed::closed;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.net.transport")

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& value) noexcept {
   const auto& actual = value.code();
   if (!actual || std::string_view{actual.category().name()} != "forge.net.transport") {
      return std::nullopt;
   }
   if (actual == forge::exceptions::make_error_code(code::invalid_endpoint)) {
      return code::invalid_endpoint;
   }
   if (actual == forge::exceptions::make_error_code(code::closed)) {
      return code::closed;
   }
   if (actual == forge::exceptions::make_error_code(code::canceled)) {
      return code::canceled;
   }
   if (actual == forge::exceptions::make_error_code(code::frame_too_large)) {
      return code::frame_too_large;
   }
   if (actual == forge::exceptions::make_error_code(code::protocol_error)) {
      return code::protocol_error;
   }
   if (actual == forge::exceptions::make_error_code(code::unsupported_protocol)) {
      return code::unsupported_protocol;
   }
   if (actual == forge::exceptions::make_error_code(code::duplicate_registration)) {
      return code::duplicate_registration;
   }
   if (actual == forge::exceptions::make_error_code(code::invalid_buffer)) {
      return code::invalid_buffer;
   }
   return std::nullopt;
}

[[nodiscard]] inline bool is(const forge::exceptions::base& value, code expected) noexcept {
   return value.code() == forge::exceptions::make_error_code(expected);
}

} // namespace forge::net::transport::exceptions
