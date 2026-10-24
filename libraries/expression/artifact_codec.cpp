module;

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.expression.artifact_codec;

namespace forge::expression {
namespace {

void validate_shape(const artifact& value) {
   if (value.format != artifact_format || value.profile != profile_version || value.semantics != semantics_version) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "unsupported expression artifact version");
   }
   const limits ceiling;
   const auto budgets = std::array{std::pair{value.budget.source_bytes, ceiling.source_bytes},
                                   std::pair{value.budget.nodes, ceiling.nodes},
                                   std::pair{value.budget.depth, ceiling.depth},
                                   std::pair{value.budget.variables, ceiling.variables},
                                   std::pair{value.budget.collection_elements, ceiling.collection_elements},
                                   std::pair{value.budget.string_bytes, ceiling.string_bytes},
                                   std::pair{value.budget.total_input_bytes, ceiling.total_input_bytes},
                                   std::pair{value.budget.ast_events, ceiling.ast_events}};
   for (const auto [requested, maximum] : budgets) {
      if (requested == 0 || requested > maximum) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_profile, "expression artifact budget exceeds its profile");
      }
   }
   if (value.expression.empty() || value.expression.size() > max_expression_bytes ||
       value.variables.size() > value.budget.variables) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression artifact exceeds its envelope limits");
   }
   if (value.result != type::int64 && value.result != type::uint64 && value.result != type::boolean) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_program, "expression artifact result is not an admitted scalar");
   }
   const variable* previous = nullptr;
   for (const auto& item : value.variables) {
      if (item.name.empty() || item.name.size() > ceiling.string_bytes ||
          std::to_underlying(item.kind) > std::to_underlying(type::map_uint64) ||
          (previous != nullptr && previous->name >= item.name)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_program, "expression artifact declarations are not canonical");
      }
      previous = &item;
   }
   forge::datastream<std::size_t> size;
   raw::pack(size, value);
   if (size.tellp() > max_artifact_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression artifact exceeds its encoded byte limit");
   }
}

} // namespace

crypto::digest::sha256 artifact_digest(const artifact& value) {
   validate_shape(value);
   crypto::digest::sha256::encoder encoder;
   raw::pack(encoder, static_cast<const char*>("forge.expression.artifact.v1"));
   raw::pack(encoder, std::tie(value.format, value.profile, value.semantics, value.variables, value.budget,
                               value.result, value.expression));
   return encoder.result();
}

void validate(const artifact& value) {
   if (artifact_digest(value) != value.digest) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_program, "expression artifact digest does not match");
   }
}

raw::bytes encode_artifact(const artifact& value) {
   validate(value);
   return raw::pack(value);
}

artifact decode_artifact(std::span<const std::uint8_t> bytes) {
   if (bytes.empty() || bytes.size() > max_artifact_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::resource_exhausted, "expression artifact exceeds its encoded byte limit");
   }
   artifact value;
   try {
      value = raw::unpack_exact<artifact>(bytes, {.max_container_elements = 64,
                                                  .max_total_container_elements = 64,
                                                  .max_bytes = max_expression_bytes,
                                                  .first_container_elements = 64});
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_program, "malformed expression artifact envelope");
   }
   validate(value);
   return value;
}

} // namespace forge::expression
