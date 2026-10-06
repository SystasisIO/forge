#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "noise_cipher_state.hxx"

namespace forge::net::p2p::detail {

struct noise_symmetric_state {
   std::vector<std::uint8_t> chaining_key;
   std::vector<std::uint8_t> hash;
   noise_cipher_state cipher;

   noise_symmetric_state();

   void mix_hash(std::span<const std::uint8_t> value);

   void mix_key(std::span<const std::uint8_t> input);

   [[nodiscard]] std::vector<std::uint8_t> encrypt_and_hash(std::span<const std::uint8_t> plaintext);

   [[nodiscard]] std::vector<std::uint8_t> decrypt_and_hash(std::span<const std::uint8_t> ciphertext);

   [[nodiscard]] std::array<noise_cipher_state, 2> split() const;
};

} // namespace forge::net::p2p::detail
