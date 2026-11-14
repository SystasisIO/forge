module;

#include <blake3.h>
#include <forge/exceptions/macros.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

module forge.crypto.digest.blake3;

import forge.codec.hex;
import forge.crypto.digest;

namespace forge::crypto::digest {

struct blake3::encoder::impl {
   blake3_hasher value;
};

blake3::encoder::encoder() : _state(std::make_unique<impl>()) {
   blake3_hasher_init(&_state->value);
}

blake3::encoder::~encoder() = default;

void blake3::encoder::write(std::span<const std::byte> input) noexcept {
   blake3_hasher_update(&_state->value, input.data(), input.size());
}

void blake3::encoder::reset() noexcept {
   blake3_hasher_reset(&_state->value);
}

blake3 blake3::encoder::result() const noexcept {
   auto value = blake3{};
   blake3_hasher_finalize(&_state->value, value.bytes.data(), value.bytes.size());
   return value;
}

blake3 blake3::hash(std::span<const std::byte> input) {
   auto digest = encoder{};
   digest.write(input);
   return digest.result();
}

std::string blake3::str() const {
   return forge::codec::hex::encode(bytes);
}

blake3 blake3::from_hex(std::string_view input) {
   auto value = blake3{};
   if (input.size() != 2 * value.bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_size, "BLAKE3 digest must contain 32 bytes");
   }
   forge::codec::hex::decode(input, value.bytes);
   return value;
}

} // namespace forge::crypto::digest
