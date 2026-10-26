module;

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.net.p2p.identity;

import forge.variant.value;
import forge.schema.input_value;
import forge.schema.value_kind;
import forge.multiformats.types;

export namespace forge::net::p2p {

struct public_key {
   enum class type : std::uint8_t {
      rsa = 0,
      ed25519 = 1,
      secp256k1 = 2,
      ecdsa = 3,
   };

   type type;
   std::vector<std::uint8_t> data;
};

struct peer_id {
   std::string value;

   [[nodiscard]] std::string to_string() const;
   [[nodiscard]] std::string to_cid_string() const;
   [[nodiscard]] forge::multiformats::bytes to_bytes() const;

   [[nodiscard]] static peer_id from_string(std::string_view value);
   [[nodiscard]] static peer_id from_bytes(std::span<const std::uint8_t> value);

   [[nodiscard]] friend bool operator==(const peer_id&, const peer_id&) noexcept = default;
   [[nodiscard]] friend auto operator<=>(const peer_id&, const peer_id&) noexcept = default;
};

using public_key_resolver = std::function<std::optional<public_key>(const peer_id&)>;

[[nodiscard]] forge::multiformats::bytes encode_public_key(const public_key& key);
[[nodiscard]] public_key decode_public_key(std::span<const std::uint8_t> bytes);
void validate_public_key(const public_key& key);
void validate_public_key(const public_key& key, const peer_id& expected_peer);
[[nodiscard]] peer_id make_peer_id(const public_key& key);
[[nodiscard]] peer_id make_peer_id_from_certificate_pem(std::string_view certificate_pem);
[[nodiscard]] peer_id make_peer_id_from_certificate_der(std::span<const std::uint8_t> der);
[[nodiscard]] bool valid_peer_id(const peer_id& id) noexcept;

// Text codecs preserve the canonical string representation. A default empty
// identity is a value sentinel; protocol admission must still require validity.
void to_variant(const peer_id& value, forge::variant& result);
void from_variant(const forge::variant& value, peer_id& result);

} // namespace forge::net::p2p

export template <> struct forge::schema::value_adapter<forge::net::p2p::peer_id> {
   static constexpr forge::schema::value_kind kind = forge::schema::value_kind::string;
   [[nodiscard]] static forge::schema::input_value encode(const forge::net::p2p::peer_id& input);
   [[nodiscard]] static forge::net::p2p::peer_id decode(const forge::schema::input_value& input);
};
