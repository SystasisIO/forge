#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

import fixture.grant;
import forge.contract;
import forge.raw.codec;

class [[forge::contract("granttest")]] granttest : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] fixture::grant echo(fixture::grant value) {
      value.validate({.issuer = "https://issuer.example.test", .audience = "https://resource.example.test",
                      .purpose = "artifact.read", .schema = "https://schemas.example.test/grant/v1",
                      .max_lifetime = std::chrono::seconds{300}},
                     std::chrono::sys_seconds{std::chrono::seconds{1800000100}});
      return value;
   }

   [[forge::action]] std::vector<std::uint8_t> fresh() {
      const auto value = fixture::grant{
          .iss = "https://issuer.example.test", .aud = "https://resource.example.test",
          .jti = "g_" + std::string(64, 'a'), .iat = 1800000000, .nbf = 1800000000, .exp = 1800000300,
          .att = {.purpose = "artifact.read", .schema = "https://schemas.example.test/grant/v1",
                  .claims = {.object = "o_17", .generation = "18446744073709551615",
                             .ranges = {{"9007199254740993", "1"}}, .note = "media"}}};
      return forge::raw::pack(echo(value));
   }
};
