module;

#include <cstdint>
#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module forge.chain.api.json_schema;

import forge.api.http.openapi;
import forge.chain.protocol.entity_selector;
import forge.chain.protocol.float64;
import forge.chain.protocol.types;
import forge.crypto.bls.serialization;
import forge.variant.value;

namespace forge::chain::api::schema {

[[nodiscard]] forge::api::http::openapi_query_object account_selector() {
   auto properties = forge::mutable_variant_object{};
   properties.set("id",
                  forge::variant{forge::mutable_variant_object{}("type", "integer")("minimum", std::uint64_t{0})});
   properties.set("key", forge::variant{forge::mutable_variant_object{}("type", "string")});

   const auto required = [](std::string_view name) {
      return forge::variant{forge::mutable_variant_object{}("required", forge::variants{std::string{name}})};
   };
   const auto without = [&required](std::string_view present, std::string_view absent) {
      return forge::variant{
          forge::mutable_variant_object{}("required", forge::variants{std::string{present}})("not", required(absent))};
   };

   return forge::api::http::openapi_query_object{
       .name = "selector",
       .schema = forge::variant{forge::mutable_variant_object{}("type", "object")("properties", std::move(properties))(
           "additionalProperties", false)("oneOf", forge::variants{without("id", "key"), without("key", "id")})},
       .fields = {"id", "key"},
   };
}

} // namespace forge::chain::api::schema

export namespace forge::api::http {

template <> struct json_schema_traits<forge::chain::protocol::public_key> {
   [[nodiscard]] static forge::variant make() {
      return forge::variant{forge::mutable_variant_object{}("type", "string")("format", "forge-public-key")};
   }
};

template <> struct json_schema_traits<forge::chain::protocol::signature> {
   [[nodiscard]] static forge::variant make() {
      return forge::variant{forge::mutable_variant_object{}("type", "string")("format", "forge-signature")};
   }
};

template <> struct json_schema_traits<forge::chain::protocol::float64> {
   [[nodiscard]] static forge::variant make() {
      return forge::variant{forge::mutable_variant_object{}("type", "number")("format", "double")};
   }
};

template <> struct json_schema_traits<forge::crypto::bls::public_key> {
   [[nodiscard]] static forge::variant make() {
      return forge::variant{forge::mutable_variant_object{}("type", "string")("format", "forge-bls-public-key")};
   }
};

template <typename Request>
   requires std::derived_from<Request, forge::chain::protocol::account_selector>
struct openapi_query_object_traits<Request> {
   [[nodiscard]] static std::vector<openapi_query_object> make() {
      return {forge::chain::api::schema::account_selector()};
   }
};

} // namespace forge::api::http
