module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

module forge.net.p2p.node;

import forge.crypto.symmetric.chacha20_poly1305;
import forge.net.p2p.exceptions;

#include "details/noise_cipher_state.hxx"

namespace forge::net::p2p::detail {

namespace {

[[nodiscard]] std::array<std::uint8_t, 12> noise_nonce(std::uint64_t value) {
   auto out = std::array<std::uint8_t, 12>{};
   for (auto index = 0; index != 8; ++index) {
      out[4 + index] = static_cast<std::uint8_t>((value >> (8U * index)) & 0xffU);
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> chacha20_poly1305_encrypt(std::span<const std::uint8_t> key,
                                                                  std::uint64_t nonce_value,
                                                                  std::span<const std::uint8_t> ad,
                                                                  std::span<const std::uint8_t> plaintext) {
   if (key.size() != forge::crypto::symmetric::chacha20_poly1305::key{}.size()) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise cipher key must be 32 bytes");
   }
   auto cipher_key = forge::crypto::symmetric::chacha20_poly1305::key{};
   std::copy(key.begin(), key.end(), cipher_key.begin());
   const auto nonce = noise_nonce(nonce_value);
   return forge::crypto::symmetric::chacha20_poly1305::encrypt(cipher_key, nonce, ad, plaintext);
}

[[nodiscard]] std::vector<std::uint8_t> chacha20_poly1305_decrypt(std::span<const std::uint8_t> key,
                                                                  std::uint64_t nonce_value,
                                                                  std::span<const std::uint8_t> ad,
                                                                  std::span<const std::uint8_t> ciphertext) {
   if (ciphertext.size() < 16) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise ciphertext is missing authentication tag");
   }
   if (key.size() != forge::crypto::symmetric::chacha20_poly1305::key{}.size()) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "Noise cipher key must be 32 bytes");
   }
   auto cipher_key = forge::crypto::symmetric::chacha20_poly1305::key{};
   std::copy(key.begin(), key.end(), cipher_key.begin());
   const auto nonce = noise_nonce(nonce_value);
   try {
      return forge::crypto::symmetric::chacha20_poly1305::decrypt(cipher_key, nonce, ad, ciphertext);
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "Noise authentication failed");
   }
}

} // namespace

[[nodiscard]] bool noise_cipher_state::has_key() const noexcept {
   return !key.empty();
}

[[nodiscard]] std::vector<std::uint8_t> noise_cipher_state::encrypt(std::span<const std::uint8_t> ad,
                                                   std::span<const std::uint8_t> plaintext) {
   if (!has_key()) {
      return {plaintext.begin(), plaintext.end()};
   }
   auto out = chacha20_poly1305_encrypt(key, nonce, ad, plaintext);
   ++nonce;
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> noise_cipher_state::decrypt(std::span<const std::uint8_t> ad,
                                                   std::span<const std::uint8_t> ciphertext) {
   if (!has_key()) {
      return {ciphertext.begin(), ciphertext.end()};
   }
   auto out = chacha20_poly1305_decrypt(key, nonce, ad, ciphertext);
   ++nonce;
   return out;
}

} // namespace forge::net::p2p::detail
