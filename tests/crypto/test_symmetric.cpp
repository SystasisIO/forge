#include <boost/test/unit_test.hpp>
#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

import forge.crypto.symmetric.aes;
import forge.crypto.symmetric.kdf;
import forge.crypto.symmetric.xsalsa20;
import forge.crypto.symmetric.salsa20;
import forge.crypto.digest.sha256;
import forge.crypto.core.random;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.exceptions;
import forge.raw.raw;

#include "details/counter_state.hxx"

struct encrypted_record {
   std::uint32_t id = 0;
   std::string name;
};

BOOST_DESCRIBE_STRUCT(encrypted_record, (), (id, name))

extern "C" int forge_xsalsa20_vendor_xor_ic(unsigned char* output, const unsigned char* input,
                                             unsigned long long size, const unsigned char* nonce,
                                             std::uint64_t initial_block_counter, const unsigned char* key);

namespace {

namespace xsalsa20 = forge::crypto::symmetric::xsalsa20;

[[nodiscard]] forge::crypto::symmetric::xsalsa20::key xsalsa20_official_key() {
   return forge::crypto::symmetric::xsalsa20::key{std::array<std::uint8_t, 32>{
       0x1b, 0x27, 0x55, 0x64, 0x73, 0xe9, 0x85, 0xd4, 0x62, 0xcd, 0x51, 0x19, 0x7a, 0x9a, 0x46, 0xc7,
       0x60, 0x09, 0x54, 0x9e, 0xac, 0x64, 0x74, 0xf2, 0x06, 0xc4, 0xee, 0x08, 0x44, 0xf6, 0x83, 0x89,
   }};
}

[[nodiscard]] forge::crypto::symmetric::xsalsa20::nonce xsalsa20_official_nonce() {
   return forge::crypto::symmetric::xsalsa20::nonce{.bytes = {
       0x69, 0x69, 0x6e, 0xe9, 0x55, 0xb6, 0x2b, 0x73, 0xcd, 0x62, 0xbd, 0xa8,
       0x75, 0xfc, 0x73, 0xd6, 0x82, 0x19, 0xe0, 0x03, 0x6b, 0x7a, 0x0b, 0x37,
   }};
}

} // namespace

BOOST_AUTO_TEST_SUITE(crypto_symmetric)

