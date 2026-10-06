#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace forge::net::p2p::detail {

struct noise_cipher_state {
   std::vector<std::uint8_t> key;
   std::uint64_t nonce = 0;

   [[nodiscard]] bool has_key() const noexcept;

   [[nodiscard]] std::vector<std::uint8_t> encrypt(std::span<const std::uint8_t> ad,
                                                   std::span<const std::uint8_t> plaintext);

   [[nodiscard]] std::vector<std::uint8_t> decrypt(std::span<const std::uint8_t> ad,
                                                   std::span<const std::uint8_t> ciphertext);
};

} // namespace forge::net::p2p::detail
