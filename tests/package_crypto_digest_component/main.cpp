#include <cstdint>
#include <span>
#include <string>
#include <utility>

import forge.crypto.digest.ripemd160;
import forge.crypto.digest.sha1;
import forge.crypto.digest.sha224;
import forge.crypto.digest.sha256;
import forge.crypto.digest.sha3;
import forge.crypto.digest.sha512;
import forge.crypto.digest.shake128;

namespace {

void consume(std::span<const std::uint8_t>) {}

template <typename T>
concept has_rvalue_data = requires(T&& value) { std::move(value).data(); };

template <typename T>
concept has_rvalue_uint8_span = requires(T&& value) { std::move(value).to_uint8_span(); };

} // namespace

int main() {
   const auto shake = forge::crypto::digest::shake128({}, 16);
   if (shake.size() != 16 || shake.front() != 0x7f || shake.back() != 0x3e) {
      return 1;
   }
   static_assert(!has_rvalue_data<forge::crypto::digest::ripemd160>);
   static_assert(!has_rvalue_data<forge::crypto::digest::sha1>);
   static_assert(!has_rvalue_data<forge::crypto::digest::sha224>);
   static_assert(!has_rvalue_data<forge::crypto::digest::sha256>);
   static_assert(!has_rvalue_data<forge::crypto::digest::sha3>);
   static_assert(!has_rvalue_data<forge::crypto::digest::sha512>);
   static_assert(!has_rvalue_uint8_span<forge::crypto::digest::sha256>);
   static_assert(!has_rvalue_uint8_span<forge::crypto::digest::sha512>);

   const auto value = forge::crypto::digest::sha256::hash(std::string{"forge"});
   const auto value_bytes = value.to_uint8_span();
   consume(value_bytes);

   auto encoder = forge::crypto::digest::sha256::encoder{};
   const auto input = std::string{"named"};
   encoder.write(input.data(), static_cast<std::uint32_t>(input.size()));
   const auto encoded = encoder.result();
   const auto encoded_bytes = encoded.to_uint8_span();

   if (encoded_bytes.size() != forge::crypto::digest::sha256::byte_size) {
      return 1;
   }
   return value_bytes.data() == reinterpret_cast<const std::uint8_t*>(value.data()) ? 0 : 1;
}
