module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>

export module forge.auth.session.serialization;

export import forge.auth.pairing.serialization;
export import forge.auth.session.types;

import forge.raw.codec;
import forge.raw.exceptions;

export namespace forge::auth::session {

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

template <typename Stream> void pack_duration(Stream& stream, std::chrono::system_clock::duration value) {
   const auto encoded = std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
   forge::raw::pack(stream, encoded);
}

template <typename Stream> void unpack_duration(Stream& stream, std::chrono::system_clock::duration& value) {
   auto encoded = std::int64_t{};
   forge::raw::unpack(stream, encoded);
   value = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::nanoseconds{encoded});
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

template <typename Stream> void unpack_session_state(Stream& stream, session_state& value) {
   auto encoded = std::uint8_t{};
   forge::raw::unpack(stream, encoded);
   switch (encoded) {
   case static_cast<std::uint8_t>(session_state::active):
      value = session_state::active;
      return;
   case static_cast<std::uint8_t>(session_state::rotated):
      value = session_state::rotated;
      return;
   case static_cast<std::uint8_t>(session_state::revoked):
      value = session_state::revoked;
      return;
   default:
      throw_invalid_raw<Stream>("session state is invalid");
   }
}

template <typename Stream> void unpack_device_grant_state(Stream& stream, device_grant_state& value) {
   auto encoded = std::uint8_t{};
   forge::raw::unpack(stream, encoded);
   switch (encoded) {
   case static_cast<std::uint8_t>(device_grant_state::active):
      value = device_grant_state::active;
      return;
   case static_cast<std::uint8_t>(device_grant_state::rotated):
      value = device_grant_state::rotated;
      return;
   case static_cast<std::uint8_t>(device_grant_state::revoked):
      value = device_grant_state::revoked;
      return;
   default:
      throw_invalid_raw<Stream>("device grant state is invalid");
   }
}

template <typename Stream> void validate_time_range(const session_record& value) {
   const auto supported = [](time_point timestamp) { return timestamp >= time_point{}; };
   if (!supported(value.created_at) || !supported(value.last_activity_at) || !supported(value.idle_expires_at) ||
       !supported(value.absolute_expires_at) || (value.terminal_at.has_value() && !supported(*value.terminal_at)) ||
       value.last_activity_at < value.created_at || value.absolute_expires_at <= value.created_at ||
       value.absolute_expires_at <= value.last_activity_at ||
       value.idle_timeout <= std::chrono::system_clock::duration::zero()) {
      throw_invalid_raw<Stream>("session timestamps or idle timeout are invalid");
   }

   const auto extension = std::min(value.idle_timeout, value.absolute_expires_at - value.last_activity_at);
   if (value.idle_expires_at != value.last_activity_at + extension ||
       (value.terminal_at.has_value() &&
        (*value.terminal_at < value.created_at || *value.terminal_at < value.last_activity_at ||
         *value.terminal_at >= value.idle_expires_at || *value.terminal_at >= value.absolute_expires_at))) {
      throw_invalid_raw<Stream>("session timestamp range is invalid");
   }
}

template <typename Stream> void validate_time_range(const device_grant_record& value) {
   const auto supported = [](time_point timestamp) { return timestamp >= time_point{}; };
   if (!supported(value.issued_at) || !supported(value.last_activity_at) || !supported(value.idle_expires_at) ||
       !supported(value.absolute_expires_at) || (value.terminal_at.has_value() && !supported(*value.terminal_at)) ||
       value.last_activity_at < value.issued_at || value.absolute_expires_at <= value.issued_at ||
       value.absolute_expires_at <= value.last_activity_at ||
       value.idle_timeout <= std::chrono::system_clock::duration::zero()) {
      throw_invalid_raw<Stream>("device grant timestamps or idle timeout are invalid");
   }

   const auto extension = std::min(value.idle_timeout, value.absolute_expires_at - value.last_activity_at);
   if (value.idle_expires_at != value.last_activity_at + extension ||
       (value.terminal_at.has_value() &&
        (*value.terminal_at < value.last_activity_at || *value.terminal_at >= value.idle_expires_at ||
         *value.terminal_at >= value.absolute_expires_at))) {
      throw_invalid_raw<Stream>("device grant timestamp range is invalid");
   }
}

} // namespace detail

