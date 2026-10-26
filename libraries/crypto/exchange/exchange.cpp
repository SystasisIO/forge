module;

#include <forge/exceptions/policy.hpp>
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

module forge.crypto.exchange;

#if defined(FORGE_CONTRACT_GUEST)
import forge.contract.crypto;
import forge.contract.crypto_ext;
#else
import forge.crypto.digest.sha3;
#endif

namespace forge::crypto::exchange {
namespace {

forge::crypto::digest::sha256 hash(std::span<const std::uint8_t> bytes) {
#if defined(FORGE_CONTRACT_GUEST)
   return forge::contract::sha256(reinterpret_cast<const char*>(bytes.data()),
                                  static_cast<std::uint32_t>(bytes.size()));
#else
   return forge::crypto::digest::sha256::hash(bytes);
#endif
}

void require(bool condition, const char* message) {
   if (!condition) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_input, message);
   }
}

std::size_t encrypted_size(std::uint32_t size) {
   return ((static_cast<std::size_t>(size) + blinder_bytes + 31U) / 32U) * 32U;
}

forge::crypto::core::bytes transform(const key& secret, std::uint32_t index, std::span<const std::uint8_t> input) {
   require(index < max_blocks && input.size() <= word_stride * 32U, "exchange counter or input exceeds profile");
   auto result = forge::crypto::core::bytes(input.size());
   auto counter_and_key = std::array<std::uint8_t, 64>{};
   std::copy(secret.begin(), secret.end(), counter_and_key.begin() + 32);
   for (auto offset = std::size_t{0}; offset < input.size(); offset += 32U) {
      const auto counter = static_cast<std::uint64_t>(index) * word_stride + offset / 32U;
      for (auto byte = std::size_t{0}; byte < 8U; ++byte) {
         counter_and_key[31U - byte] = static_cast<std::uint8_t>(counter >> (byte * 8U));
      }
#if defined(FORGE_CONTRACT_GUEST)
      const auto pad = forge::contract::keccak(reinterpret_cast<const char*>(counter_and_key.data()), 64U);
#else
      const auto pad =
          forge::crypto::digest::sha3::hash(reinterpret_cast<const char*>(counter_and_key.data()), 64U, false);
#endif
      const auto length = std::min(std::size_t{32}, input.size() - offset);
      const auto* pad_bytes = reinterpret_cast<const std::uint8_t*>(pad.data());
      for (auto byte = std::size_t{0}; byte < length; ++byte) {
         result[offset + byte] = input[offset + byte] ^ pad_bytes[byte];
      }
   }
   return result;
}

bool correct(const manifest& value, const key& secret, const fault& witness, forge::crypto::core::bytes& decoded) {
   validate(value);
   require(witness.index < value.blocks.size(), "exchange witness index is outside the manifest");
   require(witness.ciphertext.size() <= word_stride * 32U, "exchange witness exceeds block limit");
   require(hash(secret) == value.key_hash, "exchange key does not match manifest");
   const auto& expected = value.blocks[witness.index];
   require(hash(witness.ciphertext) == expected.ciphertext, "exchange witness is not the authenticated ciphertext");
   if (witness.ciphertext.size() != encrypted_size(expected.size)) {
      return false;
   }
   decoded = transform(secret, witness.index, witness.ciphertext);
   const auto end = static_cast<std::size_t>(expected.size) + blinder_bytes;
   if (std::any_of(decoded.begin() + end, decoded.end(), [](std::uint8_t byte) { return byte != 0; })) {
      return false;
   }
   return hash(std::span<const std::uint8_t>{decoded}.first(end)) == expected.source;
}

} // namespace

void validate(const manifest& value) {
   require(value.version == 1, "unsupported exchange format");
   require(value.context != forge::crypto::digest::sha256{} && value.evidence != forge::crypto::digest::sha256{} &&
               value.key_hash != forge::crypto::digest::sha256{},
           "exchange binding is empty");
   require(!value.blocks.empty() && value.blocks.size() <= max_blocks, "exchange block count exceeds profile");
   for (auto index = std::size_t{0}; index < value.blocks.size(); ++index) {
      const auto& item = value.blocks[index];
      require(item.size > 0 && item.size <= block_bytes &&
                  (index + 1 == value.blocks.size() || item.size == block_bytes),
              "exchange block size is not canonical");
      require(item.source != forge::crypto::digest::sha256{} && item.ciphertext != forge::crypto::digest::sha256{},
              "exchange block commitment is empty");
   }
}

forge::crypto::core::bytes encrypt(const key& secret, std::uint32_t index, std::span<const std::uint8_t> opening) {
   require(opening.size() > blinder_bytes && opening.size() <= block_bytes + blinder_bytes,
           "exchange opening size exceeds profile");
   auto padded = forge::crypto::core::bytes(opening.begin(), opening.end());
   padded.resize(encrypted_size(static_cast<std::uint32_t>(opening.size() - blinder_bytes)), 0);
   return transform(secret, index, padded);
}

forge::crypto::core::bytes open(const manifest& value, const key& secret, const fault& witness) {
   auto decoded = forge::crypto::core::bytes{};
   if (!correct(value, secret, witness, decoded)) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_delivery,
                                   "authenticated exchange block violates source commitment");
   }
   decoded.resize(value.blocks[witness.index].size);
   return decoded;
}

bool proves_fault(const manifest& value, const key& secret, const fault& witness) {
   auto decoded = forge::crypto::core::bytes{};
   return !correct(value, secret, witness, decoded);
}

} // namespace forge::crypto::exchange
