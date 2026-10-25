#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <tuple>
#include <vector>

import forge.crypto.exchange;
import forge.crypto.digest.sha256;
import forge.crypto.digest.sha3;
import forge.raw.codec;
import forge.raw.exceptions;

namespace {

std::array<std::uint8_t, 32> fixed_key()
{
   auto value = std::array<std::uint8_t, 32>{};
   for (auto index = std::size_t{}; index < value.size(); ++index)
   {
      value[index] = static_cast<std::uint8_t>(index);
   }
   return value;
}

std::vector<std::uint8_t> opening(std::uint32_t size)
{
   auto value = std::vector<std::uint8_t>(size + 16U);
   for (auto index = std::uint32_t{}; index < size; ++index)
   {
      value[index] = static_cast<std::uint8_t>(index % 251U);
   }
   for (auto index = std::uint32_t{}; index < 16U; ++index)
   {
      value[size + index] = static_cast<std::uint8_t>(0xa0U + index);
   }
   return value;
}

forge::crypto::digest::sha256 hash(std::span<const std::uint8_t> bytes)
{
   return forge::crypto::digest::sha256::hash(bytes);
}

forge::crypto::exchange::manifest bundle(std::vector<std::uint32_t> sizes)
{
   auto value = forge::crypto::exchange::manifest{
       .version = 1,
       .context = forge::crypto::digest::sha256::hash(std::string{"fixed independent expected context"}),
       .evidence = forge::crypto::digest::sha256::hash(std::string{"fixed trusted evidence record"}),
       .key_hash = hash(fixed_key())};
   for (auto index = std::uint32_t{}; index < sizes.size(); ++index)
   {
      const auto bytes = opening(sizes[index]);
      const auto ciphertext = forge::crypto::exchange::encrypt(fixed_key(), index, bytes);
      value.blocks.push_back({.size = sizes[index], .source = hash(bytes), .ciphertext = hash(ciphertext)});
   }
   return value;
}

template <typename Exception, typename Operation> void rejects(Operation&& operation)
{
   auto rejected = false;
   try
   {
      operation();
   }
   catch (const Exception&)
   {
      rejected = true;
   }
   FORGE_ASSERT(rejected, "exchange operation did not return the exact expected failure type");
}

void donor_fixed_vectors()
{
   // Generated once with pinned PyCryptodome 3.23.0; normal tests never rewrite
   // expected bytes. Counter input is exactly BE256(counter) || key32.
   const auto expected = std::array{
       std::tuple{0U, "abcac667cb9182d8e9c5e8e3451e710ebf8892d02958e7a121931ba5a7bea6d9"},
       std::tuple{64U, "267b03702a543be422dfb99f40b8e79d0994344fe0f2c014166833fe057b6051"},
       std::tuple{65U, "6d1da0390021edc2f60fd9a75f67f77d53ab122d0c35e6f13e6424139b507be9"},
       std::tuple{1039U, "a83d772e22b976857a2f6e3e56dedb6c8e4ddf742ca1fbf10659b8d1a058d31c"}};
   for (const auto& [counter, digest] : expected)
   {
      auto preimage = std::array<std::uint8_t, 64>{};
      preimage[28] = static_cast<std::uint8_t>(counter >> 24U);
      preimage[29] = static_cast<std::uint8_t>(counter >> 16U);
      preimage[30] = static_cast<std::uint8_t>(counter >> 8U);
      preimage[31] = static_cast<std::uint8_t>(counter);
      const auto key = fixed_key();
      std::copy(key.begin(), key.end(), preimage.begin() + 32);
      FORGE_ASSERT(forge::crypto::digest::sha3::hash(reinterpret_cast<const char*>(preimage.data()),
                                                    preimage.size(), false).str() == digest,
                   "donor Keccak counter bytes differ from independent fixed oracle");
      FORGE_ASSERT(forge::crypto::digest::sha3::hash(reinterpret_cast<const char*>(preimage.data()),
                                                    preimage.size(), true).str() != digest,
                   "NIST SHA3 was accepted as donor Keccak");
   }
   const auto source = opening(2048);
   FORGE_ASSERT(hash(source).str() == "a2d2271da7d97e2846bd4e2a8ac8ac6c54d5966b4b63b9d2c410f4a001a99e88",
                "fixed plaintext/blinder input differs from independent oracle");
   for (const auto& [index, digest] : std::array{
            std::tuple{0U, "9a9961cbe1cd6674ce81670b54f78e139ed6c75e18f876b319c68c62e4cd5bfe"},
            std::tuple{1U, "3df4c83249476594bdedb30dab9ff67b6b8a3d17e07a896ee07d324bebba40f8"},
            std::tuple{15U, "15f6b1d7b7041c4383cafa5ef198281c370c2cd2b3ae851b792f408473dbc19b"}})
   {
      const auto encrypted = forge::crypto::exchange::encrypt(fixed_key(), index, source);
      FORGE_ASSERT(encrypted.size() == 2080U && hash(encrypted).str() == digest,
                   "fixed full block ciphertext differs from independent Keccak oracle");
   }
   std::cout << "PASS exchange independent Keccak BE256 counters 0/64/65/1039 and full ciphertext vectors\n";
}

void honest_blocks_and_hidden_tail()
{
   const auto key = fixed_key();
   for (const auto size : {1U, 15U, 16U, 17U, 31U, 32U, 33U, 2047U, 2048U})
   {
      const auto value = bundle({size});
      forge::crypto::exchange::validate(value);
      const auto bytes = opening(size);
      const auto witness = forge::crypto::exchange::fault{.index = 0, .ciphertext =
          forge::crypto::exchange::encrypt(key, 0, bytes)};
      const auto plaintext = forge::crypto::exchange::open(value, key, witness);
      FORGE_ASSERT(plaintext == std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + size),
                   "honest opening returned blinder/padding or changed plaintext");
      FORGE_ASSERT(!forge::crypto::exchange::proves_fault(value, key, witness),
                   "honest authenticated delivery was classified as an objective fault");
   }
   auto sizes = std::vector<std::uint32_t>(16, 2048U);
   sizes.back() = 19;
   const auto value = bundle(sizes);
   forge::crypto::exchange::validate(value);
   auto restored = std::vector<std::uint8_t>{};
   for (auto index = std::uint32_t{}; index < sizes.size(); ++index)
   {
      const auto bytes = opening(sizes[index]);
      const auto witness = forge::crypto::exchange::fault{
          .index = index, .ciphertext = forge::crypto::exchange::encrypt(key, index, bytes)};
      const auto plaintext = forge::crypto::exchange::open(value, key, witness);
      restored.insert(restored.end(), plaintext.begin(), plaintext.end());
   }
   FORGE_ASSERT(restored.size() == 15U * 2048U + 19U && restored.back() == 18U,
                "complete ordered block list omitted the final hidden plaintext block");
   const auto first = forge::crypto::exchange::encrypt(key, 0, opening(2048));
   const auto next = forge::crypto::exchange::encrypt(key, 1, opening(2048));
   FORGE_ASSERT(first != next && first == forge::crypto::exchange::encrypt(key, 0, opening(2048)),
                "block counter separation or deterministic donor pad changed");
   // A fresh key per offer remains an application rule. This primitive cannot
   // detect reuse of the same key/index in an unrelated manifest.
   std::cout << "PASS exchange all block boundaries, 16 hidden blocks, plaintext-only return and counter separation\n";
}

