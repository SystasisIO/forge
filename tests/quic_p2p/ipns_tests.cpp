#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

import forge.chrono.iso8601;
import forge.codec.hex;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.asymmetric.rsa;
import forge.multiformats.varint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.chrono.timestamp;
import forge.net.p2p.ipns;

namespace forge::net::p2p::ipns {
namespace {

[[nodiscard]] forge::crypto::asymmetric::ed25519::private_key deterministic_key(std::uint8_t first = 1) {
   auto secret = forge::crypto::asymmetric::ed25519::private_key_secret{};
   for (auto index = std::size_t{}; index < secret.size(); ++index) {
      secret[index] = static_cast<std::uint8_t>(first + index);
   }
   return forge::crypto::asymmetric::ed25519::private_key::regenerate(secret);
}

[[nodiscard]] public_key libp2p_key(const forge::crypto::asymmetric::ed25519::private_key& key) {
   const auto serialized = key.get_public_key().serialize();
   return public_key{
       .type = public_key::type::ed25519,
       .data = {serialized.begin(), serialized.end()},
   };
}

[[nodiscard]] signing_callback signer(const forge::crypto::asymmetric::ed25519::private_key& key) {
   return [key](std::span<const std::uint8_t> message) {
      const auto signature = key.sign(message);
      return std::vector<std::uint8_t>{signature.begin(), signature.end()};
   };
}

[[nodiscard]] forge::chrono::timestamp at(unsigned year, unsigned month, unsigned day, unsigned hour = 0,
                                         unsigned minute = 0, unsigned second = 0,
                                         std::chrono::nanoseconds fraction = {}) {
   const auto whole =
       std::chrono::sys_seconds{std::chrono::sys_days{std::chrono::year{static_cast<int>(year)} / month / day}} +
       std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
   return forge::chrono::timestamp{whole, fraction};
}

[[nodiscard]] std::vector<std::uint8_t> bytes(std::span<const std::uint8_t> value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] std::pair<std::uint64_t, std::size_t> read_varint(std::span<const std::uint8_t> value,
                                                                std::size_t offset) {
   const auto decoded = forge::multiformats::varint_decode(value.subspan(offset));
   return {decoded.value, decoded.size};
}

[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> field_bounds(std::span<const std::uint8_t> value,
                                                                              std::uint32_t wanted) {
   auto offset = std::size_t{};
   while (offset < value.size()) {
      const auto begin = offset;
      const auto [key, key_size] = read_varint(value, offset);
      offset += key_size;
      const auto field = static_cast<std::uint32_t>(key >> 3U);
      const auto wire = static_cast<std::uint8_t>(key & 0x07U);
      if (wire == 0U) {
         const auto [ignored, size] = read_varint(value, offset);
         static_cast<void>(ignored);
         offset += size;
      } else if (wire == 1U) {
         offset += 8;
      } else if (wire == 2U) {
         const auto [size, size_length] = read_varint(value, offset);
         offset += size_length + static_cast<std::size_t>(size);
      } else if (wire == 5U) {
         offset += 4;
      } else {
         return std::nullopt;
      }
      if (offset > value.size()) {
         return std::nullopt;
      }
      if (field == wanted) {
         return std::pair{begin, offset};
      }
   }
   return std::nullopt;
}

[[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>>
bytes_field_payload(std::span<const std::uint8_t> value, std::uint32_t wanted) {
   const auto bounds = field_bounds(value, wanted);
   if (!bounds) {
      return std::nullopt;
   }
   auto offset = bounds->first;
   const auto [key, key_size] = read_varint(value, offset);
   offset += key_size;
   if ((key & 0x07U) != 2U) {
      return std::nullopt;
   }
   const auto [size, size_length] = read_varint(value, offset);
   offset += size_length;
   return std::pair{offset, offset + static_cast<std::size_t>(size)};
}

[[nodiscard]] std::vector<std::uint8_t> without_field(std::span<const std::uint8_t> value, std::uint32_t field) {
   auto out = std::vector<std::uint8_t>{value.begin(), value.end()};
   const auto bounds = field_bounds(out, field);
   BOOST_REQUIRE(bounds.has_value());
   out.erase(out.begin() + static_cast<std::ptrdiff_t>(bounds->first),
             out.begin() + static_cast<std::ptrdiff_t>(bounds->second));
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> with_flipped_bytes_field(std::span<const std::uint8_t> value,
                                                                 std::uint32_t field) {
   auto out = std::vector<std::uint8_t>{value.begin(), value.end()};
   const auto payload = bytes_field_payload(out, field);
   BOOST_REQUIRE(payload.has_value());
   BOOST_REQUIRE(payload->first != payload->second);
   out[payload->first] ^= 0x01U;
   return out;
}

[[nodiscard]] record make_record(const forge::crypto::asymmetric::ed25519::private_key& key, std::string_view value,
                                 std::uint64_t sequence, forge::chrono::timestamp eol, create_options options = {}) {
   return create(libp2p_key(key), signer(key),
                 std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()},
                 sequence, eol, std::chrono::minutes{5}, std::move(options));
}

// Produce a V2-only signed fixture with the original, possibly noncanonical EOL.
[[nodiscard]] std::vector<std::uint8_t>
with_signed_eol(const forge::crypto::asymmetric::ed25519::private_key& key, std::string_view text) {
   const auto original = std::string_view{"2030-01-02T03:04:05.123456789Z"};
   const auto base = make_record(key, "/ipfs/bafkqaaa", 7,
                                 at(2030, 1, 2, 3, 4, 5, std::chrono::nanoseconds{123'456'789}));
   auto data = bytes(base.data());
   const auto found = std::search(data.begin(), data.end(), original.begin(), original.end());
   BOOST_REQUIRE(found != data.end());
   const auto offset = static_cast<std::size_t>(found - data.begin());
   BOOST_REQUIRE(offset >= 2);
   BOOST_REQUIRE_EQUAL(data[offset - 2], 0x58U);
   BOOST_REQUIRE(text.size() >= 24 && text.size() <= 255);
   data[offset - 1] = static_cast<std::uint8_t>(text.size());
   data.erase(data.begin() + static_cast<std::ptrdiff_t>(offset),
              data.begin() + static_cast<std::ptrdiff_t>(offset + original.size()));
   data.insert(data.begin() + static_cast<std::ptrdiff_t>(offset), text.begin(), text.end());

   const auto prefix = std::string_view{"ipns-signature:"};
   auto message = std::vector<std::uint8_t>{prefix.begin(), prefix.end()};
   message.insert(message.end(), data.begin(), data.end());
   const auto signature = key.sign(message);
   auto encoded = std::vector<std::uint8_t>{};
   const auto append = [&](std::uint32_t field, std::span<const std::uint8_t> payload) {
      const auto tag = forge::multiformats::varint_encode((field << 3U) | 2U);
      const auto size = forge::multiformats::varint_encode(payload.size());
      encoded.insert(encoded.end(), tag.begin(), tag.end());
      encoded.insert(encoded.end(), size.begin(), size.end());
      encoded.insert(encoded.end(), payload.begin(), payload.end());
   };
   append(8, signature);
   append(9, data);
   return encoded;
}

// Pinned Boxo 23c380b: Ed25519 seed 01..20, 2030-01-02T03:04:05.123456789Z,
// sequence 7, TTL 5m, default V1 compatibility and default key embedding.
constexpr auto golden_hex = std::string_view{
    "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c"
    "4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118"
    "00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09"
    "b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66"
    "e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278"
    "68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a"
    "30352e3132333435363738395a6c56616c69646974795479706500"};

} // namespace

BOOST_AUTO_TEST_CASE(p2p_ipns_create_matches_pinned_boxo_v1_v2_golden) {
   const auto key = deterministic_key();
   const auto expected = forge::codec::hex::decode(golden_hex);
   const auto value = std::string_view{"/ipfs/bafkqac3jobxhgidsn5rww4yk"};
   const auto eol = at(2030, 1, 2, 3, 4, 5, std::chrono::nanoseconds{123'456'789});
   const auto created =
       create(libp2p_key(key), signer(key),
              std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(value.data()), value.size()}, 7, eol,
              std::chrono::minutes{5});

   BOOST_TEST(encode(created) == expected, boost::test_tools::per_element());
   BOOST_TEST(bytes(created.value()) == std::vector<std::uint8_t>(value.begin(), value.end()),
              boost::test_tools::per_element());
   BOOST_TEST(created.sequence() == 7U);
   BOOST_CHECK(created.eol() == eol);
   BOOST_TEST(created.eol_text() == "2030-01-02T03:04:05.123456789Z");
   BOOST_TEST(created.ttl() == std::chrono::minutes{5});
   BOOST_TEST(created.has_v1_compatibility());
   BOOST_TEST(created.has_v2_signature());
   BOOST_TEST(created.signature_v1().size() == 64U);
   BOOST_TEST(created.signature_v2().size() == 64U);
   BOOST_TEST(!created.embedded_public_key().has_value());
   validate(created, make_peer_id(libp2p_key(key)), std::nullopt, at(2029, 1, 1));
}

BOOST_AUTO_TEST_CASE(p2p_ipns_embedded_and_inline_public_key_paths_validate_binding) {
   const auto key = deterministic_key();
   const auto public_value = libp2p_key(key);
   const auto peer = make_peer_id(public_value);
   auto embedded_options = create_options{};
   embedded_options.embed_public_key = true;
   const auto embedded = make_record(key, "/ipfs/bafkqaaa", 1, at(2030, 1, 1), embedded_options);
   const auto embedded_key = embedded.embedded_public_key();
   BOOST_REQUIRE(embedded_key.has_value());
   BOOST_TEST(static_cast<int>(embedded_key->type) == static_cast<int>(public_value.type));
   BOOST_TEST(embedded_key->data == public_value.data, boost::test_tools::per_element());
   validate(embedded, peer, public_value, at(2029, 1, 1));

   auto inline_options = create_options{};
   inline_options.embed_public_key = false;
   const auto inline_record = make_record(key, "/ipfs/bafkqaaa", 2, at(2030, 1, 1), inline_options);
   BOOST_TEST(!inline_record.embedded_public_key().has_value());
   validate(inline_record, peer, std::nullopt, at(2029, 1, 1));

   const auto wrong_peer = make_peer_id(libp2p_key(deterministic_key(33)));
   BOOST_CHECK_THROW(validate(embedded, wrong_peer, std::nullopt, at(2029, 1, 1)), exceptions::invalid_identity);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_non_inline_key_uses_keybook_without_falling_back_from_embedded_key) {
   const auto private_key = forge::crypto::asymmetric::rsa::private_key::generate(2048);
   const auto public_value = public_key{
       .type = public_key::type::rsa,
       .data = private_key.get_public_key().serialize(),
   };
   const auto peer = make_peer_id(public_value);
   auto options = create_options{};
   options.embed_public_key = false;
   const auto created = create(
       public_value, [&private_key](std::span<const std::uint8_t> message) { return private_key.sign(message); },
       std::vector<std::uint8_t>{'/', 'i', 'p', 'f', 's', '/', 'v'}, 3, at(2030, 1, 1), std::chrono::minutes{5},
       options);
   BOOST_TEST(!created.embedded_public_key().has_value());

   auto resolver_calls = std::size_t{};
   const auto resolver = public_key_resolver{[&](const peer_id& requested) -> std::optional<public_key> {
      ++resolver_calls;
      BOOST_TEST(requested.to_string() == peer.to_string());
      return public_value;
   }};
   validate(created, peer, resolver, at(2029, 1, 1));
   BOOST_TEST(resolver_calls == 1U);
   BOOST_CHECK_THROW(validate(created, peer, std::nullopt, at(2029, 1, 1)), exceptions::invalid_identity);

   options.embed_public_key = true;
   const auto embedded = create(
       public_value, [&private_key](std::span<const std::uint8_t> message) { return private_key.sign(message); },
       std::vector<std::uint8_t>{'/', 'i', 'p', 'f', 's', '/', 'v'}, 4, at(2030, 1, 1), std::chrono::minutes{5},
       options);
   const auto malformed_embedded = decode(with_flipped_bytes_field(encode(embedded), 7));
   resolver_calls = 0;
   BOOST_CHECK_THROW(validate(malformed_embedded, peer, resolver, at(2029, 1, 1)), exceptions::invalid_identity);
   BOOST_TEST(resolver_calls == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_scalar_metadata_and_unknown_protobuf_fields_roundtrip) {
   const auto key = deterministic_key();
   auto options = create_options{};
   options.metadata_values = {
       {"_bool", true},
       {"_bytes", std::vector<std::uint8_t>{1, 2, 3}},
       {"_int", std::int64_t{-7}},
       {"_text", std::string{"forge"}},
   };
   const auto created = make_record(key, "/ipfs/bafkqaaa", 9, at(2030, 1, 1), std::move(options));
   BOOST_REQUIRE_EQUAL(created.metadata_values().size(), 4U);
   BOOST_TEST(std::get<bool>(created.metadata_values().at("_bool")));
   BOOST_TEST(std::get<std::int64_t>(created.metadata_values().at("_int")) == -7);
   BOOST_TEST(std::get<std::string>(created.metadata_values().at("_text")) == "forge");
   BOOST_TEST(std::get<std::vector<std::uint8_t>>(created.metadata_values().at("_bytes")) ==
                  (std::vector<std::uint8_t>{1, 2, 3}),
              boost::test_tools::per_element());

   auto with_unknown = encode(created);
   const auto unknown_key = forge::multiformats::varint_encode(15U << 3U);
   with_unknown.insert(with_unknown.end(), unknown_key.begin(), unknown_key.end());
   with_unknown.push_back(42U);
   const auto decoded = decode(with_unknown);
   BOOST_TEST(encode(decoded) == with_unknown, boost::test_tools::per_element());

   auto bad_options = create_options{};
   bad_options.metadata_values.emplace("Value", true);
   BOOST_CHECK_THROW(make_record(key, "/ipfs/bafkqaaa", 1, at(2030, 1, 1), std::move(bad_options)),
                     exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_create_rejects_invalid_metadata_and_oversized_input_before_signing) {
   const auto key = deterministic_key();
   auto calls = std::size_t{};
   const auto counted_signer = [&](std::span<const std::uint8_t>) {
      ++calls;
      return std::vector<std::uint8_t>(64, 1U);
   };

   auto invalid_text = create_options{};
   invalid_text.metadata_values.emplace("text", std::string{1, static_cast<char>(0x80)});
   BOOST_CHECK_THROW(static_cast<void>(create(libp2p_key(key), counted_signer, {}, 1, at(2030, 1, 1),
                                              std::chrono::minutes{5}, std::move(invalid_text))),
                     exceptions::invalid_options);
   BOOST_TEST(calls == 0U);

   const auto oversized = std::vector<std::uint8_t>(max_record_size + 1U, 0U);
   BOOST_CHECK_THROW(static_cast<void>(create(libp2p_key(key), counted_signer, oversized, 1, at(2030, 1, 1),
                                              std::chrono::minutes{5})),
                     exceptions::invalid_options);
   BOOST_TEST(calls == 0U);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_validation_rejects_tampering_mismatch_and_expiry) {
   const auto key = deterministic_key();
   const auto peer = make_peer_id(libp2p_key(key));
   const auto created = make_record(key, "/ipfs/bafkqaaa", 1, at(2030, 1, 1));

   const auto bad_signature = decode(with_flipped_bytes_field(encode(created), 8));
   BOOST_CHECK_THROW(validate(bad_signature, peer, std::nullopt, at(2029, 1, 1)), exceptions::invalid_identity);

   const auto mismatched_v1 = decode(with_flipped_bytes_field(encode(created), 1));
   BOOST_CHECK_THROW(validate(mismatched_v1, peer, std::nullopt, at(2029, 1, 1)), exceptions::protocol_error);

   const auto expired = make_record(key, "/ipfs/bafkqaaa", 2, at(2020, 1, 1));
   BOOST_CHECK_THROW(validate(expired, peer, std::nullopt, at(2020, 1, 2)), exceptions::protocol_error);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_create_rejects_malformed_key_and_mismatched_signer) {
   const auto key = deterministic_key();
   auto malformed = libp2p_key(key);
   malformed.data.pop_back();
   auto calls = std::size_t{};
   const auto counted = [&](std::span<const std::uint8_t>) {
      ++calls;
      return std::vector<std::uint8_t>(64, 1U);
   };
   BOOST_CHECK_THROW(static_cast<void>(create(malformed, counted, {}, 1, at(2030, 1, 1), std::chrono::minutes{5})),
                     exceptions::invalid_options);
   BOOST_TEST(calls == 0U);

   BOOST_CHECK_THROW(static_cast<void>(create(libp2p_key(key), signer(deterministic_key(2)), {}, 1, at(2030, 1, 1),
                                              std::chrono::minutes{5})),
                     exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_decode_rejects_malformed_noncanonical_and_oversized_records) {
   BOOST_CHECK_THROW(decode(std::vector<std::uint8_t>{0x4aU, 0x01U, 0xa0U}), exceptions::codec_error);
   BOOST_CHECK_THROW(decode(std::vector<std::uint8_t>{0x4aU, 0x05U, 0xa1U, 0x78U, 0x01U, 'x', 0xf5U}),
                     exceptions::codec_error);
   BOOST_CHECK_THROW(decode(std::vector<std::uint8_t>(max_record_size + 1U, 0U)), exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(p2p_ipns_sequence_preserves_full_uint64_boxo_range) {
   const auto key = deterministic_key();
   const auto peer = make_peer_id(libp2p_key(key));
   for (const auto sequence : {std::uint64_t{1} << 63U, (std::numeric_limits<std::uint64_t>::max)()}) {
      const auto created = make_record(key, "/ipfs/bafkqaaa", sequence, at(2030, 1, 1));
      const auto decoded = decode(encode(created));
      BOOST_TEST(decoded.sequence() == sequence);
      validate(decoded, peer, std::nullopt, at(2029, 1, 1));
   }
}

BOOST_AUTO_TEST_CASE(p2p_ipns_eol_supports_boxo_year_range_beyond_nanosecond_epoch) {
   const auto key = deterministic_key();
   const auto created = make_record(key, "/ipfs/bafkqaaa", 1, at(2500, 1, 2, 3, 4, 5));
   BOOST_TEST(created.eol_text() == "2500-01-02T03:04:05Z");
   BOOST_CHECK(created.eol() == at(2500, 1, 2, 3, 4, 5));
   validate(created, make_peer_id(libp2p_key(key)), std::nullopt, at(2499, 1, 1));
}

BOOST_AUTO_TEST_CASE(p2p_ipns_wide_eol_preserves_signed_original_text) {
   const auto key = deterministic_key();
   const auto peer = make_peer_id(libp2p_key(key));
   for (const auto text : {"2030-01-02T03:04:05.123456780Z",
                           "2030-01-02T04:34:05.123456789+01:30",
                           "2030-01-02T03:04:05.123456789987Z",
                           "9999-12-31T23:59:59.999999999Z",
                           "1969-12-31T23:59:59.999999999Z",
                           "1677-09-21T00:12:43.145224192Z",
                           "2262-04-11T23:47:16.854775807Z"}) {
      const auto encoded = with_signed_eol(key, text);
      const auto decoded = decode(encoded);
      const auto expected = forge::chrono::iso8601::parse_rfc3339_timestamp(text);
      BOOST_CHECK(decoded.eol() == expected);
      BOOST_TEST(decoded.eol_text() == text);
      BOOST_TEST(encode(decoded) == encoded, boost::test_tools::per_element());
      const auto before = forge::chrono::timestamp{
          expected.whole_seconds() - std::chrono::seconds{1}, expected.subsecond()};
      validate(decoded, peer, std::nullopt, before);
      const auto after = forge::chrono::timestamp{
          expected.whole_seconds() + std::chrono::seconds{1}, expected.subsecond()};
      BOOST_CHECK_THROW(validate(decoded, peer, std::nullopt, after), exceptions::protocol_error);
   }
}

BOOST_AUTO_TEST_CASE(p2p_ipns_wide_eol_keeps_typed_codec_and_option_errors) {
   const auto key = deterministic_key();
   for (const auto text : {"2030-02-30T03:04:05.123456789Z",
                           "2030-01-02T03:04:05.123456789+24:00",
                           "2030-01-02T03:04:05.123456789Ztrailing"}) {
      BOOST_CHECK_THROW(decode(with_signed_eol(key, text)), exceptions::codec_error);
   }
   BOOST_CHECK_THROW(make_record(key, "/ipfs/bafkqaaa", 1, at(10000, 1, 1)),
                     exceptions::invalid_options);
   const auto last = at(9999, 12, 31, 23, 59, 59, std::chrono::nanoseconds{999'999'999});
   const auto created = make_record(key, "/ipfs/bafkqaaa", 1, last);
   BOOST_CHECK(created.eol() == last);
   BOOST_TEST(created.eol_text() == "9999-12-31T23:59:59.999999999Z");
}

BOOST_AUTO_TEST_CASE(p2p_ipns_selector_matches_boxo_order_and_raw_tie_break) {
   const auto key = deterministic_key();
   const auto low_sequence = make_record(key, "/ipfs/bafkqaaa", 1, at(2030, 1, 1));
   const auto high_sequence = make_record(key, "/ipfs/bafkqaaa", 2, at(2030, 1, 1));
   auto candidates = std::vector<record>{low_sequence, high_sequence};
   BOOST_TEST(select(candidates) == 1U);

   const auto late_eol = make_record(key, "/ipfs/bafkqaaa", 2, at(2031, 1, 1));
   candidates = {high_sequence, late_eol};
   BOOST_TEST(select(candidates) == 1U);

   const auto v1_only_newer = decode(without_field(encode(late_eol), 8));
   candidates = {low_sequence, v1_only_newer};
   BOOST_TEST(select(candidates) == 0U);

   auto raw_low = encode(high_sequence);
   auto raw_high = raw_low;
   const auto unknown_key = forge::multiformats::varint_encode(15U << 3U);
   raw_low.insert(raw_low.end(), unknown_key.begin(), unknown_key.end());
   raw_low.push_back(1U);
   raw_high.insert(raw_high.end(), unknown_key.begin(), unknown_key.end());
   raw_high.push_back(2U);
   candidates = {decode(raw_low), decode(raw_high)};
   BOOST_TEST(select(candidates) == 1U);
   BOOST_TEST(encode(candidates[1]) == raw_high, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(p2p_ipns_routing_key_is_namespace_plus_raw_peer_multihash) {
   const auto peer = make_peer_id(libp2p_key(deterministic_key()));
   const auto key = routing_key(peer);
   BOOST_REQUIRE(key.size() > routing_prefix.size());
   const auto prefix = std::string_view{reinterpret_cast<const char*>(key.data()), routing_prefix.size()};
   BOOST_TEST(prefix == routing_prefix);
   BOOST_TEST(std::vector<std::uint8_t>(key.begin() + static_cast<std::ptrdiff_t>(routing_prefix.size()), key.end()) ==
                  peer.to_bytes(),
              boost::test_tools::per_element());
}

} // namespace forge::net::p2p::ipns
