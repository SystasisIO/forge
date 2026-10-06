module;

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.crypto.digest.hmac;
import forge.crypto.digest.sha256;

#include "details/noise_symmetric_state.hxx"

namespace forge::net::p2p::detail {

namespace {

[[nodiscard]] std::vector<std::uint8_t> sha256(std::span<const std::uint8_t> value) {
   const auto digest = forge::crypto::digest::sha256::hash(value);
   const auto bytes = digest.to_uint8_span();
   return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::vector<std::uint8_t> hmac_sha256(std::span<const std::uint8_t> key,
                                                    std::span<const std::uint8_t> value) {
   const auto digest = forge::crypto::digest::hmac_sha256{}.digest(key, value);
   const auto bytes = digest.to_uint8_span();
   return {bytes.begin(), bytes.end()};
}

[[nodiscard]] std::vector<std::uint8_t> concat(std::span<const std::uint8_t> left,
                                               std::span<const std::uint8_t> right) {
   auto out = std::vector<std::uint8_t>{};
   out.reserve(left.size() + right.size());
   out.insert(out.end(), left.begin(), left.end());
   out.insert(out.end(), right.begin(), right.end());
   return out;
}

[[nodiscard]] std::array<std::vector<std::uint8_t>, 2> noise_hkdf2(std::span<const std::uint8_t> chaining_key,
                                                                   std::span<const std::uint8_t> input) {
   const auto temp_key = hmac_sha256(chaining_key, input);
   const auto first_input = std::array<std::uint8_t, 1>{1};
   const auto out1 = hmac_sha256(temp_key, first_input);
   auto out2_input = out1;
   out2_input.push_back(2);
   return {out1, hmac_sha256(temp_key, out2_input)};
}

} // namespace

noise_symmetric_state::noise_symmetric_state() {
   constexpr auto protocol = std::string_view{"Noise_XX_25519_ChaChaPoly_SHA256"};
   hash.assign(32, 0);
   std::copy(protocol.begin(), protocol.end(), hash.begin());
   chaining_key = hash;
   mix_hash(std::span<const std::uint8_t>{});
}

void noise_symmetric_state::mix_hash(std::span<const std::uint8_t> value) {
   hash = sha256(concat(hash, value));
}

void noise_symmetric_state::mix_key(std::span<const std::uint8_t> input) {
   auto keys = noise_hkdf2(chaining_key, input);
   chaining_key = std::move(keys[0]);
   cipher.key = std::move(keys[1]);
   cipher.nonce = 0;
}

[[nodiscard]] std::vector<std::uint8_t> noise_symmetric_state::encrypt_and_hash(std::span<const std::uint8_t> plaintext) {
   auto ciphertext = cipher.encrypt(hash, plaintext);
   mix_hash(ciphertext);
   return ciphertext;
}

[[nodiscard]] std::vector<std::uint8_t> noise_symmetric_state::decrypt_and_hash(std::span<const std::uint8_t> ciphertext) {
   auto plaintext = cipher.decrypt(hash, ciphertext);
   mix_hash(ciphertext);
   return plaintext;
}

[[nodiscard]] std::array<noise_cipher_state, 2> noise_symmetric_state::split() const {
   const auto empty = std::span<const std::uint8_t>{};
   const auto keys = noise_hkdf2(chaining_key, empty);
   return {noise_cipher_state{.key = keys[0]}, noise_cipher_state{.key = keys[1]}};
}

} // namespace forge::net::p2p::detail
