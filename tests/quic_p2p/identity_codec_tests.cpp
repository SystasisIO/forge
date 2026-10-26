#include <boost/test/unit_test.hpp>
#include <cstdint>
#include <string>
#include <vector>

import forge.net.p2p.identity;
import forge.net.p2p.exceptions;
import forge.raw.raw;
import forge.schema.input_value;
import forge.schema.value_kind;
import forge.variant.value;

namespace forge::net::p2p {

BOOST_AUTO_TEST_CASE(p2p_identity_text_codecs_preserve_string_and_raw_layout) {
   const auto identity = make_peer_id({public_key::type::ed25519, std::vector<std::uint8_t>(32, 17)});
   auto encoded = forge::variant{};
   to_variant(identity, encoded);
   BOOST_TEST(encoded.is_string());
   BOOST_TEST(encoded.as_string() == identity.value);
   auto decoded = peer_id{};
   from_variant(encoded, decoded);
   BOOST_CHECK(decoded == identity);
   BOOST_TEST(forge::raw::pack(identity) == forge::raw::pack(identity.value));
   BOOST_CHECK(forge::raw::unpack_exact<peer_id>(forge::raw::pack(identity)) == identity);
   const auto scalar = forge::schema::value_adapter<peer_id>::encode(identity);
   BOOST_CHECK(forge::schema::value_adapter<peer_id>::decode(scalar) == identity);
   BOOST_CHECK_THROW((void)forge::schema::value_adapter<peer_id>::decode(forge::schema::input_value{true}),
                     exceptions::invalid_identity);
   BOOST_CHECK_THROW(from_variant(forge::variant{std::string{"invalid-peer"}}, decoded), exceptions::invalid_identity);
   BOOST_CHECK_THROW(from_variant(forge::variant{}, decoded), exceptions::invalid_identity);
   BOOST_CHECK_THROW(from_variant(forge::variant{true}, decoded), exceptions::invalid_identity);
   BOOST_CHECK_THROW(from_variant(forge::variant{std::uint64_t{42}}, decoded), exceptions::invalid_identity);
   from_variant(forge::variant{identity.to_cid_string()}, decoded);
   BOOST_CHECK(decoded == identity);
   BOOST_CHECK_THROW(to_variant(peer_id{"invalid-peer"}, encoded), exceptions::invalid_identity);
   from_variant(forge::variant{std::string{}}, decoded);
   BOOST_TEST(decoded.value.empty());
   BOOST_TEST(!valid_peer_id(decoded));
}


} // namespace forge::net::p2p
