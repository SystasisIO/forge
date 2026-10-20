module;

#include <boost/describe.hpp>

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

export module forge.auth.attestation.document_schema;

export import forge.auth.attestation.document;
export import forge.auth.attestation.signed_document;
export import forge.schema.object;

import forge.schema.constraint;
import forge.variant.described;
export import forge.variant.schema;
export import forge.variant.value;

export namespace forge::auth::attestation {

// Explicit native wire conversion; diagnostic conversion is always redacted.
void to_variant(const signed_document& input, forge::variant& output);
void from_variant(const forge::variant& input, signed_document& output);

template <typename Claims> [[nodiscard]] forge::schema::artifact document_schema();

} // namespace forge::auth::attestation

export namespace forge::schema {

template <> struct value_adapter<forge::auth::attestation::signed_document> {
   static constexpr value_kind kind = value_kind::string;

   [[nodiscard]] static input_value encode(const forge::auth::attestation::signed_document& input);
   [[nodiscard]] static forge::auth::attestation::signed_document decode(const input_value& input);
   [[nodiscard]] static constraint constraints();
};

template <typename Claims> struct rules<forge::auth::attestation::extension<Claims>> {
   [[nodiscard]] static object_schema<forge::auth::attestation::extension<Claims>> define() {
      using type = forge::auth::attestation::extension<Claims>;
      static_assert(boost::describe::has_describe_members<Claims>::value || std::same_as<Claims, forge::variant_object>,
                    "attestation claims require a described object or native variant_object");
      auto result = object<type>();
      result.template field<&type::v>("v").required().range(std::int64_t{1}, std::int64_t{1});
      result.template field<&type::purpose>("purpose")
          .required().ascii().length(1, forge::auth::attestation::max_purpose_bytes).pattern("^[a-z][a-z0-9._-]*$");
      result.template field<&type::schema>("schema")
          .required().ascii().length(1, forge::auth::attestation::max_identifier_bytes)
          .pattern(forge::auth::attestation::detail::uri_identifier_pattern);
      result.template field<&type::claims>("claims").required().secret();
      return result;
   }
};

template <typename Claims> struct rules<forge::auth::attestation::document<Claims>> {
   [[nodiscard]] static object_schema<forge::auth::attestation::document<Claims>> define() {
      using type = forge::auth::attestation::document<Claims>;
      auto result = object<type>();
      constexpr auto uri_pattern = forge::auth::attestation::detail::uri_identifier_pattern;
      result.template field<&type::iss>("iss")
          .required().ascii().length(1, forge::auth::attestation::max_identifier_bytes).pattern(uri_pattern);
      result.template field<&type::aud>("aud")
          .required().ascii().length(1, forge::auth::attestation::max_identifier_bytes).pattern(uri_pattern);
      result.template field<&type::jti>("jti")
          .required().ascii().length(forge::auth::attestation::document_id_bytes,
                                    forge::auth::attestation::document_id_bytes).pattern("^g_[0-9a-f]{64}$");
      result.template field<&type::iat>("iat").required().range(std::int64_t{0}, forge::auth::attestation::max_numeric_date);
      result.template field<&type::nbf>("nbf").required().range(std::int64_t{0}, forge::auth::attestation::max_numeric_date);
      result.template field<&type::exp>("exp").required().range(std::int64_t{0}, forge::auth::attestation::max_numeric_date);
      result.template field<&type::att>("att").required();
      return result;
   }
};

} // namespace forge::schema

export namespace forge::auth::attestation {

template <typename Claims> [[nodiscard]] forge::schema::artifact document_schema() {
   return forge::schema::export_constraints<document<Claims>>();
}

} // namespace forge::auth::attestation
