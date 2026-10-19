#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

import forge.auth.attestation.document_schema;
import forge.codec.base64;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.asymmetric.p256;
import forge.crypto.jose.codec;
import forge.crypto.core.random;
import forge.schema.constraint;
import forge.variant.schema;
import forge.variant.value;

namespace {
namespace att = forge::auth::attestation;
namespace json = forge::codec::json;
namespace jose = forge::crypto::jose;
namespace schema = forge::schema;
namespace base64 = forge::codec::base64;
using document = att::document<forge::variant_object>;

constexpr auto issuer = "https://issuer.example.test";
constexpr auto audience = "https://resource.example.test";
constexpr auto purpose = "artifact.read";
constexpr auto schema_id = "https://schemas.example.test/grant/v1";

[[nodiscard]] json::read_options strict_options() {
   return {.max_depth = att::max_json_depth,
           .unknown_fields = json::unknown_field_policy::error,
           .described_records = json::described_record_policy::exact,
           .max_bytes = att::max_payload_bytes,
           .max_nodes = att::max_json_nodes,
           .max_container_size = att::max_container_size,
           .max_string_bytes = att::max_string_bytes};
}

[[nodiscard]] att::verification_policy profile() {
   return {.issuer = issuer, .audience = audience, .purpose = purpose, .schema = schema_id,
           .max_lifetime = std::chrono::seconds{300}, .clock_skew = std::chrono::seconds{0}};
}

[[nodiscard]] std::string read_file(const std::filesystem::path& path, std::size_t bound) {
   std::ifstream file{path, std::ios::binary};
   if (!file || std::filesystem::file_size(path) > bound) {
      throw std::runtime_error{"missing or oversized test input"};
   }
   return {std::istreambuf_iterator<char>{file}, {}};
}

void write_file(const std::filesystem::path& path, std::string_view value) {
   std::ofstream file{path, std::ios::binary};
   file.write(value.data(), static_cast<std::streamsize>(value.size()));
   if (!file) {
      throw std::runtime_error{"cannot persist test artifact"};
   }
}

[[nodiscard]] std::string write_json(const auto& value) {
   auto result = json::write(value);
   if (!result.ok()) {
      throw std::runtime_error{"cannot encode test artifact"};
   }
   return std::move(result.text);
}

[[nodiscard]] jose::trusted_key load_key(const std::filesystem::path& path) {
   const auto parsed = json::read_value(read_file(path, 1024), strict_options());
   if (!parsed.ok() || !parsed.value.is_object()) {
      throw std::runtime_error{"invalid public test key"};
   }
   const auto& value = parsed.value.get_object();
   if (value.size() != 5 || value["kty"].get_string() != "EC" || value["crv"].get_string() != "P-256") {
      throw std::runtime_error{"unexpected public test key"};
   }
   const auto options = base64::decode_options{.characters = base64::alphabet::url,
                                               .pad = base64::padding_policy::forbid};
   const auto x = base64::decode(value["x"].get_string(), options);
   const auto y = base64::decode(value["y"].get_string(), options);
   if (x.size() != 32 || y.size() != 32) {
      throw std::runtime_error{"invalid public test point"};
   }
   jose::trusted_key key{.kid = value["kid"].get_string()};
   key.public_key[0] = 4;
   std::copy(x.begin(), x.end(), key.public_key.begin() + 1);
   std::copy(y.begin(), y.end(), key.public_key.begin() + 33);
   return key;
}

[[nodiscard]] schema::artifact product_schema() {
   schema::constraint identifier{.kind = schema::constraint_kind::string};
   identifier.min_size = 1;
   identifier.max_size = 128;
   schema::constraint decimal{.kind = schema::constraint_kind::string};
   decimal.ascii = true;
   decimal.pattern = "^(0|[1-9][0-9]*)$";
   decimal.decimal_minimum = 0;
   decimal.decimal_maximum = std::numeric_limits<std::uint64_t>::max();
   schema::constraint range{.kind = schema::constraint_kind::object};
   range.properties = {{"offset", decimal}, {"length", decimal}};
   range.required = {"offset", "length"};
   schema::constraint ranges{.kind = schema::constraint_kind::array};
   ranges.items = {range};
   ranges.min_size = 1;
   ranges.max_size = 8;
   schema::constraint root{.kind = schema::constraint_kind::object};
   root.properties = {{"object", identifier}, {"generation", decimal}, {"ranges", ranges}, {"note", identifier}};
   root.required = {"object", "generation", "ranges", "note"};
   return {.root = std::move(root)};
}

void verify(std::string_view token, const jose::trusted_key& key) {
   const auto snapshot = jose::trusted_keys{{.trust_domain = issuer, .verifier_profile = "interop-f1", .revision = 1},
                                             {key}, {.typ = "forge-attestation+jwt"}};
   const auto authenticated = jose::verifier{}.verify(token, snapshot);
   const auto parsed = json::read<document>(authenticated.payload(), strict_options());
   if (!parsed.ok()) {
      throw std::invalid_argument{"invalid attestation"};
   }
   parsed.value.validate(profile(), std::chrono::sys_seconds{std::chrono::seconds{1800000100}});
   if (!schema::validate(product_schema(), forge::variant_schema::to_input(forge::variant{parsed.value.att.claims})).empty()) {
      throw std::invalid_argument{"invalid product claims"};
   }
}

void emit(const std::filesystem::path& directory) {
   std::filesystem::create_directories(directory);
   // Public P-256 generator and scalar 1 are disposable test vectors only.
   const auto point = forge::codec::hex::decode(
       "046b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296"
       "4fe342e2fe1a7f9b8ee7eb4a7c0f9e162bce33576b315ececbb6406837bf51f5");
   jose::trusted_key key{.kid = "interop-key"};
   std::copy(point.begin(), point.end(), key.public_key.begin());
   const auto encoding = base64::encode_options{.characters = base64::alphabet::url, .pad = base64::padding::omit};
   auto public_jwk = forge::mutable_variant_object{};
   public_jwk("kty", "EC")("crv", "P-256")("kid", key.kid)
       ("x", base64::encode(std::span<const std::uint8_t>{point}.subspan(1, 32), encoding))
       ("y", base64::encode(std::span<const std::uint8_t>{point}.subspan(33, 32), encoding));
   write_file(directory / "public.jwk", write_json(forge::variant{public_jwk}));
   const auto claims = json::read_value(
       R"({"object":"o_17","generation":"18446744073709551615","ranges":[{"offset":"9007199254740993","length":"1"}],"note":"\u043c\u0435\u0434\u0438\u0430"})",
       strict_options());
   if (!claims.ok()) {
      throw std::runtime_error{"invalid fixture claims"};
   }
   const auto random = forge::crypto::core::random_array<32>();
   const auto value = document{.iss = issuer, .aud = audience,
       .jti = "g_" + forge::codec::hex::encode(random), .iat = 1800000000, .nbf = 1800000000, .exp = 1800000300,
       .att = {.purpose = purpose, .schema = schema_id, .claims = claims.value.get_object()}};
   value.validate(profile(), std::chrono::sys_seconds{std::chrono::seconds{1800000100}});
   auto prepared = jose::prepare(write_json(value), key, {.typ = "forge-attestation+jwt"});
   const auto private_key = forge::crypto::asymmetric::p256::private_key::regenerate(
       forge::crypto::digest::sha256{std::string(63, '0') + '1'});
   const auto signature = private_key.sign_compact(prepared.signing_digest());
   const auto token = prepared.complete(std::span<const std::uint8_t>{signature}.subspan(1));
   verify(token, key);
   write_file(directory / "forge.jwt", token);
}

void export_schema(const std::filesystem::path& directory) {
   auto rules = att::document_schema<forge::variant_object>();
   rules.root.properties.at("att").properties.at("claims") = product_schema().root;
   const auto exported = forge::variant_schema::from_input(schema::json_schema(rules));
   write_file(directory / "document.schema.json", write_json(exported));
}
} // namespace

int main(int argc, char** argv) {
   try {
      if (argc == 3 && std::string_view{argv[1]} == "emit") {
         emit(argv[2]);
         std::cout << "EMITTED\n";
         return 0;
      }
      if (argc == 3 && std::string_view{argv[1]} == "schema") {
         export_schema(argv[2]);
         std::cout << "EXPORTED\n";
         return 0;
      }
      if (argc == 4 && std::string_view{argv[1]} == "verify") {
         const auto key = load_key(argv[3]);
         if (!std::filesystem::is_regular_file(argv[2])) {
            throw std::runtime_error{"missing test token"};
         }
         if (std::filesystem::file_size(argv[2]) > att::max_compact_bytes) {
            std::cout << "REJECT\n";
            return 1;
         }
         const auto token = read_file(argv[2], att::max_compact_bytes);
         try {
            verify(token, key);
         } catch (const std::exception&) {
            std::cout << "REJECT\n";
            return 1;
         }
         std::cout << "ACCEPT\n";
         return 0;
      }
   } catch (const std::exception&) {
      std::cerr << "interop harness error\n";
   }
   return 2;
}
