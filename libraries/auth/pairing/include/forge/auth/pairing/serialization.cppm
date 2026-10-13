module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

export module forge.auth.pairing.serialization;

export import forge.auth.credential.serialization;
export import forge.auth.pairing.types;

import forge.raw.codec;
import forge.raw.exceptions;

export namespace forge::auth::pairing {

namespace detail {

template <typename Stream> [[noreturn]] void throw_invalid_raw(const char* message) {
   FORGE_THROW_EXCEPTION(forge::raw::exceptions::codec_error, message);
}

template <typename Stream> void pack_time_point(Stream& stream, time_point value) {
   const auto encoded = std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count();
   forge::raw::pack(stream, encoded);
}

template <typename Stream> void unpack_time_point(Stream& stream, time_point& value) {
   auto encoded = std::int64_t{};
   forge::raw::unpack(stream, encoded);
   value = time_point{std::chrono::duration_cast<time_point::duration>(std::chrono::nanoseconds{encoded})};
}

template <typename Stream> void pack_optional_time_point(Stream& stream, const std::optional<time_point>& value) {
   forge::raw::pack(stream, value.has_value());
   if (value.has_value()) {
      pack_time_point(stream, *value);
   }
}

template <typename Stream> void unpack_optional_time_point(Stream& stream, std::optional<time_point>& value) {
   auto present = false;
   forge::raw::unpack(stream, present);
   if (!present) {
      value.reset();
      return;
   }
   auto decoded = time_point{};
   unpack_time_point(stream, decoded);
   value = decoded;
}

template <typename Stream> void unpack_pending_state(Stream& stream, pending_state& value) {
   auto encoded = std::uint8_t{};
   forge::raw::unpack(stream, encoded);
   switch (encoded) {
   case static_cast<std::uint8_t>(pending_state::pending):
      value = pending_state::pending;
      return;
   case static_cast<std::uint8_t>(pending_state::approved):
      value = pending_state::approved;
      return;
   case static_cast<std::uint8_t>(pending_state::rejected):
      value = pending_state::rejected;
      return;
   case static_cast<std::uint8_t>(pending_state::superseded):
      value = pending_state::superseded;
      return;
   default:
      throw_invalid_raw<Stream>("pairing pending state is invalid");
   }
}

template <typename Stream> void validate_time_range(const bootstrap_record& value) {
   if (value.expires_at <= value.created_at) {
      throw_invalid_raw<Stream>("pairing bootstrap timestamps are invalid");
   }
}

template <typename Stream> void validate_time_range(const pending_request& value) {
   if (value.expires_at <= value.created_at ||
       (value.resolved_at.has_value() &&
        (*value.resolved_at < value.created_at || *value.resolved_at >= value.expires_at))) {
      throw_invalid_raw<Stream>("pairing pending timestamps are invalid");
   }
}

} // namespace detail

template <typename Stream> void raw_pack(Stream& stream, const token_digest& value) {
   forge::raw::pack(stream, value.value);
}

template <typename Stream> void raw_unpack(Stream& stream, token_digest& value) {
   forge::raw::unpack(stream, value.value);
}

template <typename Stream> void raw_pack(Stream& stream, const bootstrap_record& value) {
   forge::raw::pack(stream, value.digest);
   forge::raw::pack(stream, value.scope_baseline);
   detail::pack_time_point(stream, value.created_at);
   detail::pack_time_point(stream, value.expires_at);
   forge::raw::pack(stream, value.consumed);
}

template <typename Stream> void raw_unpack(Stream& stream, bootstrap_record& value) {
   forge::raw::unpack(stream, value.digest);
   forge::raw::unpack(stream, value.scope_baseline);
   detail::unpack_time_point(stream, value.created_at);
   detail::unpack_time_point(stream, value.expires_at);
   forge::raw::unpack(stream, value.consumed);
   detail::validate_time_range<Stream>(value);
}

template <typename Stream> void raw_pack(Stream& stream, const pending_request& value) {
   forge::raw::pack(stream, value.identity);
   forge::raw::pack(stream, value.requested_scopes);
   forge::raw::pack(stream, value.scope_baseline);
   forge::raw::pack(stream, value.pre_session_digest);
   detail::pack_time_point(stream, value.created_at);
   detail::pack_time_point(stream, value.expires_at);
   forge::raw::pack(stream, static_cast<std::uint8_t>(value.state));
   detail::pack_optional_time_point(stream, value.resolved_at);
   forge::raw::pack(stream, value.approved_credential);
   forge::raw::pack(stream, value.pre_session_consumed);
}

template <typename Stream> void raw_unpack(Stream& stream, pending_request& value) {
   forge::raw::unpack(stream, value.identity);
   forge::raw::unpack(stream, value.requested_scopes);
   forge::raw::unpack(stream, value.scope_baseline);
   forge::raw::unpack(stream, value.pre_session_digest);
   detail::unpack_time_point(stream, value.created_at);
   detail::unpack_time_point(stream, value.expires_at);
   detail::unpack_pending_state(stream, value.state);
   detail::unpack_optional_time_point(stream, value.resolved_at);
   forge::raw::unpack(stream, value.approved_credential);
   forge::raw::unpack(stream, value.pre_session_consumed);
   detail::validate_time_range<Stream>(value);
}

} // namespace forge::auth::pairing

export namespace forge::raw {

template <typename T> struct pairing_codec_traits {
   template <typename Stream> static void pack(Stream& stream, const T& value) {
      forge::auth::pairing::raw_pack(stream, value);
   }

   template <typename Stream> static void unpack(Stream& stream, T& value) {
      forge::auth::pairing::raw_unpack(stream, value);
   }
};

template <>
struct codec_traits<forge::auth::pairing::token_digest> : pairing_codec_traits<forge::auth::pairing::token_digest> {};
template <>
struct codec_traits<forge::auth::pairing::bootstrap_record>
    : pairing_codec_traits<forge::auth::pairing::bootstrap_record> {};
template <>
struct codec_traits<forge::auth::pairing::pending_request>
    : pairing_codec_traits<forge::auth::pairing::pending_request> {};

} // namespace forge::raw
