module;

#include <algorithm>
#include <any>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

module forge.config.core.decode;

import forge.config.core.value;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.scalar;

namespace forge::config::core {

bool parse_bool_text(std::string text, bool& output) {
   try {
      output = forge::schema::parse_scalar_text<bool>(text);
      return true;
   } catch (const std::invalid_argument&) {
      return false;
   }
}

schema::input_value to_schema_value(const value& input) {
   if (const auto* bool_value = std::get_if<bool>(&input.storage)) {
      return schema::input_value{*bool_value};
   }
   if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
      return schema::input_value{*signed_value};
   }
   if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
      return schema::input_value{*unsigned_value};
   }
   if (const auto* floating_value = std::get_if<double>(&input.storage)) {
      return schema::input_value{*floating_value};
   }
   if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
      return schema::input_value{*string_value};
   }
   if (const auto* array_value = input.as_array()) {
      auto out = schema::input_value::array_type{};
      out.reserve(array_value->size());
      for (const auto& item : *array_value) {
         out.push_back(to_schema_value(item));
      }
      return schema::input_value{std::move(out)};
   }
   if (const auto* object_value = input.as_object()) {
      auto out = schema::input_value::object_type{};
      for (const auto& [name, item] : *object_value) {
         out.emplace(name, to_schema_value(item));
      }
      return schema::input_value{std::move(out)};
   }
   return schema::input_value{};
}

value from_schema_value(const schema::input_value& input) {
   if (const auto* bool_value = std::get_if<bool>(&input.storage)) {
      return value{*bool_value};
   }
   if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
      return value{*signed_value};
   }
   if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
      return value{*unsigned_value};
   }
   if (const auto* floating_value = std::get_if<double>(&input.storage)) {
      return value{*floating_value};
   }
   if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
      return value{*string_value};
   }
   if (const auto* array_value = input.as_array()) {
      auto out = value::array_type{};
      out.reserve(array_value->size());
      for (const auto& item : *array_value) {
         out.push_back(from_schema_value(item));
      }
      return value{std::move(out)};
   }
   if (const auto* object_value = input.as_object()) {
      auto out = value::object_type{};
      for (const auto& [name, item] : *object_value) {
         out.emplace(name, from_schema_value(item));
      }
      return value{std::move(out)};
   }
   return value{};
}

std::any value_to_any(const value& input, schema::value_kind kind) {
   switch (kind) {
   case schema::value_kind::dynamic:
      return input;
   case schema::value_kind::boolean:
      if (const auto* bool_value = std::get_if<bool>(&input.storage)) {
         return *bool_value;
      }
      if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
         auto parsed = false;
         if (parse_bool_text(*string_value, parsed)) {
            return parsed;
         }
      }
      break;
   case schema::value_kind::signed_integer:
      if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
         return *signed_value;
      }
      if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
         return forge::schema::checked_integral_cast<std::int64_t>(*unsigned_value);
      }
      if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
         return forge::schema::parse_scalar_text<std::int64_t>(*string_value);
      }
      break;
   case schema::value_kind::unsigned_integer:
      if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
         return *unsigned_value;
      }
      if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
         return forge::schema::checked_integral_cast<std::uint64_t>(*signed_value);
      }
      if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
         return forge::schema::parse_scalar_text<std::uint64_t>(*string_value);
      }
      break;
   case schema::value_kind::floating:
      if (const auto* double_value = std::get_if<double>(&input.storage)) {
         return *double_value;
      }
      if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
         return static_cast<double>(*signed_value);
      }
      if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
         return static_cast<double>(*unsigned_value);
      }
      if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
         return forge::schema::parse_scalar_text<double>(*string_value);
      }
      break;
   case schema::value_kind::string:
      if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
         return *string_value;
      }
      break;
   case schema::value_kind::string_list:
      if (const auto* array = input.as_array()) {
         auto strings = std::vector<std::string>{};
         strings.reserve(array->size());
         for (const auto& entry : *array) {
            const auto* string_value = std::get_if<std::string>(&entry.storage);
            if (!string_value) {
               throw std::invalid_argument{"list entry is not a string"};
            }
            strings.push_back(*string_value);
         }
         return strings;
      }
      break;
   case schema::value_kind::object:
      if (const auto* object = input.as_object()) {
         return *object;
      }
      break;
   case schema::value_kind::object_list:
      if (const auto* array = input.as_array()) {
         for (const auto& entry : *array) {
            if (!entry.as_object()) {
               throw std::invalid_argument{"list entry is not an object"};
            }
         }
         return *array;
      }
      break;
   }
   throw std::invalid_argument{"config value has incompatible type"};
}

std::string format_decode_diagnostics(std::string_view prefix, const decode_diagnostics& diagnostics) {
   auto output = std::string{prefix};
   if (diagnostics.entries.empty()) {
      return output;
   }

   output += ": ";
   auto first = true;
   for (const auto& entry : diagnostics.entries) {
      if (!first) {
         output += "; ";
      }
      first = false;
      output += entry.path;
      output += " ";
      output += entry.code;
      output += " ";
      output += entry.message;
   }
   return output;
}

value any_to_value(schema::value_kind kind, const std::any& input) {
   switch (kind) {
   case schema::value_kind::dynamic:
      return std::any_cast<value>(input);
   case schema::value_kind::boolean:
      return schema::cast_any_to<bool>(input);
   case schema::value_kind::signed_integer:
      return schema::cast_any_to<std::int64_t>(input);
   case schema::value_kind::unsigned_integer:
      return schema::cast_any_to<std::uint64_t>(input);
   case schema::value_kind::floating:
      return schema::cast_any_to<double>(input);
   case schema::value_kind::string:
      return schema::cast_any_to<std::string>(input);
   case schema::value_kind::string_list: {
      auto array = value::array_type{};
      for (const auto& entry : schema::cast_any_to<std::vector<std::string>>(input)) {
         array.emplace_back(entry);
      }
      return array;
   }
   case schema::value_kind::object:
      return schema::cast_any_to<value::object_type>(input);
   case schema::value_kind::object_list:
      return schema::cast_any_to<value::array_type>(input);
   }
   return {};
}

} // namespace forge::config::core
