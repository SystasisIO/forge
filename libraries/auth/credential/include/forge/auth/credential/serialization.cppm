module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <optional>

export module forge.auth.credential.serialization;

export import forge.auth.credential.record;

import forge.raw.codec;
import forge.raw.exceptions;

export namespace forge::auth::credential {

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

template <typename Stream> void unpack_credential_state(Stream& stream, state& value) {
   auto encoded = std::uint8_t{};
   forge::raw::unpack(stream, encoded);
   switch (encoded) {
   case static_cast<std::uint8_t>(state::active):
      value = state::active;
      return;
   case static_cast<std::uint8_t>(state::revoked):
      value = state::revoked;
      return;
   default:
      throw_invalid_raw<Stream>("credential state is invalid");
   }
}

template <typename Stream> void validate_time_range(const record& value) {
   if (value.updated_at < value.issued_at ||
       (value.revoked_at.has_value() && (*value.revoked_at < value.issued_at || *value.revoked_at > value.updated_at))) {
      throw_invalid_raw<Stream>("credential timestamps are invalid");
   }
}

} // namespace detail

template <typename Stream> void raw_pack(Stream& stream, const id& value) {
   forge::raw::pack(stream, value.value);
}

template <typename Stream> void raw_unpack(Stream& stream, id& value) {
   forge::raw::unpack(stream, value.value);
}

template <typename Stream> void raw_pack(Stream& stream, const binding& value) {
   forge::raw::pack(stream, value.id);
   forge::raw::pack(stream, value.generation);
}

template <typename Stream> void raw_unpack(Stream& stream, binding& value) {
   forge::raw::unpack(stream, value.id);
   forge::raw::unpack(stream, value.generation);
}

template <typename Stream> void raw_pack(Stream& stream, const record& value) {
   forge::raw::pack(stream, value.id);
   forge::raw::pack(stream, value.identity);
   forge::raw::pack(stream, value.scopes);
   forge::raw::pack(stream, value.generation);
   detail::pack_time_point(stream, value.issued_at);
   detail::pack_time_point(stream, value.updated_at);
   forge::raw::pack(stream, static_cast<std::uint8_t>(value.state));
   detail::pack_optional_time_point(stream, value.revoked_at);
}

template <typename Stream> void raw_unpack(Stream& stream, record& value) {
   forge::raw::unpack(stream, value.id);
   forge::raw::unpack(stream, value.identity);
   forge::raw::unpack(stream, value.scopes);
   forge::raw::unpack(stream, value.generation);
   detail::unpack_time_point(stream, value.issued_at);
   detail::unpack_time_point(stream, value.updated_at);
   detail::unpack_credential_state(stream, value.state);
   detail::unpack_optional_time_point(stream, value.revoked_at);
   detail::validate_time_range<Stream>(value);
}

} // namespace forge::auth::credential

export namespace forge::raw {

template <typename T> struct credential_codec_traits {
   template <typename Stream> static void pack(Stream& stream, const T& value) {
      forge::auth::credential::raw_pack(stream, value);
   }

   template <typename Stream> static void unpack(Stream& stream, T& value) {
      forge::auth::credential::raw_unpack(stream, value);
   }
};

template <> struct codec_traits<forge::auth::credential::id>
    : credential_codec_traits<forge::auth::credential::id> {};
template <> struct codec_traits<forge::auth::credential::binding>
    : credential_codec_traits<forge::auth::credential::binding> {};
template <> struct codec_traits<forge::auth::credential::record>
    : credential_codec_traits<forge::auth::credential::record> {};

} // namespace forge::raw
