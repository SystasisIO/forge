module;

#if !defined(FORGE_CONTRACT_GUEST)
#include <forge/exceptions/macros.hpp>
#include <boost/describe.hpp>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#endif

export module forge.crypto.asymmetric;

export import forge.crypto.asymmetric.values;

#if !defined(FORGE_CONTRACT_GUEST)
export import forge.crypto.asymmetric.serialization;

import forge.core.utility;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.asymmetric.p256;
import forge.crypto.asymmetric.rsa;
import forge.crypto.asymmetric.secp256k1;
import forge.crypto.digest.sha256;
export import forge.exceptions;
import forge.raw.raw;
import forge.variant.chrono;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.exceptions;
import forge.variant.format;
import forge.variant.multiprecision;
import forge.variant.static_variant;
import forge.variant.value;

export namespace forge::crypto::asymmetric {

namespace exceptions {

enum class code : std::uint16_t {
   invalid_key = 1,
   invalid_options = 2,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.asymmetric")

using invalid_key = forge::exceptions::coded_exception<code, code::invalid_key>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;

} // namespace exceptions

BOOST_DESCRIBE_ENUM(algorithm, secp256k1, p256, webauthn, ed25519, rsa)

class private_key {
 public:
   using storage_type = std::variant<secp256k1::private_key, p256::private_key, ed25519::private_key, rsa::private_key>;

   private_key() = default;
   private_key(private_key&&) = default;
   private_key(const private_key&) = default;
   private_key& operator=(private_key&&) = default;
   private_key& operator=(const private_key&) = default;

   explicit private_key(storage_type value) : _storage(std::move(value)) {}

   [[nodiscard]] algorithm type() const noexcept;
   [[nodiscard]] public_key get_public_key() const;
   [[nodiscard]] signature sign(std::span<const std::uint8_t> message) const;
   [[nodiscard]] signature sign_digest(const digest::sha256& digest) const;
   [[nodiscard]] const storage_type& storage() const noexcept {
      return _storage;
   }

   template <typename T> [[nodiscard]] const T& as() const {
      return std::get<T>(_storage);
   }

   template <typename Visitor> decltype(auto) visit(Visitor&& visitor) const {
      return std::visit(std::forward<Visitor>(visitor), _storage);
   }

   template <typename KeyType = secp256k1::private_key> [[nodiscard]] static private_key generate() {
      return private_key{storage_type{KeyType::generate()}};
   }

   template <typename KeyType = p256::private_key> [[nodiscard]] static private_key generate_p256() {
      return private_key{storage_type{KeyType::generate()}};
   }

   template <typename KeyType = secp256k1::private_key>
   [[nodiscard]] static private_key regenerate(typename KeyType::data_type value) {
      return private_key{storage_type{KeyType::regenerate(std::move(value))}};
   }

 private:
   storage_type _storage;

   friend bool operator==(const private_key&, const private_key&);
   friend bool operator<(const private_key&, const private_key&);
};

[[nodiscard]] bool valid(const public_key& key);
[[nodiscard]] bool verify(const public_key& key, std::span<const std::uint8_t> message, const signature& value);
[[nodiscard]] public_key recover(const signature& value, const digest::sha256& digest, bool check_canonical = true);
[[nodiscard]] std::size_t hash_value(const signature& value);

std::ostream& operator<<(std::ostream& stream, const public_key& key);

enum class checksum_scheme {
   none,
   ripemd160,
   ripemd160_with_text_suffix,
   single_sha256,
   double_sha256,
};

enum class checksum_payload {
   raw_payload,
   encoded_payload,
};

enum class text_codec {
   base58,
   hex,
};

struct checksum_options {
   checksum_scheme scheme = checksum_scheme::none;
   checksum_payload payload = checksum_payload::raw_payload;
   std::string text_suffix;
};

struct text_encoding_rule {
   algorithm type = algorithm::secp256k1;
   std::string text_prefix;
   text_codec codec = text_codec::base58;
   std::vector<std::uint8_t> binary_prefix;
   std::vector<std::uint8_t> binary_suffix;
   checksum_options checksum;
   bool parse = true;
   bool format = true;
};

struct text_encoding_profile {
   std::string id;
   std::vector<text_encoding_rule> private_keys;
   std::vector<text_encoding_rule> public_keys;
   std::vector<text_encoding_rule> signatures;
};

namespace profiles {
[[nodiscard]] const text_encoding_profile& forge();
[[nodiscard]] const text_encoding_profile& antelope();
[[nodiscard]] const text_encoding_profile& bitcoin();
[[nodiscard]] const text_encoding_profile& solana();
[[nodiscard]] const text_encoding_profile& tezos();
} // namespace profiles

class encoding {
 public:
   [[nodiscard]] static const encoding& forge();
   [[nodiscard]] static const encoding& antelope();
   [[deprecated("Use encoding::antelope()")]] [[nodiscard]] static const encoding& eos();
   [[nodiscard]] static encoding custom(text_encoding_profile profile);
   [[nodiscard]] static encoding from_profile(const text_encoding_profile& profile);

   [[nodiscard]] const std::string& id() const noexcept;
   [[nodiscard]] const text_encoding_profile& profile() const noexcept;

   [[nodiscard]] public_key parse_public(std::string_view text) const;
   [[nodiscard]] private_key parse_private(std::string_view text) const;
   [[nodiscard]] signature parse_signature(std::string_view text) const;

   [[nodiscard]] std::string format(const public_key& key) const;
   [[nodiscard]] std::string format(const private_key& key) const;
   [[nodiscard]] std::string format(const signature& value) const;

 private:
   explicit encoding(text_encoding_profile profile);

   text_encoding_profile profile_;
};

void to_variant(const private_key& value, forge::variant& output,
                const forge::yield_function_t& yield = forge::yield_function_t());
void from_variant(const forge::variant& value, private_key& output);

[[nodiscard]] constexpr bool diagnostic_is_secret(const private_key&) noexcept {
   return true;
}

} // namespace forge::crypto::asymmetric

export namespace forge::raw {
template <> struct enum_wire_type<forge::crypto::asymmetric::algorithm> {
   using type = std::int32_t;
};
} // namespace forge::raw
#endif