void malformed_witness_is_not_invalid_delivery()
{
   const auto key = fixed_key();
   const auto value = bundle({2048, 33});
   const auto valid = forge::crypto::exchange::fault{
       .index = 1, .ciphertext = forge::crypto::exchange::encrypt(key, 1, opening(33))};
   auto wrong = valid;
   wrong.ciphertext.front() ^= 1;
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, key, wrong)); });
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::open(value, key, wrong)); });
   wrong = valid;
   wrong.index = 0;
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, key, wrong)); });
   wrong.index = 2;
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, key, wrong)); });
   auto different_key = key;
   different_key.front() ^= 1;
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, different_key, valid)); });
   wrong = valid;
   wrong.ciphertext.clear();
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, key, wrong)); });
   wrong.ciphertext.resize(2081U);
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::proves_fault(value, key, wrong)); });
   std::cout << "PASS exchange malformed witness/wrong key/wrong block cannot prove invalid delivery\n";
}

void authenticated_bad_source_padding_and_length()
{
   const auto key = fixed_key();
   const auto valid = bundle({33});
   const auto original = opening(33);
   auto garbage = original;
   std::fill(garbage.begin(), garbage.end(), 0x7dU);
   auto ciphertext = forge::crypto::exchange::encrypt(key, 0, garbage);
   auto value = valid;
   value.blocks[0].ciphertext = hash(ciphertext);
   auto witness = forge::crypto::exchange::fault{.index = 0, .ciphertext = ciphertext};
   FORGE_ASSERT(forge::crypto::exchange::proves_fault(value, key, witness),
                "complete authenticated garbage ciphertext did not prove bad source");
   rejects<forge::crypto::exchange::exceptions::invalid_delivery>(
       [&] { static_cast<void>(forge::crypto::exchange::open(value, key, witness)); });

   ciphertext = forge::crypto::exchange::encrypt(key, 0, original);
   ciphertext.back() ^= 1U; // This byte decrypts to canonical zero padding.
   value = valid;
   value.blocks[0].ciphertext = hash(ciphertext);
   witness.ciphertext = ciphertext;
   FORGE_ASSERT(forge::crypto::exchange::proves_fault(value, key, witness),
                "authenticated nonzero padding did not prove invalid delivery");
   rejects<forge::crypto::exchange::exceptions::invalid_delivery>(
       [&] { static_cast<void>(forge::crypto::exchange::open(value, key, witness)); });

   ciphertext = forge::crypto::exchange::encrypt(key, 0, original);
   ciphertext.pop_back();
   value = valid;
   value.blocks[0].ciphertext = hash(ciphertext);
   witness.ciphertext = ciphertext;
   FORGE_ASSERT(forge::crypto::exchange::proves_fault(value, key, witness),
                "authenticated noncanonical ciphertext length did not prove invalid delivery");
   rejects<forge::crypto::exchange::exceptions::invalid_delivery>(
       [&] { static_cast<void>(forge::crypto::exchange::open(value, key, witness)); });
   std::cout << "PASS exchange authenticated garbage/source/padding/length faults use exact invalid_delivery\n";
}

