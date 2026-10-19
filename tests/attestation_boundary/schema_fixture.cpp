#include <filesystem>
#include <limits>
#include <stdexcept>

import fixture.grant;
import forge.auth.attestation.document_schema;
import forge.codec.json;
import forge.schema.constraint;
import forge.variant.schema;

namespace forge::schema {
template <> struct rules<fixture::byte_range> {
   static object_schema<fixture::byte_range> define() {
      using type = fixture::byte_range;
      auto result = object<type>();
      result.field<&type::offset>("offset").required().decimal_range(0, UINT64_MAX);
      result.field<&type::length>("length").required().decimal_range(0, UINT64_MAX);
      return result;
   }
};

template <> struct rules<fixture::read_claims> {
   static object_schema<fixture::read_claims> define() {
      using type = fixture::read_claims;
      auto result = object<type>();
      result.field<&type::object>("object").required().length(1, 128);
      result.field<&type::generation>("generation").required().decimal_range(0, UINT64_MAX);
      result.field<&type::ranges>("ranges").required().length(1, 8);
      result.field<&type::note>("note").required().length(1, 128);
      return result;
   }
};
} // namespace forge::schema

int main(int argc, char** argv) {
   if (argc != 2) {
      return 2;
   }
   // Only the package build companion imports product declarations. The
   // independent runtime consumer receives the resulting artifact file.
   const auto artifact = forge::auth::attestation::document_schema<fixture::read_claims>();
   const auto result = forge::codec::json::save_value(
       std::filesystem::path{argv[1]}, forge::variant_schema::from_input(forge::schema::export_artifact(artifact)));
   if (!result.ok()) {
      throw std::runtime_error{"cannot export fixture schema"};
   }
}
