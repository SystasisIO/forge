module;

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module forge.crypto.bls;

export import forge.crypto.bls.exceptions;
export import forge.crypto.bls.serialization;

import forge.variant.chrono;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.exceptions;
import forge.variant.format;
import forge.variant.multiprecision;
import forge.variant.value;

export namespace forge::crypto::bls {

class private_key;

namespace encoding {

[[nodiscard]] private_key parse_private_key(std::string_view text);

[[nodiscard]] std::string format(const private_key& value);

} // namespace encoding

class private_key {
 public:
   private_key() = default;
   ~private_key();

   private_key(const private_key&) = delete;
   private_key& operator=(const private_key&) = delete;
   private_key(private_key&& other) noexcept;
   private_key& operator=(private_key&& other) noexcept;

   explicit private_key(std::span<const std::uint8_t> seed);

   [[nodiscard]] public_key get_public_key() const;
   [[nodiscard]] signature sign(std::span<const std::uint8_t> message) const;
   [[nodiscard]] signature proof_of_possession() const;
   [[nodiscard]] static private_key generate();

   friend bool operator==(const private_key&, const private_key&) = default;

 private:
   std::array<std::uint64_t, 4> _secret{};

   friend private_key encoding::parse_private_key(std::string_view);
   friend std::string encoding::format(const private_key&);
};

struct aggregate_verification_group;

class proof_verified_public_key {
 public:
   proof_verified_public_key(const proof_verified_public_key& other);
   proof_verified_public_key(proof_verified_public_key&& other) noexcept;
   proof_verified_public_key& operator=(const proof_verified_public_key& other);
   proof_verified_public_key& operator=(proof_verified_public_key&& other) noexcept;
   ~proof_verified_public_key();

   [[nodiscard]] const public_key& get() const noexcept {
      return key_;
   }

 private:
   struct impl;

   proof_verified_public_key(public_key key, std::unique_ptr<impl> implementation);

   public_key key_;
   std::unique_ptr<impl> impl_;

   friend bool verify(const proof_verified_public_key&, std::span<const std::uint8_t>, const signature&) noexcept;
   friend std::optional<proof_verified_public_key> verify_proof_of_possession(const public_key&,
                                                                              const signature&) noexcept;
   friend bool verify_grouped(std::span<const aggregate_verification_group>, const aggregate_signature&);
};

class signature_accumulator {
 public:
   signature_accumulator();
   signature_accumulator(const signature_accumulator& other);
   signature_accumulator(signature_accumulator&& other) noexcept;
   signature_accumulator& operator=(const signature_accumulator& other);
   signature_accumulator& operator=(signature_accumulator&& other) noexcept;
   ~signature_accumulator();

   void add(const signature& value);
   void add(const aggregate_signature& value);
   [[nodiscard]] aggregate_signature finish() const;

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

struct aggregate_verification_group {
   std::span<const proof_verified_public_key> public_keys;
   std::span<const std::uint8_t> message;
};

[[nodiscard]] bool valid(const public_key& value) noexcept;
[[nodiscard]] bool valid(const signature& value) noexcept;
[[nodiscard]] bool valid(const aggregate_signature& value) noexcept;

[[nodiscard]] bool verify(const public_key& key, std::span<const std::uint8_t> message,
                          const signature& value) noexcept;
[[nodiscard]] bool verify(const proof_verified_public_key& key, std::span<const std::uint8_t> message,
                          const signature& value) noexcept;
[[nodiscard]] std::optional<proof_verified_public_key> verify_proof_of_possession(const public_key& key,
                                                                                  const signature& proof) noexcept;
[[nodiscard]] bool verify_grouped(std::span<const aggregate_verification_group> groups,
                                  const aggregate_signature& value);

void to_variant(const private_key& value, variant& output);
void from_variant(const variant& value, private_key& output);

[[nodiscard]] constexpr bool diagnostic_is_secret(const private_key&) noexcept {
   return true;
}

} // namespace forge::crypto::bls