template <typename Stream> void raw_pack(Stream& stream, const session_record& value) {
   forge::raw::pack(stream, value.session_digest);
   forge::raw::pack(stream, value.csrf_digest);
   forge::raw::pack(stream, value.credential_id);
   forge::raw::pack(stream, value.credential_generation);
   forge::raw::pack(stream, value.identity);
   forge::raw::pack(stream, value.scopes);
   detail::pack_time_point(stream, value.created_at);
   detail::pack_time_point(stream, value.last_activity_at);
   detail::pack_time_point(stream, value.idle_expires_at);
   detail::pack_time_point(stream, value.absolute_expires_at);
   detail::pack_duration(stream, value.idle_timeout);
   forge::raw::pack(stream, static_cast<std::uint8_t>(value.state));
   detail::pack_optional_time_point(stream, value.terminal_at);
}

template <typename Stream> void raw_unpack(Stream& stream, session_record& value) {
   forge::raw::unpack(stream, value.session_digest);
   forge::raw::unpack(stream, value.csrf_digest);
   forge::raw::unpack(stream, value.credential_id);
   forge::raw::unpack(stream, value.credential_generation);
   forge::raw::unpack(stream, value.identity);
   forge::raw::unpack(stream, value.scopes);
   detail::unpack_time_point(stream, value.created_at);
   detail::unpack_time_point(stream, value.last_activity_at);
   detail::unpack_time_point(stream, value.idle_expires_at);
   detail::unpack_time_point(stream, value.absolute_expires_at);
   detail::unpack_duration(stream, value.idle_timeout);
   detail::unpack_session_state(stream, value.state);
   detail::unpack_optional_time_point(stream, value.terminal_at);
   detail::validate_time_range<Stream>(value);
}

template <typename Stream> void raw_pack(Stream& stream, const device_grant_record& value) {
   forge::raw::pack(stream, value.token_digest);
   forge::raw::pack(stream, value.credential_id);
   forge::raw::pack(stream, value.credential_generation);
   forge::raw::pack(stream, value.rotation_generation);
   detail::pack_time_point(stream, value.issued_at);
   detail::pack_time_point(stream, value.last_activity_at);
   detail::pack_time_point(stream, value.idle_expires_at);
   detail::pack_time_point(stream, value.absolute_expires_at);
   detail::pack_duration(stream, value.idle_timeout);
   forge::raw::pack(stream, static_cast<std::uint8_t>(value.state));
   detail::pack_optional_time_point(stream, value.terminal_at);
}

template <typename Stream> void raw_unpack(Stream& stream, device_grant_record& value) {
   forge::raw::unpack(stream, value.token_digest);
   forge::raw::unpack(stream, value.credential_id);
   forge::raw::unpack(stream, value.credential_generation);
   forge::raw::unpack(stream, value.rotation_generation);
   detail::unpack_time_point(stream, value.issued_at);
   detail::unpack_time_point(stream, value.last_activity_at);
   detail::unpack_time_point(stream, value.idle_expires_at);
   detail::unpack_time_point(stream, value.absolute_expires_at);
   detail::unpack_duration(stream, value.idle_timeout);
   detail::unpack_device_grant_state(stream, value.state);
   detail::unpack_optional_time_point(stream, value.terminal_at);
   if (value.token_digest.empty() || value.credential_id.value.empty() || value.credential_generation == 0 ||
       value.rotation_generation == 0 || (value.state == device_grant_state::active && value.terminal_at.has_value()) ||
       (value.state != device_grant_state::active && !value.terminal_at.has_value())) {
      detail::throw_invalid_raw<Stream>("device grant record is invalid");
   }
   detail::validate_time_range<Stream>(value);
}

} // namespace forge::auth::session

export namespace forge::raw {

template <> struct codec_traits<forge::auth::session::session_record> {
   template <typename Stream> static void pack(Stream& stream, const forge::auth::session::session_record& value) {
      forge::auth::session::raw_pack(stream, value);
   }

   template <typename Stream> static void unpack(Stream& stream, forge::auth::session::session_record& value) {
      forge::auth::session::raw_unpack(stream, value);
   }
};

template <> struct codec_traits<forge::auth::session::device_grant_record> {
   template <typename Stream> static void pack(Stream& stream, const forge::auth::session::device_grant_record& value) {
      forge::auth::session::raw_pack(stream, value);
   }

   template <typename Stream> static void unpack(Stream& stream, forge::auth::session::device_grant_record& value) {
      forge::auth::session::raw_unpack(stream, value);
   }
};

} // namespace forge::raw
