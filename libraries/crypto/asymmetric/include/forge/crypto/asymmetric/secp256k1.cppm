module;
#include <forge/exceptions/macros.hpp>
#include <array>
#include <boost/describe.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <variant>
#include <vector>

export module forge.crypto.asymmetric.secp256k1;

import forge.crypto.digest.sha256;
import forge.crypto.digest.sha512;
export import forge.exceptions;
export import forge.crypto.core.types;
import forge.raw.raw;

export namespace forge::crypto::asymmetric::secp256k1 {
namespace exceptions {

enum class code : std::uint16_t {
   invalid_input = 1,
   invalid_signature = 2,
   invalid_options = 3,
   backend_error = 4,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.asymmetric.secp256k1")

using invalid_input = forge::exceptions::coded_exception<code, code::invalid_input>;
using invalid_signature = forge::exceptions::coded_exception<code, code::invalid_signature>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using backend_error = forge::exceptions::coded_exception<code, code::backend_error>;

} // namespace exceptions

namespace detail {
class public_key_impl;
class private_key_impl;
} // namespace detail

typedef forge::crypto::digest::sha256 blind_factor_type;
typedef std::array<char, 33> commitment_type;
typedef std::array<char, 33> public_key_data;
typedef forge::crypto::digest::sha256 private_key_secret;
typedef std::array<char, 65> public_key_point_data; ///< the full uncompressed secp256k1 point
typedef std::array<char, 72> signature;
using der_signature = forge::crypto::core::bytes;
typedef std::array<unsigned char, 65> compact_signature;
typedef std::array<char, 78> extended_key_data;
typedef forge::crypto::digest::sha256 blinded_hash;
typedef forge::crypto::digest::sha256 blind_signature;

/**
 *  @class public_key
 *  @brief contains only the public point of a secp256k1 key.
 */
class public_key {
 public:
   public_key();
   public_key(const public_key& k);
   ~public_key();
   //           bool verify( const forge::crypto::digest::sha256& digest, const signature& sig );
   public_key_data serialize() const;
   public_key_point_data serialize_ecc_point() const;

   operator public_key_data() const {
      return serialize();
   }

   public_key(const public_key_data& v);
   public_key(const public_key_point_data& v);
   public_key(const compact_signature& c, const forge::crypto::digest::sha256& digest, bool check_canonical = true);

   bool valid() const;

   public_key(public_key&& pk);
   public_key& operator=(public_key&& pk);
   public_key& operator=(const public_key& pk);

   inline friend bool operator==(const public_key& a, const public_key& b) {
      return a.serialize() == b.serialize();
   }
   inline friend bool operator!=(const public_key& a, const public_key& b) {
      return a.serialize() != b.serialize();
   }

   unsigned int fingerprint() const;

 private:
   friend class private_key;
   static public_key from_key_data(const public_key_data& v);
   static bool is_canonical(const compact_signature& c);
   std::unique_ptr<detail::public_key_impl> my;
};

/**
 *  @class private_key
 *  @brief a secp256k1 private key.
 */
class private_key {
 public:
   using data_type = private_key_secret;

   private_key();
   private_key(private_key&& pk);
   private_key(const private_key& pk);
   ~private_key();

   private_key& operator=(private_key&& pk);
   private_key& operator=(const private_key& pk);

   static private_key generate();
   static private_key regenerate(const forge::crypto::digest::sha256& secret);

   private_key child(const forge::crypto::digest::sha256& offset) const;

   /**
    *  This method of generation enables creating a new private key in a deterministic manner relative to
    *  an initial seed.   A public_key created from the seed can be multiplied by the offset to calculate
    *  the new public key without having to know the private key.
    */
   static private_key generate_from_seed(const forge::crypto::digest::sha256& seed,
                                         const forge::crypto::digest::sha256& offset = forge::crypto::digest::sha256());

   private_key_secret get_secret() const; // get the private key secret

   operator private_key_secret() const {
      return get_secret();
   }

   /**
    *  Given a public key, calculatse a 512 bit shared secret between that
    *  key and this private key.
    */
   forge::crypto::digest::sha512 get_shared_secret(const public_key& pub) const;

   //           signature         sign( const forge::crypto::digest::sha256& digest )const;
   compact_signature sign_digest(const forge::crypto::digest::sha256& digest, bool require_canonical = true) const {
      return sign_compact(digest, require_canonical);
   }
   compact_signature sign_compact(const forge::crypto::digest::sha256& digest, bool require_canonical = true) const;
   //           bool              verify( const forge::crypto::digest::sha256& digest, const signature& sig );

   public_key get_public_key() const;

   inline friend bool operator==(const private_key& a, const private_key& b) {
      return a.get_secret() == b.get_secret();
   }
   inline friend std::strong_ordering operator<=>(const private_key& a, const private_key& b) {
      return a.get_secret() <=> b.get_secret();
   }

   unsigned int fingerprint() const {
      return get_public_key().fingerprint();
   }

 private:
   std::unique_ptr<detail::private_key_impl> my;
};

inline bool verify_digest(const public_key& key, const digest::sha256& digest, const compact_signature& signature,
                          bool check_canonical = true) {
   return public_key(signature, digest, check_canonical).serialize() == key.serialize();
}

inline bool verify_message(const public_key& key, std::span<const std::uint8_t> message,
                           const compact_signature& signature) {
   return verify_digest(key, digest::sha256::hash(message), signature, true);
}

[[nodiscard]] der_signature sign_der(const private_key& key, std::span<const std::uint8_t> message);
[[nodiscard]] bool verify_der(const public_key& key, std::span<const std::uint8_t> message,
                              std::span<const std::uint8_t> signature);

using recover_bytes = forge::crypto::core::bytes;

recover_bytes recover(const recover_bytes& signature, const recover_bytes& digest);

[[nodiscard]] constexpr bool diagnostic_is_secret(const private_key&) noexcept {
   return true;
}

} // namespace forge::crypto::asymmetric::secp256k1
