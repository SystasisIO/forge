module;

#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.codec.json;

import :exact;
import forge.schema.exceptions;

namespace forge::codec::json::detail {

std::string field_path(std::string_view path, std::string_view field) {
   if (path.empty()) {
      return std::string{field};
   }
   return std::string{path} + "." + std::string{field};
}

std::string element_path(std::string_view path, std::size_t index) {
   return std::string{path} + "[" + std::to_string(index) + "]";
}

void add_exact_error(std::vector<schema::diagnostic>& diagnostics, std::string path, std::string code,
                     std::string message) {
   diagnostics.push_back(schema::diagnostic{
       .path = path.empty() ? "$" : std::move(path),
       .code = std::move(code),
       .level = schema::severity::error,
       .message = std::move(message),
   });
}

bool matches_canonical_json_value(const variant& source, const variant& canonical) {
   switch (canonical.get_type()) {
   case variant::null_type:
      return source.is_null();
   case variant::int64_type: {
      const auto expected = canonical.as_int64();
      if (source.is_int64()) {
         return source.as_int64() == expected;
      }
      return expected >= 0 && source.is_uint64() && source.as_uint64() == static_cast<std::uint64_t>(expected);
   }
   case variant::uint64_type: {
      const auto expected = canonical.as_uint64();
      if (source.is_uint64()) {
         return source.as_uint64() == expected;
      }
      return source.is_int64() && source.as_int64() >= 0 && static_cast<std::uint64_t>(source.as_int64()) == expected;
   }
   case variant::double_type: {
      const auto expected = canonical.as_double();
      if (source.is_double()) {
         const auto actual = source.as_double();
         return actual == expected && (actual != 0.0 || std::signbit(actual) == std::signbit(expected));
      }
      if (expected == 0.0 && std::signbit(expected)) {
         return false;
      }
      if (source.is_int64()) {
         const auto value = source.as_int64();
         const auto converted = static_cast<double>(value);
         return converted == expected && converted >= -std::ldexp(1.0, 63) && converted < std::ldexp(1.0, 63) &&
                static_cast<std::int64_t>(converted) == value;
      }
      if (source.is_uint64()) {
         const auto value = source.as_uint64();
         const auto converted = static_cast<double>(value);
         return converted == expected && converted < std::ldexp(1.0, 64) &&
                static_cast<std::uint64_t>(converted) == value;
      }
      return false;
   }
   case variant::bool_type:
      return source.is_bool() && source.as_bool() == canonical.as_bool();
   case variant::string_type:
      return source.is_string() && source.get_string() == canonical.get_string();
   case variant::array_type: {
      if (!source.is_array() || source.get_array().size() != canonical.get_array().size()) {
         return false;
      }
      for (std::size_t index = 0; index < canonical.get_array().size(); ++index) {
         if (!matches_canonical_json_value(source.get_array()[index], canonical.get_array()[index])) {
            return false;
         }
      }
      return true;
   }
   case variant::object_type: {
      if (!source.is_object() || source.get_object().size() != canonical.get_object().size()) {
         return false;
      }
      for (const auto& entry : canonical.get_object()) {
         const auto source_entry = source.get_object().find(entry.key());
         if (source_entry == source.get_object().end() ||
             !matches_canonical_json_value(source_entry->value(), entry.value())) {
            return false;
         }
      }
      return true;
   }
   case variant::blob_type:
      return source.is_blob() && source.get_blob().data == canonical.get_blob().data;
   }
   return false;
}

schema::input_value to_schema_input(const variant& source) {
   return variant_schema::to_input(source);
}

void append_schema_diagnostics(std::vector<schema::diagnostic>& output, std::vector<schema::diagnostic> diagnostics) {
   for (auto& entry : diagnostics) {
      if (entry.code == "config.unknown") {
         entry.code = "json.unknown";
      } else if (entry.code == "config.missing") {
         entry.code = "json.missing";
      } else if (entry.code == "config.duplicate") {
         entry.code = "json.duplicate";
      } else if (entry.code == "config.type") {
         entry.code = "json.type";
      } else if (entry.code == "config.range") {
         entry.code = "json.range";
      }
      output.push_back(std::move(entry));
   }
}

} // namespace forge::codec::json::detail
