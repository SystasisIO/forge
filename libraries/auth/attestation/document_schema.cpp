module;

#include <forge/exceptions/macros.hpp>

#include <string>
#include <utility>
#include <variant>

module forge.auth.attestation.document_schema;

namespace forge::auth::attestation {

void to_variant(const signed_document& input, forge::variant& output) {
   input.validate();
   output = forge::variant{input.compact()};
}

void from_variant(const forge::variant& input, signed_document& output) {
   if (!input.is_string()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_document, "attestation Compact wire value must be a string");
   }
   output = signed_document{input.get_string()};
}

} // namespace forge::auth::attestation

namespace forge::schema {

input_value value_adapter<forge::auth::attestation::signed_document>::encode(
    const forge::auth::attestation::signed_document& input) {
   input.validate();
   return input_value{input.compact()};
}

forge::auth::attestation::signed_document
value_adapter<forge::auth::attestation::signed_document>::decode(const input_value& input) {
   const auto* text = std::get_if<std::string>(&input.storage);
   if (text == nullptr) {
      FORGE_THROW_EXCEPTION(forge::auth::attestation::exceptions::invalid_document,
                            "attestation Compact wire value must be a string");
   }
   return forge::auth::attestation::signed_document{*text};
}

constraint value_adapter<forge::auth::attestation::signed_document>::constraints() {
   return {.kind = constraint_kind::string,
           .min_size = 1,
           .max_size = forge::auth::attestation::max_compact_bytes,
           .pattern = "^[!-~]+$",
           .ascii = true};
}

} // namespace forge::schema
