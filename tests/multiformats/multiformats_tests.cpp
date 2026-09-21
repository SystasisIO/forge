#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

#include <forge/exceptions/macros.hpp>

import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;
import forge.exceptions;

namespace {

[[nodiscard]] forge::multiformats::bytes bytes_from_text(std::string_view text) {
   return {text.begin(), text.end()};
}

} // namespace

BOOST_AUTO_TEST_SUITE(multiformats)

BOOST_AUTO_TEST_CASE(varint_uses_minimal_unsigned_leb128_encoding) try {
   using forge::multiformats::varint_decode;
   using forge::multiformats::varint_encode;

   BOOST_CHECK(varint_encode(0).empty() == false);
   BOOST_CHECK_EQUAL(varint_encode(0).front(), 0);

   const auto encoded_300 = varint_encode(300);
   const std::vector<std::uint8_t> expected_300{0xac, 0x02};
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded_300.begin(), encoded_300.end(), expected_300.begin(), expected_300.end());

   const auto decoded = varint_decode(expected_300);
   BOOST_CHECK_EQUAL(decoded.value, 300);
   BOOST_CHECK_EQUAL(decoded.size, 2);

   const std::vector<std::uint8_t> non_minimal{0x80, 0x00};
   BOOST_CHECK_THROW((void)varint_decode(non_minimal), forge::multiformats::exceptions::invalid_format);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(varint_rejects_overlong_zero_payload_before_shift) try {
   using forge::multiformats::varint_decode;

   const std::vector<std::uint8_t> overflowing_zero_payload{
       0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x00};

   BOOST_CHECK_EXCEPTION((void)varint_decode(overflowing_zero_payload),
                         forge::multiformats::exceptions::invalid_format,
                         [](const auto& error) { return std::string{error.what()}.find("overflows") != std::string::npos; });
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multicodec_constants_match_libp2p_foundation_codes) try {
   using enum forge::multiformats::multicodec_code;

   BOOST_CHECK_EQUAL(forge::multiformats::code_value(identity), 0x00);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(sha2_256), 0x12);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(sha2_512), 0x13);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(libp2p_key), 0x72);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(ip4), 0x04);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(dnsaddr), 0x38);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(udp), 0x0111);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(quic), 0x01cc);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(quic_v1), 0x01cd);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(p2p_circuit), 0x0122);
   BOOST_CHECK_EQUAL(forge::multiformats::code_value(p2p), 0x01a5);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multihash_roundtrips_identity_and_sha2_256) try {
   const auto hello = bytes_from_text("hello");

   auto identity = forge::multiformats::multihash::identity(hello);
   BOOST_CHECK_EQUAL(identity.code, forge::multiformats::code_value(forge::multiformats::multicodec_code::identity));
   BOOST_CHECK_EQUAL_COLLECTIONS(identity.digest.begin(), identity.digest.end(), hello.begin(), hello.end());

   auto encoded_identity = identity.encode();
   auto decoded_identity = forge::multiformats::multihash::decode(encoded_identity);
   BOOST_CHECK_EQUAL_COLLECTIONS(
       decoded_identity.digest.begin(), decoded_identity.digest.end(), hello.begin(), hello.end());

   auto sha = forge::multiformats::multihash::sha2_256(hello);
   BOOST_CHECK_EQUAL(sha.digest_hex(), "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multibase_uses_libp2p_required_prefixes) try {
   const auto hello = bytes_from_text("hello world");

   BOOST_CHECK_EQUAL(forge::multiformats::multibase_encode(forge::multiformats::multibase_code::base58btc, hello),
                     "zStV1DL6CwTryKyV");
   BOOST_CHECK_EQUAL(forge::multiformats::multibase_encode(forge::multiformats::multibase_code::base32, hello),
                     "bnbswy3dpeb3w64tmmq");

   auto decoded = forge::multiformats::multibase_decode("BNBSWY3DPEB3W64TMMQ");
   BOOST_CHECK_EQUAL_COLLECTIONS(decoded.bytes.begin(), decoded.bytes.end(), hello.begin(), hello.end());
   BOOST_TEST(static_cast<int>(decoded.code) ==
              static_cast<int>(forge::multiformats::multibase_code::base32_upper));
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_matches_donor_tcp_and_quic_binary_vectors) try {
   static_assert(std::is_same_v<decltype(forge::multiformats::multiaddr{}.components()),
                                const std::vector<forge::multiformats::multiaddr_component>&>);

   const auto tcp = forge::multiformats::multiaddr::parse("/ip4/192.0.2.42/tcp/443");
   const auto expected_tcp = forge::multiformats::bytes{0x04, 0xc0, 0x00, 0x02, 0x2a, 0x06, 0x01, 0xbb};
   const auto encoded_tcp = tcp.to_bytes();
   BOOST_CHECK_EQUAL(tcp.to_string(), "/ip4/192.0.2.42/tcp/443");
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded_tcp.begin(), encoded_tcp.end(), expected_tcp.begin(), expected_tcp.end());
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(expected_tcp).to_string(), "/ip4/192.0.2.42/tcp/443");

   const auto quic = forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/udp/4001/quic-v1");
   const auto expected_quic =
       forge::multiformats::bytes{0x04, 0x7f, 0x00, 0x00, 0x01, 0x91, 0x02, 0x0f, 0xa1, 0xcd, 0x03};
   const auto encoded_quic = quic.to_bytes();
   BOOST_CHECK_EQUAL(quic.to_string(), "/ip4/127.0.0.1/udp/4001/quic-v1");
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded_quic.begin(), encoded_quic.end(), expected_quic.begin(), expected_quic.end());
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(expected_quic).to_string(),
                     "/ip4/127.0.0.1/udp/4001/quic-v1");
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_ip6zone_matches_donor_variable_length_binary_vector) try {
   const auto text = std::string{"/ip6zone/en0/ip6/fe80::1/tcp/4001"};
   const auto expected = forge::multiformats::bytes{
       0x2a, 0x03, 'e', 'n', '0', 0x29,
       0xfe, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
       0x06, 0x0f, 0xa1,
   };

   const auto parsed = forge::multiformats::multiaddr::parse(text);
   const auto encoded = parsed.to_bytes();
   BOOST_CHECK_EQUAL(parsed.to_string(), text);
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), expected.begin(), expected.end());
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(expected).to_string(), text);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_roundtrips_dns_wss_peer_and_relay_circuit) try {
   const auto peer = std::string{"QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC"};
   const auto wss = "/dns4/example.com/tcp/443/wss/p2p/" + peer;
   const auto relayed = "/ip4/127.0.0.1/tcp/9090/p2p-circuit/p2p/" + peer;

   auto wss_address = forge::multiformats::multiaddr::parse(wss);
   BOOST_CHECK_EQUAL(wss_address.to_string(), wss);
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(wss_address.to_bytes()).to_string(), wss);

   auto relayed_address = forge::multiformats::multiaddr::parse(relayed);
   BOOST_CHECK_EQUAL(relayed_address.to_string(), relayed);
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(relayed_address.to_bytes()).to_string(), relayed);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_dnsaddr_matches_standard_variable_string_vectors) try {
   const auto dnsaddr = forge::multiformats::multiaddr::parse("/dnsaddr/bootstrap.libp2p.io");
   const auto expected_dnsaddr = forge::multiformats::bytes{
       0x38, 0x13, 'b', 'o', 'o', 't', 's', 't', 'r', 'a', 'p', '.', 'l', 'i', 'b', 'p', '2', 'p', '.', 'i', 'o'};
   const auto encoded_dnsaddr = dnsaddr.to_bytes();
   BOOST_CHECK_EQUAL(dnsaddr.to_string(), "/dnsaddr/bootstrap.libp2p.io");
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded_dnsaddr.begin(), encoded_dnsaddr.end(), expected_dnsaddr.begin(),
                                 expected_dnsaddr.end());
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(expected_dnsaddr).to_string(),
                     "/dnsaddr/bootstrap.libp2p.io");

   const auto peer = std::string{"QmcgpsyWgH8Y8ajJz1Cu72KnS5uo2Aa2LpzU7kinSupNKC"};
   const auto composed = "/dnsaddr/bootstrap.libp2p.io/p2p/" + peer;
   const auto composed_address = forge::multiformats::multiaddr::parse(composed);
   BOOST_CHECK_EQUAL(composed_address.to_string(), composed);
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::from_bytes(composed_address.to_bytes()).to_string(), composed);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_encapsulates_and_decapsulates_like_libp2p) try {
   const auto base = forge::multiformats::multiaddr::parse("/ip4/1.2.3.4");
   const auto inner = forge::multiformats::multiaddr::parse("/tcp/80/ws");
   BOOST_CHECK_EQUAL(base.encapsulate(inner).to_string(), "/ip4/1.2.3.4/tcp/80/ws");

   const auto quic = forge::multiformats::multiaddr::parse("/ip4/1.2.3.6/udp/1234/quic-v1");
   BOOST_CHECK_EQUAL(quic.decapsulate(forge::multiformats::multiaddr::parse("/udp/1234")).to_string(),
                     "/ip4/1.2.3.6");
   BOOST_CHECK_EQUAL(quic.decapsulate(forge::multiformats::multiaddr::parse("/udp/1234/quic-v1")).to_string(),
                     "/ip4/1.2.3.6");
   BOOST_CHECK_EQUAL(quic.decapsulate(forge::multiformats::multiaddr::parse("/tcp/80")).to_string(),
                     "/ip4/1.2.3.6/udp/1234/quic-v1");
   BOOST_CHECK_EQUAL(forge::multiformats::multiaddr::parse("/ip4/1.2.3.4").decapsulate(base).to_string(), "");
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_rejects_malformed_donor_cases_with_typed_errors) try {
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/quic-v1/1234"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip4/not-an-ip/tcp/80"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/tcp/not-a-port"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/tcp/65536"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip4/127.0.0.1/p2p/not-base58-0"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/dnsaddr/"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip6zone/"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::parse("/ip6zone/en0//ip6/fe80::1"),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x38, 0x00}),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(
       (void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x38, 0x03, 'a', '/', 'b'}),
       forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x2a, 0x00}),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(
       (void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x2a, 0x03, 'a', '/', 'b'}),
       forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x2a, 0x80}),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0x04, 0x7f}),
                     forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW((void)forge::multiformats::multiaddr::from_bytes(forge::multiformats::bytes{0xff, 0xff, 0xff, 0xff}),
                     forge::multiformats::exceptions::invalid_format);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_CASE(multiaddr_push_rejects_invalid_component_state) try {
   using enum forge::multiformats::protocol_code;

   auto value = forge::multiformats::multiaddr{};
   BOOST_CHECK_THROW(value.push({.code = ip4, .value = {}}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = ip6zone, .value = {}}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = ip6zone, .value = "en/0"}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = tcp, .value = {}}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = p2p, .value = {}}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = quic_v1, .value = "unexpected"}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = ws, .value = "unexpected"}), forge::multiformats::exceptions::invalid_format);
   BOOST_CHECK_THROW(value.push({.code = wss, .value = "unexpected"}), forge::multiformats::exceptions::invalid_format);
}
FORGE_LOG_AND_RETHROW();

BOOST_AUTO_TEST_SUITE_END()
