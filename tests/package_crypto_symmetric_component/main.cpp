#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

import forge.crypto.symmetric.xsalsa20;
import forge.crypto.symmetric.salsa20;
import forge.crypto.core.secret_bytes;

extern "C" void sodium_memzero(void* pointer, std::size_t size) {
   auto* bytes = static_cast<unsigned char*>(pointer);
   for (auto index = std::size_t{0}; index < size; ++index) {
      bytes[index] = 0U;
   }
}

extern "C" void randombytes_buf(void*, std::size_t) {}

extern "C" int crypto_stream_xsalsa20(unsigned char*, unsigned long long, const unsigned char*, const unsigned char*) {
   return 0;
}

extern "C" int crypto_stream_salsa20(unsigned char*, unsigned long long, const unsigned char*, const unsigned char*) {
   return 0;
}

int main() {
   const auto salsa_key = forge::crypto::symmetric::salsa20::key{std::array<std::uint8_t, 32>{}};
   const auto salsa_nonce = forge::crypto::symmetric::salsa20::make_nonce(std::array<std::uint8_t, 8>{});
   const auto salsa = forge::crypto::core::secret_bytes{
       forge::crypto::symmetric::salsa20::keystream(salsa_key, salsa_nonce, 65)};
   if (salsa.size() != 65 || salsa.span().front() != 0x9a) {
      return 1;
   }
   const auto key = forge::crypto::symmetric::xsalsa20::key{std::array<std::uint8_t, 32>{}};
   const auto nonce = forge::crypto::symmetric::xsalsa20::nonce{};
   auto bytes = std::vector<std::uint8_t>{1, 2, 3};
   auto collision_probe = std::array<std::uint8_t, 1>{0x5a};
   sodium_memzero(collision_probe.data(), collision_probe.size());
   randombytes_buf(collision_probe.data(), collision_probe.size());
   const auto stub_result =
       crypto_stream_xsalsa20(bytes.data(), static_cast<unsigned long long>(bytes.size()), nonce.bytes.data(), key.span().data());
   auto stream = forge::crypto::symmetric::xsalsa20::stream{key, nonce};
   stream.transform(bytes);
   return collision_probe.front() != 0U || stub_result != 0 || bytes == std::vector<std::uint8_t>{1, 2, 3} ? 1 : 0;
}