BOOST_AUTO_TEST_CASE(random_bytes_and_key_have_requested_sizes) try {
   const auto nonce = forge::crypto::core::random_bytes(12);
   const auto empty = forge::crypto::core::random_bytes(0);
   const auto fixed = forge::crypto::core::random_array<24>();
   const auto key = forge::crypto::symmetric::aes::generate_aes256_key();

   BOOST_CHECK_EQUAL(nonce.size(), 12U);
   BOOST_CHECK(empty.empty());
   BOOST_CHECK_EQUAL(fixed.size(), 24U);
   BOOST_CHECK_EQUAL(key.bytes.size(), forge::crypto::symmetric::aes::aes256_key_size);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes_key_move_clears_source_material) try {
   auto source = forge::crypto::symmetric::aes::generate_aes256_key();
   const auto expected = source.bytes;
   auto destination = std::move(source);

   BOOST_CHECK_EQUAL_COLLECTIONS(destination.bytes.begin(), destination.bytes.end(), expected.begin(), expected.end());
   BOOST_CHECK(std::ranges::all_of(source.bytes, [](std::uint8_t value) { return value == 0U; }));
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(hkdf_sha256_derives_requested_material) try {
   const auto material =
       forge::crypto::symmetric::kdf::derive_hkdf_sha256(forge::crypto::symmetric::kdf::hkdf_sha256_request{
           .secret = {'s', 'e', 'c', 'r', 'e', 't'},
           .salt = {'s', 'a', 'l', 't'},
           .info = {'i', 'n', 'f', 'o'},
           .output_size = 48,
       });

   BOOST_CHECK_EQUAL(material.size(), 48U);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(hkdf_sha256_accepts_secret_span_without_owning_copy) try {
   auto secret = forge::crypto::core::secret_bytes{forge::crypto::core::bytes{'s', 'e', 'c', 'r', 'e', 't'}};
   const auto salt = forge::crypto::core::bytes{'s', 'a', 'l', 't'};
   const auto info = forge::crypto::core::bytes{'i', 'n', 'f', 'o'};
   const auto owned =
       forge::crypto::symmetric::kdf::derive_hkdf_sha256(forge::crypto::symmetric::kdf::hkdf_sha256_request{
           .secret = {'s', 'e', 'c', 'r', 'e', 't'},
           .salt = salt,
           .info = info,
           .output_size = 48,
       });
   const auto from_span =
       forge::crypto::symmetric::kdf::derive_hkdf_sha256(forge::crypto::symmetric::kdf::hkdf_sha256_span_request{
           .secret = secret.span(),
           .salt = salt,
           .info = info,
           .output_size = 48,
       });

   BOOST_CHECK_EQUAL_COLLECTIONS(from_span.begin(), from_span.end(), owned.begin(), owned.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(scrypt_derives_requested_material) try {
   const auto salt = forge::crypto::core::bytes{'s', 'a', 'l', 't'};
   const auto material = forge::crypto::symmetric::kdf::derive_scrypt(forge::crypto::symmetric::kdf::scrypt_request{
       .password = "correct horse battery staple",
       .salt = salt,
       .n = 1024,
       .r = 8,
       .p = 1,
       .max_memory_bytes = 8ULL * 1024ULL * 1024ULL,
       .output_size = 32,
   });
   auto password = forge::crypto::core::secret_bytes{
       forge::crypto::core::bytes{'c', 'o', 'r', 'r', 'e', 'c', 't', ' ', 'h', 'o', 'r', 's', 'e', ' ',
                                  'b', 'a', 't', 't', 'e', 'r', 'y', ' ', 's', 't', 'a', 'p', 'l', 'e'},
   };
   const auto from_span =
       forge::crypto::symmetric::kdf::derive_scrypt(forge::crypto::symmetric::kdf::scrypt_span_request{
           .password = password.span(),
           .salt = salt,
           .n = 1024,
           .r = 8,
           .p = 1,
           .max_memory_bytes = 8ULL * 1024ULL * 1024ULL,
           .output_size = 32,
       });

   BOOST_CHECK_EQUAL(material.size(), 32U);
   BOOST_CHECK_EQUAL_COLLECTIONS(from_span.begin(), from_span.end(), material.begin(), material.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(secret_bytes_is_move_only_and_explicitly_clearable) try {
   static_assert(!std::is_copy_constructible_v<forge::crypto::core::secret_bytes>);
   static_assert(!std::is_copy_assignable_v<forge::crypto::core::secret_bytes>);
   static_assert(std::is_move_constructible_v<forge::crypto::core::secret_bytes>);
   static_assert(std::is_move_assignable_v<forge::crypto::core::secret_bytes>);

   auto secret = forge::crypto::core::secret_bytes{forge::crypto::core::bytes{'s', 'e', 'c', 'r', 'e', 't'}};
   const auto expected = forge::crypto::core::bytes{'s', 'e', 'c', 'r', 'e', 't'};
   BOOST_TEST(secret.size() == 6U);
   BOOST_TEST(!secret.empty());
   BOOST_CHECK_EQUAL_COLLECTIONS(secret.span().begin(), secret.span().end(), expected.begin(), expected.end());

   auto moved = std::move(secret);
   BOOST_TEST(moved.size() == 6U);
   BOOST_TEST(secret.empty());

   moved.clear();
   BOOST_TEST(moved.empty());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_gcm_roundtrips_with_aad) try {
   auto key = forge::crypto::symmetric::aes::aes256_key{};
   std::fill(key.bytes.begin(), key.bytes.end(), std::uint8_t{0x42});

   auto encrypted =
       forge::crypto::symmetric::aes::encrypt_aes256_gcm(forge::crypto::symmetric::aes::aes256_gcm_encrypt_request{
           .key = key,
           .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
           .plaintext = {'p', 'a', 'y', 'l', 'o', 'a', 'd'},
           .aad = {'m', 'e', 't', 'a'},
       });

   BOOST_CHECK_EQUAL(encrypted.nonce.size(), forge::crypto::symmetric::aes::aes_gcm_nonce_size);
   BOOST_CHECK_EQUAL(encrypted.tag.size(), forge::crypto::symmetric::aes::aes_gcm_tag_size);
   const auto expected = forge::crypto::core::bytes{'p', 'a', 'y', 'l', 'o', 'a', 'd'};
   BOOST_CHECK(encrypted.ciphertext != expected);

   const auto plaintext =
       forge::crypto::symmetric::aes::decrypt_aes256_gcm(forge::crypto::symmetric::aes::aes256_gcm_decrypt_request{
           .key = key,
           .encrypted = encrypted,
           .aad = {'m', 'e', 't', 'a'},
       });

   BOOST_CHECK_EQUAL_COLLECTIONS(plaintext.begin(), plaintext.end(), expected.begin(), expected.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_gcm_rejects_bad_tag) try {
   const auto key = forge::crypto::symmetric::aes::generate_aes256_key();
   auto encrypted =
       forge::crypto::symmetric::aes::encrypt_aes256_gcm(forge::crypto::symmetric::aes::aes256_gcm_encrypt_request{
           .key = key,
           .nonce = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11},
           .plaintext = {'p', 'a', 'y', 'l', 'o', 'a', 'd'},
           .aad = {'m', 'e', 't', 'a'},
       });

   encrypted.tag.front() ^= std::uint8_t{0x01};

   const auto decrypt_with_bad_tag = [&] {
      (void)forge::crypto::symmetric::aes::decrypt_aes256_gcm(forge::crypto::symmetric::aes::aes256_gcm_decrypt_request{
          .key = key,
          .encrypted = encrypted,
          .aad = {'m', 'e', 't', 'a'},
      });
   };

   BOOST_CHECK_EXCEPTION(decrypt_with_bad_tag(), forge::crypto::symmetric::aes::exceptions::authentication_failed,
                         [](const forge::crypto::symmetric::aes::exceptions::authentication_failed& error) {
                            return error.code().category().name() == std::string_view{"forge.crypto.symmetric.aes"};
                         });
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_gcm_streaming_encoder_matches_one_shot_chunks) try {
   const auto key = forge::crypto::symmetric::aes::generate_aes256_key();
   const auto nonce = forge::crypto::core::bytes{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
   const auto aad = forge::crypto::core::bytes{'m', 'e', 't', 'a'};
   const auto plaintext = forge::crypto::core::bytes{'s', 't', 'r', 'e', 'a', 'm', 'i', 'n', 'g'};

   auto streaming_ciphertext = forge::crypto::core::bytes{};
   auto encoder =
       forge::crypto::symmetric::aes::aes256_gcm_encoder{forge::crypto::symmetric::aes::aes256_gcm_encoder_options{
           .key = key,
           .nonce = nonce,
           .aad = aad,
           .ciphertext_sink =
               [&](std::span<const std::uint8_t> chunk) {
                  streaming_ciphertext.insert(streaming_ciphertext.end(), chunk.begin(), chunk.end());
               },
       }};

   encoder.write(std::span<const std::uint8_t>{plaintext.data(), 3});
   encoder.write(std::span<const std::uint8_t>{plaintext.data() + 3, plaintext.size() - 3});
   const auto streaming_auth = encoder.finalize();

   const auto one_shot = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = key,
       .nonce = nonce,
       .plaintext = plaintext,
       .aad = aad,
   });

   BOOST_CHECK_EQUAL_COLLECTIONS(streaming_ciphertext.begin(), streaming_ciphertext.end(), one_shot.ciphertext.begin(),
                                 one_shot.ciphertext.end());
   BOOST_CHECK_EQUAL_COLLECTIONS(streaming_auth.tag.begin(), streaming_auth.tag.end(), one_shot.tag.begin(),
                                 one_shot.tag.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_gcm_streaming_encoder_accepts_raw_pack) try {
   const auto key = forge::crypto::symmetric::aes::generate_aes256_key();
   const auto nonce = forge::crypto::core::bytes{11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
   const auto aad = forge::crypto::core::bytes{'r', 'a', 'w'};
   const auto value = encrypted_record{.id = 42, .name = "packed"};

   auto streaming_ciphertext = forge::crypto::core::bytes{};
   auto encoder =
       forge::crypto::symmetric::aes::aes256_gcm_encoder{forge::crypto::symmetric::aes::aes256_gcm_encoder_options{
           .key = key,
           .nonce = nonce,
           .aad = aad,
           .ciphertext_sink =
               [&](std::span<const std::uint8_t> chunk) {
                  streaming_ciphertext.insert(streaming_ciphertext.end(), chunk.begin(), chunk.end());
               },
       }};

   forge::raw::pack(encoder, value);
   const auto streaming_auth = encoder.finalize();

   const auto packed = forge::raw::pack(value);
   const auto one_shot = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = key,
       .nonce = nonce,
       .plaintext = forge::crypto::core::bytes{packed.begin(), packed.end()},
       .aad = aad,
   });

   BOOST_CHECK_EQUAL_COLLECTIONS(streaming_ciphertext.begin(), streaming_ciphertext.end(), one_shot.ciphertext.begin(),
                                 one_shot.ciphertext.end());
   BOOST_CHECK_EQUAL_COLLECTIONS(streaming_auth.tag.begin(), streaming_auth.tag.end(), one_shot.tag.begin(),
                                 one_shot.tag.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_gcm_streaming_decoder_emits_provisional_plaintext_until_finalize) try {
   const auto key = forge::crypto::symmetric::aes::generate_aes256_key();
   const auto aad = forge::crypto::core::bytes{'m', 'e', 't', 'a'};
   const auto expected = forge::crypto::core::bytes{'p', 'r', 'o', 'v', 'i', 's', 'i', 'o', 'n', 'a', 'l'};
   auto encrypted = forge::crypto::symmetric::aes::encrypt_aes256_gcm({
       .key = key,
       .nonce = {1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144},
       .plaintext = expected,
       .aad = aad,
   });

   auto provisional = forge::crypto::core::bytes{};
   auto decoder =
       forge::crypto::symmetric::aes::aes256_gcm_decoder{forge::crypto::symmetric::aes::aes256_gcm_decoder_options{
           .key = key,
           .nonce = encrypted.nonce,
           .tag = encrypted.tag,
           .aad = aad,
           .plaintext_sink =
               [&](std::span<const std::uint8_t> chunk) {
                  provisional.insert(provisional.end(), chunk.begin(), chunk.end());
               },
       }};

   decoder.write(std::span<const std::uint8_t>{encrypted.ciphertext.data(), 4});
   BOOST_CHECK(!provisional.empty());
   decoder.write(std::span<const std::uint8_t>{encrypted.ciphertext.data() + 4, encrypted.ciphertext.size() - 4});
   decoder.finalize();

   BOOST_CHECK_EQUAL_COLLECTIONS(provisional.begin(), provisional.end(), expected.begin(), expected.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(aes256_cbc_roundtrips_compatibility_payload) try {
   auto key = forge::crypto::symmetric::aes::aes256_key{};
   std::fill(key.bytes.begin(), key.bytes.end(), std::uint8_t{0x24});

   auto encrypted =
       forge::crypto::symmetric::aes::encrypt_aes256_cbc(forge::crypto::symmetric::aes::aes256_cbc_encrypt_request{
           .key = key,
           .iv = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15},
           .plaintext = {'c', 'o', 'm', 'p', 'a', 't'},
       });

   BOOST_CHECK_EQUAL(encrypted.iv.size(), forge::crypto::symmetric::aes::aes_cbc_iv_size);
   const auto expected = forge::crypto::core::bytes{'c', 'o', 'm', 'p', 'a', 't'};
   BOOST_CHECK(encrypted.ciphertext != expected);

   const auto plaintext =
       forge::crypto::symmetric::aes::decrypt_aes256_cbc(forge::crypto::symmetric::aes::aes256_cbc_decrypt_request{
           .key = key,
           .encrypted = encrypted,
       });

   BOOST_CHECK_EQUAL_COLLECTIONS(plaintext.begin(), plaintext.end(), expected.begin(), expected.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_matches_independent_botan_full_vector) try {
   // Botan 9bce628f src/tests/data/stream/salsa20.vec. The 24-byte ECRYPT nonce selects XSalsa20.
   const auto key = xsalsa20_official_key();
   const auto nonce = xsalsa20_official_nonce();
   const auto expected = std::array<std::uint8_t, 139>{
       0xee, 0xa6, 0xa7, 0x25, 0x1c, 0x1e, 0x72, 0x91, 0x6d, 0x11, 0xc2, 0xcb, 0x21, 0x4d, 0x3c, 0x25,
       0x25, 0x39, 0x12, 0x1d, 0x8e, 0x23, 0x4e, 0x65, 0x2d, 0x65, 0x1f, 0xa4, 0xc8, 0xcf, 0xf8, 0x80,
       0x30, 0x9e, 0x64, 0x5a, 0x74, 0xe9, 0xe0, 0xa6, 0x0d, 0x82, 0x43, 0xac, 0xd9, 0x17, 0x7a, 0xb5,
       0x1a, 0x1b, 0xeb, 0x8d, 0x5a, 0x2f, 0x5d, 0x70, 0x0c, 0x09, 0x3c, 0x5e, 0x55, 0x85, 0x57, 0x96,
       0x25, 0x33, 0x7b, 0xd3, 0xab, 0x61, 0x9d, 0x61, 0x57, 0x60, 0xd8, 0xc5, 0xb2, 0x24, 0xa8, 0x5b,
       0x1d, 0x0e, 0xfe, 0x0e, 0xb8, 0xa7, 0xee, 0x16, 0x3a, 0xbb, 0x03, 0x76, 0x52, 0x9f, 0xcc, 0x09,
       0xba, 0xb5, 0x06, 0xc6, 0x18, 0xe1, 0x3c, 0xe7, 0x77, 0xd8, 0x2c, 0x3a, 0xe9, 0xd1, 0xa6, 0xf9,
       0x72, 0xd4, 0x16, 0x02, 0x87, 0xcb, 0xfe, 0x60, 0xbf, 0x21, 0x30, 0xfc, 0x0a, 0x6f, 0xf6, 0x04,
       0x9d, 0x0a, 0x5c, 0x8a, 0x82, 0xf4, 0x29, 0x23, 0x1f, 0x00, 0x80,
   };
   const auto input = forge::crypto::core::bytes(expected.size(), std::uint8_t{0});

   const auto output = forge::crypto::symmetric::xsalsa20::transform(key, nonce, input);

   BOOST_CHECK_EQUAL_COLLECTIONS(output.begin(), output.end(), expected.begin(), expected.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_stream_matches_one_shot_across_chunk_boundaries) try {
   const auto key = xsalsa20_official_key();
   const auto nonce = xsalsa20_official_nonce();
   auto input = forge::crypto::core::bytes(257);
   for (auto index = std::size_t{0}; index < input.size(); ++index) {
      input[index] = static_cast<std::uint8_t>(index);
   }

   const auto one_shot = forge::crypto::symmetric::xsalsa20::transform(key, nonce, input);
   auto streaming = input;
   auto stream = forge::crypto::symmetric::xsalsa20::stream{key, nonce};
   const auto chunk_sizes = std::array<std::size_t, 7>{1, 63, 2, 64, 61, 3, 63};
   auto offset = std::size_t{0};
   for (const auto chunk_size : chunk_sizes) {
      stream.transform(std::span<std::uint8_t>{streaming.data() + offset, chunk_size});
      offset += chunk_size;
   }

   BOOST_REQUIRE_EQUAL(offset, streaming.size());
   BOOST_CHECK_EQUAL_COLLECTIONS(streaming.begin(), streaming.end(), one_shot.begin(), one_shot.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_stream_roundtrips_across_independent_io_chunks) try {
   const auto key = xsalsa20_official_key();
   const auto nonce = xsalsa20_official_nonce();
   const auto plaintext = forge::crypto::core::bytes{
       'c', 'h', 'u', 'n', 'k', '-', 's', 'a', 'f', 'e', '-', 's', 't', 'r', 'e', 'a', 'm', '-', 'p', 'a', 'y', 'l', 'o', 'a', 'd',
   };
   auto ciphertext = plaintext;
   auto encryptor = forge::crypto::symmetric::xsalsa20::stream{key, nonce};
   encryptor.transform(std::span<std::uint8_t>{ciphertext.data(), 5});
   encryptor.transform(std::span<std::uint8_t>{ciphertext.data() + 5, ciphertext.size() - 5});

   auto decrypted = ciphertext;
   auto decryptor = forge::crypto::symmetric::xsalsa20::stream{key, nonce};
   decryptor.transform(std::span<std::uint8_t>{decrypted.data(), 1});
   decryptor.transform(std::span<std::uint8_t>{decrypted.data() + 1, 17});
   decryptor.transform(std::span<std::uint8_t>{decrypted.data() + 18, decrypted.size() - 18});

   BOOST_CHECK_EQUAL_COLLECTIONS(decrypted.begin(), decrypted.end(), plaintext.begin(), plaintext.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_private_vendor_bridge_matches_libsodium_high_counter_vector) try {
   // libsodium 1.0.22 test/default/stream.exp line 67.
   const auto key = xsalsa20_official_key();
   const auto nonce = xsalsa20_official_nonce();
   auto actual = std::array<std::uint8_t, 192>{};
   const auto expected = std::array<std::uint8_t, 192>{
       0xb4, 0x6a, 0xf0, 0xbf, 0x76, 0x1b, 0x78, 0x53, 0x3e, 0x01, 0xa0, 0xdd, 0x7e, 0x07, 0x21, 0x6c,
       0x97, 0x10, 0xef, 0x35, 0xf0, 0x9a, 0x28, 0xd1, 0xe5, 0xfa, 0x46, 0x9b, 0x60, 0x24, 0x72, 0xca,
       0x50, 0x85, 0xf6, 0xdb, 0xcc, 0x6a, 0x6b, 0x51, 0xfb, 0x89, 0x98, 0x6f, 0x8f, 0xec, 0xa8, 0x56,
       0x58, 0xd0, 0x57, 0x01, 0xf5, 0x67, 0x7d, 0x0b, 0xb3, 0x40, 0xa1, 0xf2, 0xc7, 0x69, 0x54, 0x72,
       0x19, 0xf5, 0x42, 0x0c, 0x62, 0xff, 0xff, 0x7d, 0x13, 0x04, 0xda, 0xd8, 0x2b, 0x6d, 0xec, 0x2b,
       0xdc, 0x59, 0xec, 0x12, 0xa9, 0xe1, 0x8a, 0x77, 0x4e, 0xed, 0x12, 0x8c, 0x2c, 0x90, 0x61, 0x0a,
       0x9d, 0x4c, 0x75, 0xc0, 0x81, 0x7d, 0x64, 0x81, 0x7a, 0x76, 0xbb, 0xc1, 0x27, 0x46, 0x97, 0x1a,
       0xe8, 0x97, 0xaf, 0x21, 0x0a, 0x07, 0x2c, 0x1b, 0xc9, 0xfb, 0x04, 0x4e, 0x08, 0x6b, 0x7b, 0xfe,
       0x85, 0xfa, 0xd9, 0x5d, 0x5c, 0x2b, 0xbb, 0x28, 0xc1, 0x2d, 0xe5, 0x75, 0x5b, 0x1c, 0xcd, 0xe6,
       0x3e, 0x93, 0xcc, 0x89, 0x2b, 0x4d, 0x2b, 0xcb, 0xd7, 0xdc, 0x07, 0x06, 0xb0, 0x94, 0xc2, 0x49,
       0x2e, 0x32, 0x9e, 0x3b, 0x9a, 0x98, 0xa9, 0xcb, 0xc7, 0xd0, 0x10, 0x31, 0xcf, 0x1d, 0x58, 0x61,
       0xf5, 0x76, 0xe1, 0x29, 0x1d, 0xf6, 0x28, 0x6c, 0x28, 0x14, 0x6b, 0x0b, 0x4d, 0xf9, 0xad, 0x44,
   };
   constexpr auto counter = (1ULL << 32U) - 1ULL;

   BOOST_REQUIRE_EQUAL(forge_xsalsa20_vendor_xor_ic(actual.data(), actual.data(),
                                                     static_cast<unsigned long long>(actual.size()), nonce.bytes.data(),
                                                     counter, key.span().data()),
                       0);
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), expected.begin(), expected.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_counter_state_preserves_last_block_and_rejected_capacity_state) try {
   using forge::crypto::symmetric::xsalsa20::detail::counter_state;
   using forge::crypto::symmetric::xsalsa20::detail::counter_position;

   const auto initial = counter_state{};
   BOOST_CHECK_EQUAL(initial.next_block_counter(), 0U);
   BOOST_CHECK_EQUAL(initial.block_offset(), xsalsa20::block_size);
   BOOST_CHECK(!initial.exhausted());

   const auto final_counter = std::numeric_limits<std::uint64_t>::max();
   auto position = counter_position{
       .next_block_counter = final_counter,
       .block_offset = xsalsa20::detail::counter_block_size,
       .exhausted = false,
   };
   BOOST_CHECK(xsalsa20::detail::requires_block(position));

   xsalsa20::detail::require_capacity(position, xsalsa20::detail::counter_block_size);
   position = xsalsa20::detail::advance_after_block_load(position);
   BOOST_CHECK(!xsalsa20::detail::requires_block(position));
   BOOST_CHECK(position.exhausted);
   BOOST_CHECK_EQUAL(position.next_block_counter, final_counter);

   position = xsalsa20::detail::advance_after_consume(position, 17U);
   BOOST_CHECK_EQUAL(position.block_offset, 17U);
   xsalsa20::detail::require_capacity(position, 47U);

   const auto position_before_rejection = position;
   const auto reject_capacity = [&] { xsalsa20::detail::require_capacity(position, 48U); };
   BOOST_CHECK_THROW(reject_capacity(), xsalsa20::exceptions::counter_exhausted);
   BOOST_CHECK_EQUAL(position.next_block_counter, position_before_rejection.next_block_counter);
   BOOST_CHECK_EQUAL(position.block_offset, position_before_rejection.block_offset);
   BOOST_CHECK_EQUAL(position.exhausted, position_before_rejection.exhausted);

   position = xsalsa20::detail::advance_after_consume(position, 47U);
   const auto reject_next_byte = [&] { xsalsa20::detail::require_capacity(position, 1U); };
   BOOST_CHECK_THROW(reject_next_byte(), xsalsa20::exceptions::counter_exhausted);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_secret_types_and_stream_state_are_move_only) try {
   using forge::crypto::symmetric::xsalsa20::key;
   using forge::crypto::symmetric::xsalsa20::stream;

   static_assert(!std::is_copy_constructible_v<key>);
   static_assert(!std::is_copy_assignable_v<key>);
   static_assert(std::is_move_constructible_v<key>);
   static_assert(!std::is_copy_constructible_v<stream>);
   static_assert(!std::is_copy_assignable_v<stream>);
   static_assert(std::is_move_constructible_v<stream>);

   auto source_key = xsalsa20_official_key();
   auto key = std::move(source_key);
   BOOST_CHECK(source_key.span().empty());

   const auto nonce = xsalsa20_official_nonce();
   auto expected = forge::crypto::core::bytes{'m', 'o', 'v', 'e', '-', 's', 't', 'a', 't', 'e'};
   auto continued = expected;
   auto source_stream = stream{key, nonce};
   source_stream.transform(std::span<std::uint8_t>{continued.data(), 3});
   auto stream = std::move(source_stream);
   stream.transform(std::span<std::uint8_t>{continued.data() + 3, continued.size() - 3});

   const auto one_shot = forge::crypto::symmetric::xsalsa20::transform(key, nonce, expected);
   BOOST_CHECK_EQUAL_COLLECTIONS(continued.begin(), continued.end(), one_shot.begin(), one_shot.end());
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(xsalsa20_rejects_dynamic_key_and_nonce_sizes) try {
   const auto short_key = forge::crypto::core::bytes(forge::crypto::symmetric::xsalsa20::key_size - 1U, 0U);
   const auto short_nonce = forge::crypto::core::bytes(forge::crypto::symmetric::xsalsa20::nonce_size - 1U, 0U);

   const auto construct_key = [&] { return forge::crypto::symmetric::xsalsa20::key{short_key}; };
   const auto construct_nonce = [&] { return forge::crypto::symmetric::xsalsa20::make_nonce(short_nonce); };
   BOOST_CHECK_THROW(construct_key(), forge::crypto::symmetric::xsalsa20::exceptions::invalid_key);
   BOOST_CHECK_THROW(construct_nonce(), forge::crypto::symmetric::xsalsa20::exceptions::invalid_nonce);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(salsa20_matches_libsodium_stream2_golden) {
   namespace salsa20 = forge::crypto::symmetric::salsa20;
   // Vendored libsodium test/default/stream2.c and stream2.exp.
   const auto key = salsa20::key{std::array<std::uint8_t, 32>{
       0xdc, 0x90, 0x8d, 0xda, 0x0b, 0x93, 0x44, 0xa9, 0x53, 0x62, 0x9b, 0x73, 0x38, 0x20, 0x77, 0x88,
       0x80, 0xf3, 0xce, 0xb4, 0x21, 0xbb, 0x61, 0xb9, 0x1c, 0xbd, 0x4c, 0x3e, 0x66, 0x25, 0x6c, 0xe4}};
   const auto nonce = salsa20::make_nonce(
       std::array<std::uint8_t, 8>{0x82, 0x19, 0xe0, 0x03, 0x6b, 0x7a, 0x0b, 0x37});
   const auto output = forge::crypto::core::secret_bytes{salsa20::keystream(key, nonce, 4'194'304)};
   auto hash = forge::crypto::digest::sha256::encoder{};
   hash.write(output.span());
   BOOST_CHECK_EQUAL(hash.result().str(), "662b9d0e3463029156069b12f918691a98f7dfb2ca0393c96bbfc6b1fbd630a2");

   for (const auto size : {0U, 1U, 63U, 64U, 65U, 127U, 128U, 129U}) {
      const auto prefix = forge::crypto::core::secret_bytes{salsa20::keystream(key, nonce, size)};
      BOOST_CHECK_EQUAL(prefix.size(), size);
      BOOST_CHECK(std::equal(prefix.span().begin(), prefix.span().end(), output.span().begin()));
   }
}

BOOST_AUTO_TEST_CASE(salsa20_rejects_invalid_sizes_and_moved_keys) {
   namespace salsa20 = forge::crypto::symmetric::salsa20;
   static_assert(!std::is_copy_constructible_v<salsa20::key>);
   static_assert(!std::is_copy_assignable_v<salsa20::key>);
   static_assert(std::is_nothrow_move_constructible_v<salsa20::key>);
   static_assert(std::is_nothrow_move_assignable_v<salsa20::key>);
   for (const auto size : {0U, 31U, 33U}) {
      const auto bytes = forge::crypto::core::bytes(size);
      BOOST_CHECK_THROW(salsa20::key{bytes}, salsa20::exceptions::invalid_key);
   }
   for (const auto size : {0U, 7U, 9U}) {
      const auto bytes = forge::crypto::core::bytes(size);
      BOOST_CHECK_THROW(static_cast<void>(salsa20::make_nonce(bytes)), salsa20::exceptions::invalid_nonce);
   }
   auto source = salsa20::key{std::array<std::uint8_t, 32>{}};
   const auto nonce = salsa20::nonce{};
   const auto expected = forge::crypto::core::secret_bytes{salsa20::keystream(source, nonce, 65)};
   auto moved = std::move(source);
   BOOST_CHECK(source.span().empty());
   BOOST_CHECK_THROW(static_cast<void>(salsa20::keystream(source, nonce, 0)), salsa20::exceptions::invalid_key);
   auto destination = salsa20::key{std::array<std::uint8_t, 32>{1}};
   destination = std::move(moved);
   BOOST_CHECK(moved.span().empty());
   BOOST_CHECK_THROW(static_cast<void>(salsa20::keystream(moved, nonce, 1)), salsa20::exceptions::invalid_key);
   const auto actual = forge::crypto::core::secret_bytes{salsa20::keystream(destination, nonce, 65)};
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.span().begin(), actual.span().end(),
                                 expected.span().begin(), expected.span().end());
   BOOST_CHECK_THROW(static_cast<void>(salsa20::keystream(destination, nonce,
                                                          std::numeric_limits<std::size_t>::max())),
                     salsa20::exceptions::invalid_size);
}

BOOST_AUTO_TEST_SUITE_END()