void manifest_and_input_bounds()
{
   const auto valid = bundle({2048, 1});
   auto wrong = valid;
   wrong.version = 2;
   rejects<forge::crypto::exchange::exceptions::invalid_input>([&] { forge::crypto::exchange::validate(wrong); });
   wrong = valid;
   wrong.blocks.clear();
   rejects<forge::crypto::exchange::exceptions::invalid_input>([&] { forge::crypto::exchange::validate(wrong); });
   wrong = valid;
   wrong.blocks.resize(17, valid.blocks.front());
   rejects<forge::crypto::exchange::exceptions::invalid_input>([&] { forge::crypto::exchange::validate(wrong); });
   for (const auto size : {0U, 2049U})
   {
      wrong = valid;
      wrong.blocks.back().size = size;
      rejects<forge::crypto::exchange::exceptions::invalid_input>([&] { forge::crypto::exchange::validate(wrong); });
   }
   wrong = valid;
   wrong.blocks.front().size = 2047;
   rejects<forge::crypto::exchange::exceptions::invalid_input>([&] { forge::crypto::exchange::validate(wrong); });
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::encrypt(fixed_key(), 16, opening(1))); });
   rejects<forge::crypto::exchange::exceptions::invalid_input>(
       [&] { static_cast<void>(forge::crypto::exchange::encrypt(fixed_key(), 0, opening(2049))); });
   std::cout << "PASS exchange manifest version/count/ordered coverage/size/index bounds\n";
}

void nested_decode_preallocation_bounds()
{
   const auto value = bundle({2048, 1});
   const auto header = forge::raw::pack(std::tuple{value.version, value.context, value.evidence, value.key_hash});
   FORGE_ASSERT(header.size() == 100U, "manifest independent Raw header shape changed");
   const auto manifest_limits = forge::raw::bounded_unpack_limits{
       .allocations = {.max_container_elements = 16, .max_total_container_elements = 16, .max_bytes = 2048,
                       .first_container_elements = 16}, .max_total_bytes = 2048};
   FORGE_ASSERT(forge::raw::pack(forge::raw::unpack_bounded_exact<forge::crypto::exchange::manifest>(
                    forge::raw::pack(value), manifest_limits)) == forge::raw::pack(value),
                "accepted bounded manifest did not roundtrip");
   for (const auto prefix : {std::vector<std::uint8_t>{17}, std::vector<std::uint8_t>{0x80, 0x80, 0x40}})
   {
      auto forged = header;
      forged.insert(forged.end(), prefix.begin(), prefix.end());
      // The 1,048,576 count fits the old default container ceiling. It must
      // fail the explicit nested bound before allocation or truncated-row read.
      rejects<forge::raw::exceptions::allocation_limit>([&]
      {
         static_cast<void>(forge::raw::unpack_bounded_exact<forge::crypto::exchange::manifest>(forged, manifest_limits));
      });
   }
   const auto witness_limits = forge::raw::bounded_unpack_limits{
       .allocations = {.max_container_elements = 2080, .max_total_container_elements = 2080, .max_bytes = 2080,
                       .first_container_elements = 2080}, .max_total_bytes = 4096};
   for (const auto prefix : {std::vector<std::uint8_t>{0xa1, 0x10},
                             std::vector<std::uint8_t>{0x80, 0x80, 0x40}})
   {
      auto forged = forge::raw::pack(std::uint32_t{0});
      forged.insert(forged.end(), prefix.begin(), prefix.end());
      rejects<forge::raw::exceptions::allocation_limit>([&]
      {
         static_cast<void>(forge::raw::unpack_bounded_exact<forge::crypto::exchange::fault>(forged, witness_limits));
      });
   }
   std::cout << "PASS exchange nested Raw count17/1048576 and byte2081/1048576 rejected before allocation\n";
}

} // namespace

int main()
{
   try
   {
      donor_fixed_vectors();
      honest_blocks_and_hidden_tail();
      malformed_witness_is_not_invalid_delivery();
      authenticated_bad_source_padding_and_length();
      manifest_and_input_bounds();
      nested_decode_preallocation_bounds();
      std::cout << "Bounded crypto exchange local primitive tests PASS; TLS/source trust and key freshness external\n";
   }
   catch (const std::exception& error)
   {
      std::cerr << error.what() << '\n';
      return 1;
   }
   return 0;
}
