module;

#include <forge/exceptions/macros.hpp>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <vector>

module forge.variant.schema;

import forge.schema.exceptions;

namespace forge::variant_schema {
namespace {

void bounded(const variant& value, const schema::constraint_limits& limits, std::size_t depth, std::size_t& nodes) {
   if (depth > limits.max_depth || ++nodes > limits.max_nodes)
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic value exceeds depth/node limit");
   if (value.is_string() && value.get_string().size() > limits.max_string_bytes)
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic string exceeds byte limit");
   if (value.is_double() && !std::isfinite(value.as_double()))
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic number must be finite");
   if (value.is_blob()) FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic JSON cannot contain blobs");
   if (value.is_array()) {
      if (value.get_array().size() > limits.max_container_size)
         FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic array exceeds element limit");
      for (const auto& child : value.get_array()) bounded(child, limits, depth + 1, nodes);
   }
   if (value.is_object()) {
      if (value.get_object().size() > limits.max_container_size)
         FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic object exceeds member limit");
      auto names = std::set<std::string>{};
      for (const auto& entry : value.get_object()) {
         if (entry.key().size() > limits.max_string_bytes || !names.insert(entry.key()).second)
            FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "invalid or duplicate dynamic member");
         bounded(entry.value(), limits, depth + 1, nodes);
      }
   }
}

} // namespace

schema::input_value to_input(const variant& input, schema::constraint_limits limits) {
   auto nodes = std::size_t{};
   bounded(input, limits, 1, nodes);
   return detail::to_schema_input(input);
}

variant from_input(const schema::input_value& input, schema::constraint_limits limits) {
   if (!schema::validate(schema::constraint{}, input, {}, limits).empty())
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "dynamic input exceeds structural limits");
   return detail::from_schema_input(input);
}

std::vector<schema::diagnostic> validate(const schema::artifact& rules, const variant& input,
                                        schema::constraint_limits limits) {
   try {
      return schema::validate(rules, to_input(input, limits), limits);
   } catch (const schema::exceptions::invalid_value&) {
      return {{.path = "$", .code = "schema.limit", .level = schema::severity::error,
               .message = "dynamic input is not a bounded JSON value"}};
   }
}

} // namespace forge::variant_schema

namespace forge::schema {

input_value value_adapter<forge::variant_object>::encode(const forge::variant_object& input) {
   return forge::variant_schema::to_input(forge::variant{input});
}

forge::variant_object value_adapter<forge::variant_object>::decode(const input_value& input) {
   if (!input.as_object())
      FORGE_THROW_EXCEPTION(exceptions::invalid_value, "dynamic object requires an exact JSON object");
   return forge::variant_schema::from_input(input).get_object();
}

constraint value_adapter<forge::variant_object>::constraints() {
   return constraint{.kind = constraint_kind::object, .additional_properties = true};
}

} // namespace forge::schema
