module;

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <forge/exceptions/macros.hpp>

export module forge.net.quic.exceptions;

export import forge.exceptions;

export namespace forge::net::quic::exceptions {

enum class code : std::uint16_t {
   invalid_endpoint = 1,
   invalid_options = 2,
   dependency_unavailable = 3,
   connect_timeout = 4,
   handshake_timeout = 5,
   idle_timeout = 6,
   tls_failed = 7,
   peer_verification_failed = 8,
   alpn_mismatch = 9,
   frame_too_large = 10,
   malformed_frame = 11,
   backpressure_rejected = 12,
   connection_closed = 13,
   stream_closed = 14,
   stream_reset = 15,
   canceled = 16,
   unsupported = 17,
   internal = 18,
   connection_rejected = 19,
};

using invalid_endpoint = forge::exceptions::coded_exception<code, code::invalid_endpoint>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using dependency_unavailable = forge::exceptions::coded_exception<code, code::dependency_unavailable>;
using connect_timeout = forge::exceptions::coded_exception<code, code::connect_timeout>;
using handshake_timeout = forge::exceptions::coded_exception<code, code::handshake_timeout>;
using idle_timeout = forge::exceptions::coded_exception<code, code::idle_timeout>;
using tls_failed = forge::exceptions::coded_exception<code, code::tls_failed>;
using peer_verification_failed = forge::exceptions::coded_exception<code, code::peer_verification_failed>;
using alpn_mismatch = forge::exceptions::coded_exception<code, code::alpn_mismatch>;
using frame_too_large = forge::exceptions::coded_exception<code, code::frame_too_large>;
using malformed_frame = forge::exceptions::coded_exception<code, code::malformed_frame>;
using backpressure_rejected = forge::exceptions::coded_exception<code, code::backpressure_rejected>;
using connection_rejected = forge::exceptions::coded_exception<code, code::connection_rejected>;
using connection_closed = forge::exceptions::coded_exception<code, code::connection_closed>;
using stream_closed = forge::exceptions::coded_exception<code, code::stream_closed>;
using stream_reset = forge::exceptions::coded_exception<code, code::stream_reset>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using unsupported = forge::exceptions::coded_exception<code, code::unsupported>;
using internal = forge::exceptions::coded_exception<code, code::internal>;

class end_of_stream : public stream_closed {
 public:
   using stream_closed::stream_closed;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.net.quic")

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& error) noexcept {
   const auto& value = error.code();
   if (!value || std::string_view{value.category().name()} != "forge.net.quic") {
      return std::nullopt;
   }
   return static_cast<code>(value.value());
}

[[nodiscard]] inline bool is(const forge::exceptions::base& error, code value) noexcept {
   return error.code() == forge::exceptions::make_error_code(value);
}

} // namespace forge::net::quic::exceptions
