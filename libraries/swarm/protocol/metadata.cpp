module;

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.protocol.metadata;

import forge.raw.raw;
import forge.raw.exceptions;

namespace forge::swarm::protocol {

void validate_realm(std::string_view realm) {
   if (realm.size() > max_realm_size ||
       !std::all_of(realm.begin(), realm.end(), [](unsigned char byte) { return byte >= 0x20U && byte <= 0x7eU; })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "realm must contain at most 128 printable ASCII bytes");
   }
}

void validate(const metadata_request& value) {
   validate_realm(value.realm);
}
void validate(const exchange_request& value) {
   validate_realm(value.realm);
}

void validate(const metadata& value) {
   try {
      if (core::calculate_swarm_id(value.manifest) != value.swarm) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "descriptor does not match metadata identity");
      }
   } catch (const forge::exceptions::context_error& error) {
      if (error.code().category() !=
          core::exceptions::make_error_code(core::exceptions::code::invalid_descriptor).category())
         throw;
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata descriptor is invalid");
   }
}

void validate(const metadata_chunk& value) {
   if (value.total == 0U || value.total > core::max_metadata_size || value.offset >= value.total ||
       value.offset % metadata_chunk_size != 0U ||
       value.data.size() != std::min(metadata_chunk_size, value.total - value.offset)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata chunk has invalid total, offset or length");
   }
}

std::vector<std::uint8_t> encode_metadata_chunk(const metadata_chunk& value) {
   validate(value);
   return forge::raw::pack(value);
}

metadata_chunk decode_metadata_chunk(std::span<const std::uint8_t> bytes, forge::raw::unpack_limits limits) {
   if (bytes.size() > max_metadata_chunk_wire_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "metadata chunk frame exceeds its byte limit");
   }
   limits.max_bytes = std::min({limits.max_bytes, metadata_chunk_size, static_cast<std::uint32_t>(bytes.size())});
   try {
      auto value = forge::raw::unpack_exact<metadata_chunk>(bytes, limits);
      validate(value);
      const auto canonical = forge::raw::pack(value);
      if (!std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "metadata chunk encoding is not canonical");
      }
      return value;
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "metadata chunk frame is truncated");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "metadata chunk frame is malformed");
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "metadata chunk exceeds decode allocation bounds");
   }
}

} // namespace forge::swarm::protocol
