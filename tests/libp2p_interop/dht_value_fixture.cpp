#include "dht_value_fixture.hxx"

#include <stdexcept>
#include <string>

import forge.codec.hex;

namespace forge::test::libp2p_interop {

bool dht_value_fixture::is_scenario(std::string_view scenario) noexcept {
   return scenario == "dht_pk_put_get" || scenario == "dht_ipns_put_get";
}

dht_value_fixture dht_value_fixture::for_scenario(std::string_view scenario) {
   constexpr auto identity_multihash =
       std::string_view{"00240801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"};
   if (scenario == "dht_pk_put_get") {
      return {
          .key = {.bytes = forge::codec::hex::decode(std::string{"2f706b2f"} + std::string{identity_multihash})},
          .value =
              forge::codec::hex::decode("0801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"),
      };
   }
   if (scenario == "dht_ipns_put_get") {
      return {
          .key = {.bytes = forge::codec::hex::decode(std::string{"2f69706e732f"} + std::string{identity_multihash})},
          .value = forge::codec::hex::decode(
              "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c"
              "4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118"
              "00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09"
              "b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66"
              "e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278"
              "68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a"
              "30352e3132333435363738395a6c56616c69646974795479706500"),
      };
   }
   throw std::runtime_error{"unknown DHT value fixture: " + std::string{scenario}};
}

} // namespace forge::test::libp2p_interop
