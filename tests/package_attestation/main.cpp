#include <chrono>
#include <stdexcept>
#include <string>
#include <type_traits>

import forge.auth.api.attestation_signer;
import forge.auth.attestation.document_schema;
import forge.codec.json;
import forge.crypto.jose.codec;
import forge.variant.value;

int main() {
   namespace att = forge::auth::attestation;
   static_assert(std::is_abstract_v<forge::auth::api::attestation_signer>);
   forge::crypto::jose::header_policy{.typ = "forge-attestation+jwt"}.validate();
   const auto value = att::document<forge::variant_object>{
       .iss = "https://issuer.example.test", .aud = "https://resource.example.test",
       .jti = "g_" + std::string(64, 'a'), .iat = 100, .nbf = 100, .exp = 200,
       .att = {.purpose = "artifact.read", .schema = "https://schemas.example.test/grant/v1"}};
   value.validate({.issuer = value.iss, .audience = value.aud, .purpose = value.att.purpose,
                   .schema = value.att.schema, .max_lifetime = std::chrono::seconds{100}},
                  std::chrono::sys_seconds{std::chrono::seconds{150}});
   const auto encoded = forge::codec::json::write(value);
   if (!encoded.ok()) {
      throw std::runtime_error{"installed attestation schema encoding failed"};
   }
   const auto decoded = forge::codec::json::read<att::document<forge::variant_object>>(
       encoded.text, {.unknown_fields = forge::codec::json::unknown_field_policy::error,
                      .described_records = forge::codec::json::described_record_policy::exact});
   if (!decoded.ok() || decoded.value != value) {
      throw std::runtime_error{"installed attestation schema roundtrip failed"};
   }
}
