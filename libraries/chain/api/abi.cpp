module;

#include <algorithm>
#include <array>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.chain.api.abi;

import forge.codec.hex;
import forge.crypto.asymmetric;
import forge.crypto.digest.ripemd160;
import forge.crypto.digest.sha256;
import forge.crypto.digest.sha512;
import forge.raw.raw;
import forge.raw.varint;
import forge.schema.diagnostic;
import forge.schema.object;
import forge.schema.value_kind;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.multiprecision;

namespace forge::chain::api {
namespace {

std::string format_diagnostic(const abi_diagnostic& diagnostic) {
   auto result = diagnostic.message;
   result += " [type=";
   result += diagnostic.type;
   result += ", path=";
   result += diagnostic.path;
   result += ", offset=";
   result += std::to_string(diagnostic.offset);
   result += ']';
   return result;
}

[[noreturn]] void fail(abi_error_code code, std::string message, std::string_view type, std::string_view path,
                       std::size_t offset) {
   throw abi_serialization_error{abi_diagnostic{
       .code = code,
       .message = std::move(message),
       .type = std::string{type},
       .path = std::string{path},
       .offset = offset,
   }};
}

[[nodiscard]] bool is_action_decode_resource_failure(abi_error_code code) noexcept {
   switch (code) {
   case abi_error_code::recursion_limit:
   case abi_error_code::deadline_exceeded:
   case abi_error_code::size_limit:
      return true;
   default:
      return false;
   }
}

void require_action_within_limits(const protocol::action& action, const abi_serialization_limits& limits) {
   if (action.authorization.size() > limits.max_container_elements) {
      fail(abi_error_code::size_limit, "Action authorization exceeds the element limit", "action", "authorization", 0U);
   }
   if (action.data.size() > limits.max_binary_bytes) {
      fail(abi_error_code::size_limit, "Action data exceeds the binary size limit", "action", "data", 0U);
   }
}

class traversal_context {
 public:
   explicit traversal_context(abi_serialization_limits limits) : limits_{limits} {
      const auto now = std::chrono::steady_clock::now();
      const auto remaining = std::chrono::steady_clock::time_point::max() - now;
      deadline_ = limits.max_serialization_time >= remaining ? std::chrono::steady_clock::time_point::max()
                                                             : now + limits.max_serialization_time;
   }

   void check(std::size_t depth, std::string_view type, std::string_view path, std::size_t offset) const {
      if (depth > limits_.max_recursion_depth) {
         fail(abi_error_code::recursion_limit, "ABI recursion depth limit exceeded", type, path, offset);
      }
      if (std::chrono::steady_clock::now() >= deadline_) {
         fail(abi_error_code::deadline_exceeded, "ABI serialization deadline exceeded", type, path, offset);
      }
   }

   [[nodiscard]] const abi_serialization_limits& limits() const noexcept {
      return limits_;
   }

   void consume_elements(std::size_t count, std::string_view type, std::string_view path, std::size_t offset) {
      if (count > limits_.max_total_container_elements - consumed_elements_) {
         fail(abi_error_code::size_limit, "ABI containers exceed the total element limit", type, path, offset);
      }
      consumed_elements_ += count;
   }

 private:
   abi_serialization_limits limits_;
   std::chrono::steady_clock::time_point deadline_;
   std::size_t consumed_elements_ = 0;
};

class binary_writer {
 public:
   explicit binary_writer(std::size_t limit) : limit_{limit} {}

   std::size_t write(const char* data, std::size_t size) {
      if (size > limit_ - std::min(limit_, bytes_.size())) {
         fail(abi_error_code::size_limit, "ABI binary size limit exceeded", active_type_, active_path_, bytes_.size());
      }
      bytes_.insert(bytes_.end(), reinterpret_cast<const std::uint8_t*>(data),
                    reinterpret_cast<const std::uint8_t*>(data) + size);
      return size;
   }

   [[nodiscard]] std::size_t tellp() const noexcept {
      return bytes_.size();
   }

   void locate(std::string_view type, std::string_view path) noexcept {
      active_type_ = type;
      active_path_ = path;
   }

   [[nodiscard]] protocol::bytes take() && {
      return std::move(bytes_);
   }

 private:
   std::size_t limit_ = 0;
   protocol::bytes bytes_;
   std::string_view active_type_;
   std::string_view active_path_;
};

class binary_reader {
 public:
   explicit binary_reader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

   std::size_t read(char* destination, std::size_t size) {
      if (size > remaining()) {
         fail(abi_error_code::invalid_binary, "ABI binary ended unexpectedly", active_type_, active_path_, position_);
      }
      std::copy_n(bytes_.data() + position_, size, reinterpret_cast<std::uint8_t*>(destination));
      position_ += size;
      return size;
   }

   bool get(char& value) {
      read(&value, 1U);
      return true;
   }

   [[nodiscard]] std::size_t tellp() const noexcept {
      return position_;
   }

   [[nodiscard]] std::size_t remaining() const noexcept {
      return bytes_.size() - position_;
   }

   void locate(std::string_view type, std::string_view path) noexcept {
      active_type_ = type;
      active_path_ = path;
   }

 private:
   std::span<const std::uint8_t> bytes_;
   std::size_t position_ = 0;
   std::string_view active_type_;
   std::string_view active_path_;
};

template <typename T> T value_from_json(const forge::variant& value) {
   auto result = T{};
   using forge::from_variant;
   from_variant(value, result);
   return result;
}

forge::schema::input_value scalar_input(const forge::variant& value, std::string_view type, std::string_view path,
                                        std::size_t offset) {
   switch (value.get_type()) {
   case forge::variant::null_type:
      return {};
   case forge::variant::bool_type:
      return forge::schema::input_value{value.as_bool()};
   case forge::variant::int64_type:
      return forge::schema::input_value{value.as_int64()};
   case forge::variant::uint64_type:
      return forge::schema::input_value{value.as_uint64()};
   case forge::variant::double_type:
      return forge::schema::input_value{value.as_double()};
   case forge::variant::string_type:
      return forge::schema::input_value{value.get_string()};
   default:
      fail(abi_error_code::invalid_json, "ABI scalar requires a scalar JSON value", type, path, offset);
   }
}

template <typename T> forge::variant json_from_value(const T& value) {
   if constexpr (std::is_arithmetic_v<T>) {
      return forge::variant{value};
   } else {
      auto result = forge::variant{};
      using forge::to_variant;
      to_variant(value, result);
      return result;
   }
}

enum class type_form {
   scalar,
   array,
   fixed_array,
   optional,
};

struct parsed_type {
   type_form form = type_form::scalar;
   std::string_view element;
   std::size_t fixed_size = 0;
};

parsed_type parse_type(std::string_view type) {
   if (type.ends_with("[]")) {
      return {.form = type_form::array, .element = type.substr(0, type.size() - 2U)};
   }
   if (type.ends_with('?')) {
      return {.form = type_form::optional, .element = type.substr(0, type.size() - 1U)};
   }
   if (type.ends_with(']')) {
      const auto open = type.find_last_of('[');
      if (open == std::string_view::npos || open == 0U || open + 1U == type.size() - 1U) {
         return {.element = type};
      }
      auto size = std::size_t{};
      for (auto index = open + 1U; index + 1U < type.size(); ++index) {
         const auto digit = type[index];
         if (digit < '0' || digit > '9') {
            return {.element = type};
         }
         const auto value = static_cast<std::size_t>(digit - '0');
         if (size > (std::numeric_limits<std::size_t>::max() - value) / 10U) {
            return {.element = type};
         }
         size = size * 10U + value;
      }
      return {.form = type_form::fixed_array, .element = type.substr(0, open), .fixed_size = size};
   }
   return {.element = type};
}

bool has_type_modifier(std::string_view type) {
   const auto parsed = parse_type(type);
   return parsed.form != type_form::scalar || type.find_first_of("[]?") != std::string_view::npos;
}

bool is_binary_extension(std::string_view type) {
   return type.ends_with('$');
}

std::string_view without_binary_extension(std::string_view type) {
   return is_binary_extension(type) ? type.substr(0, type.size() - 1U) : type;
}

constexpr auto builtin_types = std::array{
    std::string_view{"bool"},
    std::string_view{"int8"},
    std::string_view{"uint8"},
    std::string_view{"int16"},
    std::string_view{"uint16"},
    std::string_view{"int32"},
    std::string_view{"uint32"},
    std::string_view{"int64"},
    std::string_view{"uint64"},
    std::string_view{"int128"},
    std::string_view{"uint128"},
    std::string_view{"varint32"},
    std::string_view{"varuint32"},
    std::string_view{"float32"},
    std::string_view{"float64"},
    std::string_view{"float128"},
    std::string_view{"time_point"},
    std::string_view{"time_point_sec"},
    std::string_view{"block_timestamp_type"},
    std::string_view{"name"},
    std::string_view{"bytes"},
    std::string_view{"string"},
    std::string_view{"checksum160"},
    std::string_view{"checksum256"},
    std::string_view{"checksum512"},
    std::string_view{"public_key"},
    std::string_view{"signature"},
    std::string_view{"symbol"},
    std::string_view{"symbol_code"},
    std::string_view{"asset"},
    std::string_view{"extended_asset"},
};

bool is_builtin(std::string_view type) {
   return std::ranges::find(builtin_types, type) != builtin_types.end();
}

std::string field_path(std::string_view path, std::string_view field) {
   auto result = std::string{path};
   if (!result.empty()) {
      result.push_back('.');
   }
   result += field;
   return result;
}

std::string index_path(std::string_view path, std::size_t index) {
   auto result = std::string{path};
   result.push_back('[');
   result += std::to_string(index);
   result.push_back(']');
   return result;
}

std::string variant_path(std::string_view path, std::string_view selected) {
   auto result = std::string{path};
   result.push_back('<');
   result += selected;
   result.push_back('>');
   return result;
}

class serializer {
 public:
   serializer(const protocol::abi_def& abi, traversal_context& context,
              const protocol::abi_metadata* metadata = nullptr,
              abi_json_scalar_policy policy = abi_json_scalar_policy::compatible)
       : abi_{abi}, context_{context}, scalar_policy_{policy} {
      load();
      validate();
      if (metadata != nullptr) {
         load_metadata(*metadata);
      }
   }

   void encode(std::string_view type, const forge::variant& value, binary_writer& writer) const {
      validate_type(type, type, 1U);
      encode_value(type, value, writer, std::string{type}, 1U, true);
   }

   forge::variant decode(std::string_view type, binary_reader& reader) const {
      validate_type(type, type, 1U);
      return decode_value(type, reader, std::string{type}, 1U, true);
   }

 private:
   struct field_reference {
      const protocol::struct_def* owner = nullptr;
      const protocol::field_def* field = nullptr;
   };

   struct enum_definition {
      std::string type;
      std::map<std::string, forge::variant, std::less<>> values;
      std::map<std::string, std::string, std::less<>> names;
   };

   forge::variant enum_number(std::string_view type, std::string_view text, std::string_view path) const {
      const auto signed_integer = type.starts_with("int");
      if (!signed_integer && !type.starts_with("uint")) {
         fail(abi_error_code::invalid_abi, "Enum underlying type must be a fixed-width integer", type, path, 0U);
      }
      const auto width = type.substr(signed_integer ? 3U : 4U);
      if (width != "8" && width != "16" && width != "32" && width != "64") {
         fail(abi_error_code::invalid_abi, "Enum underlying type must be a fixed-width integer", type, path, 0U);
      }
      const auto bits = width == "8" ? 8U : width == "16" ? 16U : width == "32" ? 32U : 64U;
      if (signed_integer) {
         auto number = std::int64_t{};
         const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
         const auto minimum = bits == 64U ? std::numeric_limits<std::int64_t>::min() : -(std::int64_t{1} << (bits - 1U));
         const auto maximum = bits == 64U ? std::numeric_limits<std::int64_t>::max() : (std::int64_t{1} << (bits - 1U)) - 1;
         if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number < minimum ||
             number > maximum || std::to_string(number) != text) {
            fail(abi_error_code::invalid_abi, "Invalid or out-of-range enum metadata value", type, path, 0U);
         }
         return forge::variant{number};
      }
      auto number = std::uint64_t{};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
      const auto maximum = bits == 64U ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{1} << bits) - 1U;
      if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || number > maximum ||
          std::to_string(number) != text) {
         fail(abi_error_code::invalid_abi, "Invalid or out-of-range enum metadata value", type, path, 0U);
      }
      return forge::variant{number};
   }

   void load_metadata(const protocol::abi_metadata& metadata) {
      auto bytes = std::size_t{};
      auto entries = std::size_t{};
      const auto consume = [&](std::string_view text, bool entry = false) {
         context_.check(1U, "abi_metadata", "metadata", 0U);
         if ((entry && entries++ >= context_.limits().max_metadata_entries) ||
             text.size() > context_.limits().max_string_bytes ||
             text.size() > context_.limits().max_metadata_bytes - std::min(bytes, context_.limits().max_metadata_bytes)) {
            fail(abi_error_code::size_limit, "ABI metadata exceeds the configured limit", "abi_metadata", "metadata", 0U);
         }
         bytes += text.size();
      };
      consume(metadata.version);
      if (metadata.version != "forge::abi-metadata/1.0") {
         fail(abi_error_code::invalid_abi, "Unsupported ABI metadata version", "abi_metadata", "metadata.version", 0U);
      }
      auto roots = std::set<std::string, std::less<>>{};
      auto root_types = std::set<std::string, std::less<>>{};
      for (const auto& root : metadata.roots) {
         consume(root.cpp_type, true);
         consume(root.type);
         if (root.cpp_type.empty() || !roots.insert(root.cpp_type).second || !root_types.insert(root.type).second) {
            fail(abi_error_code::duplicate_definition, "Empty or duplicate ABI metadata root", root.type, "metadata.roots", 0U);
         }
         validate_type(root.type, "metadata.roots", 1U);
      }
      for (const auto& definition : metadata.enums) {
         consume(definition.name, true);
         consume(definition.type);
         require_definition_name(definition.name, "metadata.enums", "enum");
         if (!aliases_.contains(definition.name) || resolve_alias(definition.name, "metadata.enums", 1U) != definition.type) {
            fail(abi_error_code::invalid_abi, "Enum metadata must match an existing ABI integer alias", definition.name,
                 "metadata.enums", 0U);
         }
         if (definition.values.empty()) {
            fail(abi_error_code::invalid_abi, "Enum metadata must contain named values", definition.name, "metadata.enums", 0U);
         }
         auto target = enum_definition{.type = definition.type};
         for (const auto& value : definition.values) {
            consume(value.name, true);
            consume(value.value);
            const auto number = enum_number(definition.type, value.value, "metadata.enums.values");
            if (value.name.empty() || !target.values.emplace(value.name, number).second ||
                !target.names.emplace(value.value, value.name).second) {
               fail(abi_error_code::duplicate_definition, "Empty or duplicate enum metadata name or value", definition.name,
                    "metadata.enums.values", 0U);
            }
         }
         if (!enums_.emplace(definition.name, std::move(target)).second) {
            fail(abi_error_code::duplicate_definition, "Duplicate ABI enum metadata", definition.name, "metadata.enums", 0U);
         }
      }
   }

   const enum_definition* find_enum(std::string_view type, std::string_view path, std::size_t depth) const {
      if (enums_.empty()) {
         return nullptr;
      }
      auto current = type;
      for (auto count = std::size_t{}; count <= aliases_.size(); ++count) {
         context_.check(depth + count, current, path, 0U);
         if (const auto found = enums_.find(current); found != enums_.end()) {
            return &found->second;
         }
         const auto alias = aliases_.find(current);
         if (alias == aliases_.end()) {
            return nullptr;
         }
         current = alias->second;
      }
      fail(abi_error_code::circular_definition, "Circular ABI type definition", type, path, 0U);
   }

   void load() {
      context_.check(1U, "abi_def", "abi", 0U);
      if (!abi_.version.starts_with("eosio::abi/1.")) {
         fail(abi_error_code::invalid_abi, "ABI version must start with 'eosio::abi/1.'", "abi_def", "abi.version", 0U);
      }

      for (const auto& definition : abi_.structs) {
         require_definition_name(definition.name, "abi.structs", "struct");
         if (!structs_.emplace(definition.name, &definition).second) {
            fail(abi_error_code::duplicate_definition, "Duplicate ABI struct definition", definition.name,
                 "abi.structs", 0U);
         }
      }
      for (const auto& definition : abi_.variants.value) {
         require_definition_name(definition.name, "abi.variants", "variant");
         if (!variants_.emplace(definition.name, &definition).second) {
            fail(abi_error_code::duplicate_definition, "Duplicate ABI variant definition", definition.name,
                 "abi.variants", 0U);
         }
      }
      for (const auto& definition : abi_.types) {
         require_definition_name(definition.new_type_name, "abi.types", "type");
         if (is_builtin(definition.new_type_name) || structs_.contains(definition.new_type_name) ||
             variants_.contains(definition.new_type_name) ||
             !aliases_.emplace(definition.new_type_name, definition.type).second) {
            fail(abi_error_code::duplicate_definition, "Duplicate ABI type definition", definition.new_type_name,
                 "abi.types", 0U);
         }
      }

      reject_cross_category_duplicates();
      reject_named_duplicates();
   }

   void require_definition_name(std::string_view name, std::string_view path, std::string_view kind) const {
      if (name.empty() || has_type_modifier(name) || name.ends_with('$')) {
         fail(abi_error_code::invalid_abi, "Invalid ABI " + std::string{kind} + " name", name, path, 0U);
      }
   }

   void reject_cross_category_duplicates() const {
      for (const auto& [name, _] : structs_) {
         if (is_builtin(name) || variants_.contains(name)) {
            fail(abi_error_code::duplicate_definition, "ABI definition collides with an existing type", name,
                 "abi.structs", 0U);
         }
      }
      for (const auto& [name, _] : variants_) {
         if (is_builtin(name)) {
            fail(abi_error_code::duplicate_definition, "ABI definition collides with a built-in type", name,
                 "abi.variants", 0U);
         }
      }
   }

   template <typename Range, typename Key>
   void reject_duplicate_keys(const Range& range, Key key, std::string_view path, std::string_view message) const {
      auto seen = std::set<decltype(key(*range.begin()))>{};
      for (const auto& value : range) {
         context_.check(1U, "abi_def", path, 0U);
         if (!seen.insert(key(value)).second) {
            fail(abi_error_code::duplicate_definition, std::string{message}, "abi_def", path, 0U);
         }
      }
   }

   void reject_named_duplicates() const {
      reject_duplicate_keys(
          abi_.actions, [](const auto& value) { return value.name.value; }, "abi.actions",
          "Duplicate ABI action definition");
      reject_duplicate_keys(
          abi_.tables, [](const auto& value) { return value.name.value; }, "abi.tables",
          "Duplicate ABI table definition");
      reject_duplicate_keys(
          abi_.error_messages, [](const auto& value) { return value.error_code; }, "abi.error_messages",
          "Duplicate ABI error message definition");
      reject_duplicate_keys(
          abi_.action_results.value, [](const auto& value) { return value.name.value; }, "abi.action_results",
          "Duplicate ABI action result definition");
   }

   void validate() const {
      for (const auto& [name, target] : aliases_) {
         (void)resolve_alias(name, "abi.types", 1U);
         validate_type(target, field_path("abi.types", name), 1U);
      }
      for (const auto& [name, definition] : structs_) {
         auto fields = std::vector<field_reference>{};
         auto inheritance = std::set<std::string, std::less<>>{};
         auto names = std::set<std::string, std::less<>>{};
         collect_fields(*definition, fields, inheritance, names, 1U);

         auto saw_extension = false;
         for (const auto& reference : fields) {
            const auto& field = *reference.field;
            if (field.name.empty()) {
               fail(abi_error_code::invalid_abi, "ABI field name must not be empty", name, "abi.structs", 0U);
            }
            const auto extension = is_binary_extension(field.type);
            if (saw_extension && !extension) {
               fail(abi_error_code::invalid_abi, "Non-extension field follows a binary extension field", field.type,
                    field_path(name, field.name), 0U);
            }
            saw_extension = saw_extension || extension;
            validate_type(without_binary_extension(field.type), field_path(name, field.name), 1U);
         }
      }
      for (const auto& [name, definition] : variants_) {
         if (definition->types.empty()) {
            fail(abi_error_code::invalid_abi, "ABI variant must contain at least one type", name, "abi.variants", 0U);
         }
         if (definition->types.size() > context_.limits().max_container_elements ||
             definition->types.size() > std::numeric_limits<std::uint32_t>::max()) {
            fail(abi_error_code::size_limit, "ABI variant exceeds the alternative limit", name, "abi.variants", 0U);
         }
         auto seen = std::set<std::string, std::less<>>{};
         for (const auto& type : definition->types) {
            if (!seen.insert(type).second) {
               fail(abi_error_code::duplicate_definition, "Duplicate type in ABI variant", type,
                    variant_path(name, type), 0U);
            }
            validate_type(type, variant_path(name, type), 1U);
         }
      }
      for (const auto& action : abi_.actions) {
         validate_type(action.type, field_path("abi.actions", action.name.to_string()), 1U);
      }
      for (const auto& table : abi_.tables) {
         validate_type(table.type, field_path("abi.tables", table.name.to_string()), 1U);
      }
      for (const auto& result : abi_.action_results.value) {
         validate_type(result.result_type, field_path("abi.action_results", result.name.to_string()), 1U);
      }
   }

   std::string_view resolve_alias(std::string_view type, std::string_view path, std::size_t depth) const {
      auto current = type;
      for (auto count = std::size_t{}; count <= aliases_.size(); ++count) {
         context_.check(depth + count, current, path, 0U);
         const auto found = aliases_.find(current);
         if (found == aliases_.end()) {
            return current;
         }
         current = found->second;
      }
      fail(abi_error_code::circular_definition, "Circular ABI type definition", type, path, 0U);
   }

   void validate_type(std::string_view type, std::string_view path, std::size_t depth) const {
      context_.check(depth, type, path, 0U);
      if (type.empty() || type.ends_with('$')) {
         fail(abi_error_code::unknown_type, "Unknown ABI type", type, path, 0U);
      }

      const auto resolved = resolve_alias(type, path, depth);
      const auto parsed = parse_type(resolved);
      if (parsed.form != type_form::scalar) {
         if (parsed.element.empty()) {
            fail(abi_error_code::unknown_type, "ABI container element type is empty", resolved, path, 0U);
         }
         if (parsed.form == type_form::optional &&
             parse_type(resolve_alias(parsed.element, path, depth + 1U)).form == type_form::optional) {
            fail(abi_error_code::unknown_type, "Adjacent ABI optionals cannot preserve JSON presence", resolved, path,
                 0U);
         }
         if (parsed.form == type_form::fixed_array && parsed.fixed_size > context_.limits().max_container_elements) {
            fail(abi_error_code::size_limit, "ABI fixed array exceeds the element limit", resolved, path, 0U);
         }
         validate_type(parsed.element, path, depth + 1U);
         return;
      }

      if (resolved.find_first_of("[]?") != std::string_view::npos) {
         fail(abi_error_code::unknown_type, "Malformed ABI type modifier", resolved, path, 0U);
      }
      if (!is_builtin(resolved) && !structs_.contains(resolved) && !variants_.contains(resolved)) {
         fail(abi_error_code::unknown_type, "Unknown ABI type", resolved, path, 0U);
      }
   }

   const protocol::struct_def& find_struct(std::string_view type, std::string_view path, std::size_t depth) const {
      const auto resolved = resolve_alias(type, path, depth);
      const auto found = structs_.find(resolved);
      if (found == structs_.end()) {
         fail(abi_error_code::unknown_type, "ABI struct base is not a struct", resolved, path, 0U);
      }
      return *found->second;
   }

   void collect_fields(const protocol::struct_def& definition, std::vector<field_reference>& fields,
                       std::set<std::string, std::less<>>& inheritance, std::set<std::string, std::less<>>& names,
                       std::size_t depth) const {
      context_.check(depth, definition.name, definition.name, 0U);
      if (!inheritance.insert(definition.name).second) {
         fail(abi_error_code::circular_definition, "Circular ABI struct inheritance", definition.name, definition.name,
              0U);
      }
      if (!definition.base.empty()) {
         collect_fields(find_struct(definition.base, definition.name, depth + 1U), fields, inheritance, names,
                        depth + 1U);
      }
      for (const auto& field : definition.fields) {
         if (!names.insert(field.name).second) {
            fail(abi_error_code::duplicate_definition, "Duplicate ABI field in struct inheritance", field.type,
                 field_path(definition.name, field.name), 0U);
         }
         fields.push_back(field_reference{.owner = &definition, .field = &field});
      }
      inheritance.erase(definition.name);
   }

   std::vector<field_reference> fields_for(const protocol::struct_def& definition) const {
      auto fields = std::vector<field_reference>{};
      auto inheritance = std::set<std::string, std::less<>>{};
      auto names = std::set<std::string, std::less<>>{};
      collect_fields(definition, fields, inheritance, names, 1U);
      return fields;
   }

   template <typename T>
   void pack_raw(binary_writer& writer, const T& value, std::string_view type, std::string_view path) const {
      writer.locate(type, path);
      try {
         forge::raw::pack(writer, value);
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid JSON value for ABI type: " + std::string{error.what()}, type, path,
              writer.tellp());
      }
   }

   template <typename T> T unpack_raw(binary_reader& reader, std::string_view type, std::string_view path) const {
      reader.locate(type, path);
      try {
         auto value = T{};
         forge::raw::unpack(reader, value);
         return value;
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_binary, "Invalid binary value for ABI type: " + std::string{error.what()}, type,
              path, reader.tellp());
      }
   }

   std::uint32_t unpack_length(binary_reader& reader, std::string_view type, std::string_view path, std::size_t limit,
                               std::string_view message) const {
      const auto length = unpack_raw<forge::unsigned_int>(reader, type, path).value;
      if (length > limit) {
         fail(abi_error_code::size_limit, std::string{message}, type, path, reader.tellp());
      }
      return length;
   }

   void pack_length(binary_writer& writer, std::size_t length, std::string_view type, std::string_view path) const {
      if (length > std::numeric_limits<std::uint32_t>::max()) {
         fail(abi_error_code::size_limit, "ABI length exceeds the varuint32 wire limit", type, path, writer.tellp());
      }
      pack_raw(writer, forge::unsigned_int{static_cast<std::uint32_t>(length)}, type, path);
   }

   void encode_value(std::string_view type, const forge::variant& value, binary_writer& writer, std::string_view path,
                     std::size_t depth, bool extensions_allowed) const {
      context_.check(depth, type, path, writer.tellp());
      if (const auto* definition = find_enum(type, path, depth)) {
         if (!value.is_string()) {
            fail(abi_error_code::invalid_json, "Enum JSON value must be a named string", type, path, writer.tellp());
         }
         if (value.get_string().size() > context_.limits().max_string_bytes) {
            fail(abi_error_code::size_limit, "Enum JSON name exceeds the string limit", type, path, writer.tellp());
         }
         const auto found = definition->values.find(value.get_string());
         if (found == definition->values.end()) {
            fail(abi_error_code::invalid_json, "Unknown enum JSON name", type, path, writer.tellp());
         }
         encode_builtin(definition->type, found->second, writer, path, depth);
         return;
      }
      const auto resolved = resolve_alias(type, path, depth);
      const auto parsed = parse_type(resolved);
      if (parsed.form == type_form::array || parsed.form == type_form::fixed_array) {
         if (!value.is_array()) {
            fail(abi_error_code::invalid_json, "ABI array value must be a JSON array", resolved, path, writer.tellp());
         }
         const auto& values = value.get_array();
         if (values.size() > context_.limits().max_container_elements) {
            fail(abi_error_code::size_limit, "ABI array exceeds the element limit", resolved, path, writer.tellp());
         }
         if (parsed.form == type_form::fixed_array && values.size() != parsed.fixed_size) {
            fail(abi_error_code::invalid_json, "ABI fixed array has an incorrect number of values", resolved, path,
                 writer.tellp());
         }
         context_.consume_elements(values.size(), resolved, path, writer.tellp());
         if (parsed.form == type_form::array) {
            pack_length(writer, values.size(), resolved, path);
         }
         for (auto index = std::size_t{}; index < values.size(); ++index) {
            const auto child_path = index_path(path, index);
            encode_value(parsed.element, values[index], writer, child_path, depth + 1U, false);
         }
         return;
      }
      if (parsed.form == type_form::optional) {
         const auto present = !value.is_null();
         pack_raw(writer, present, resolved, path);
         if (present) {
            encode_value(parsed.element, value, writer, path, depth + 1U, extensions_allowed);
         }
         return;
      }
      if (is_builtin(resolved)) {
         encode_builtin(resolved, value, writer, path, depth);
         return;
      }
      if (const auto found = variants_.find(resolved); found != variants_.end()) {
         encode_variant(*found->second, value, writer, path, depth, extensions_allowed);
         return;
      }
      if (const auto found = structs_.find(resolved); found != structs_.end()) {
         encode_struct(*found->second, value, writer, path, depth, extensions_allowed);
         return;
      }
      fail(abi_error_code::unknown_type, "Unknown ABI type", resolved, path, writer.tellp());
   }

   forge::variant decode_value(std::string_view type, binary_reader& reader, std::string_view path, std::size_t depth,
                               bool extensions_allowed) const {
      context_.check(depth, type, path, reader.tellp());
      if (const auto* definition = find_enum(type, path, depth)) {
         const auto number = decode_builtin(definition->type, reader, path, depth);
         const auto key = definition->type.starts_with("int") ? std::to_string(number.as_int64())
                                                               : std::to_string(number.as_uint64());
         const auto found = definition->names.find(key);
         if (found == definition->names.end()) {
            fail(abi_error_code::invalid_binary, "Unknown enum binary value", type, path, reader.tellp());
         }
         return forge::variant{found->second};
      }
      const auto resolved = resolve_alias(type, path, depth);
      const auto parsed = parse_type(resolved);
      if (parsed.form == type_form::array || parsed.form == type_form::fixed_array) {
         const auto size = parsed.form == type_form::array
                               ? static_cast<std::size_t>(unpack_length(reader, resolved, path,
                                                                        context_.limits().max_container_elements,
                                                                        "ABI array exceeds the element limit"))
                               : parsed.fixed_size;
         context_.consume_elements(size, resolved, path, reader.tellp());
         auto values = forge::variants{};
         values.reserve(std::min(size, std::size_t{1024}));
         for (auto index = std::size_t{}; index < size; ++index) {
            const auto child_path = index_path(path, index);
            values.push_back(decode_value(parsed.element, reader, child_path, depth + 1U, false));
         }
         return forge::variant{std::move(values)};
      }
      if (parsed.form == type_form::optional) {
         const auto present = unpack_raw<bool>(reader, resolved, path);
         if (!present) {
            return forge::variant{};
         }
         return decode_value(parsed.element, reader, path, depth + 1U, extensions_allowed);
      }
      if (is_builtin(resolved)) {
         return decode_builtin(resolved, reader, path, depth);
      }
      if (const auto found = variants_.find(resolved); found != variants_.end()) {
         return decode_variant(*found->second, reader, path, depth, extensions_allowed);
      }
      if (const auto found = structs_.find(resolved); found != structs_.end()) {
         return decode_struct(*found->second, reader, path, depth, extensions_allowed);
      }
      fail(abi_error_code::unknown_type, "Unknown ABI type", resolved, path, reader.tellp());
   }

   void encode_struct(const protocol::struct_def& definition, const forge::variant& value, binary_writer& writer,
                      std::string_view path, std::size_t depth, bool extensions_allowed) const {
      const auto fields = fields_for(definition);
      if (value.is_object()) {
         const auto& object = value.get_object();
         auto allowed = std::set<std::string, std::less<>>{};
         for (const auto& reference : fields) {
            allowed.insert(reference.field->name);
         }
         for (const auto& entry : object) {
            if (!allowed.contains(entry.key())) {
               fail(abi_error_code::unexpected_field, "Unexpected field in ABI JSON object", definition.name,
                    field_path(path, entry.key()), writer.tellp());
            }
         }

         for (auto index = std::size_t{}; index < fields.size(); ++index) {
            const auto& field = *fields[index].field;
            const auto child_path = field_path(path, field.name);
            const auto found = object.find(field.name);
            const auto extension = is_binary_extension(field.type);
            if (found == object.end() && extension) {
               if (!extensions_allowed) {
                  fail(abi_error_code::missing_field, "Binary extension field cannot be omitted in this ABI position",
                       field.type, child_path, writer.tellp());
               }
               for (auto later = index + 1U; later < fields.size(); ++later) {
                  if (object.find(fields[later].field->name) != object.end()) {
                     fail(abi_error_code::unexpected_field, "Field follows an omitted binary extension field",
                          fields[later].field->type, field_path(path, fields[later].field->name), writer.tellp());
                  }
               }
               return;
            }
            const auto optional = parse_type(without_binary_extension(field.type)).form == type_form::optional;
            if (found == object.end() && !optional) {
               fail(abi_error_code::missing_field, "Missing field in ABI JSON object", field.type, child_path,
                    writer.tellp());
            }
            const auto child_extensions = extensions_allowed && index + 1U == fields.size();
            encode_value(without_binary_extension(field.type),
                         found == object.end() ? forge::variant{} : found->value(), writer, child_path, depth + 1U,
                         child_extensions);
         }
         return;
      }

      if (!value.is_array()) {
         fail(abi_error_code::invalid_json, "ABI struct value must be a JSON object or array", definition.name, path,
              writer.tellp());
      }
      if (!definition.base.empty()) {
         fail(abi_error_code::invalid_json, "Derived ABI struct cannot use positional JSON array input",
              definition.name, path, writer.tellp());
      }
      const auto& values = value.get_array();
      if (values.size() > fields.size()) {
         fail(abi_error_code::unexpected_field, "Too many values in positional ABI struct input", definition.name,
              index_path(path, fields.size()), writer.tellp());
      }
      for (auto index = std::size_t{}; index < fields.size(); ++index) {
         const auto& field = *fields[index].field;
         const auto child_path = field_path(path, field.name);
         if (index >= values.size()) {
            if (is_binary_extension(field.type) && extensions_allowed) {
               return;
            }
            fail(abi_error_code::missing_field, "Positional ABI struct input ended before a required field", field.type,
                 child_path, writer.tellp());
         }
         encode_value(without_binary_extension(field.type), values[index], writer, child_path, depth + 1U,
                      extensions_allowed && index + 1U == fields.size());
      }
   }

   forge::variant decode_struct(const protocol::struct_def& definition, binary_reader& reader, std::string_view path,
                                std::size_t depth, bool extensions_allowed) const {
      const auto fields = fields_for(definition);
      auto object = forge::mutable_variant_object{};
      object.reserve(fields.size());
      for (auto index = std::size_t{}; index < fields.size(); ++index) {
         const auto& field = *fields[index].field;
         const auto extension = is_binary_extension(field.type);
         const auto child_path = field_path(path, field.name);
         if (reader.remaining() == 0U) {
            if (extension && extensions_allowed) {
               break;
            }
            fail(abi_error_code::invalid_binary, "ABI binary ended before a required struct field", field.type,
                 child_path, reader.tellp());
         }
         object.set(field.name, decode_value(without_binary_extension(field.type), reader, child_path, depth + 1U,
                                             extensions_allowed && index + 1U == fields.size()));
      }
      return forge::variant{std::move(object)};
   }

   void encode_variant(const protocol::variant_def& definition, const forge::variant& value, binary_writer& writer,
                       std::string_view path, std::size_t depth, bool extensions_allowed) const {
      if (!value.is_array() || value.size() != 2U) {
         fail(abi_error_code::invalid_variant, "ABI variant value must be a two-item JSON array", definition.name, path,
              writer.tellp());
      }
      const auto& values = value.get_array();
      if (!values[0].is_string()) {
         fail(abi_error_code::invalid_variant, "ABI variant tag must be a string", definition.name,
              index_path(path, 0U), writer.tellp());
      }
      const auto& selected = values[0].get_string();
      const auto found = std::ranges::find(definition.types, selected);
      if (found == definition.types.end()) {
         fail(abi_error_code::invalid_variant, "ABI variant tag is not declared", definition.name,
              variant_path(path, selected), writer.tellp());
      }
      const auto index = static_cast<std::size_t>(found - definition.types.begin());
      pack_raw(writer, forge::unsigned_int{index}, definition.name, path);
      encode_value(*found, values[1], writer, variant_path(path, selected), depth + 1U, extensions_allowed);
   }

   forge::variant decode_variant(const protocol::variant_def& definition, binary_reader& reader, std::string_view path,
                                 std::size_t depth, bool extensions_allowed) const {
      const auto selected = unpack_raw<forge::unsigned_int>(reader, definition.name, path).value;
      if (selected >= definition.types.size()) {
         fail(abi_error_code::invalid_variant, "ABI binary contains an invalid variant tag", definition.name, path,
              reader.tellp());
      }
      const auto& selected_type = definition.types[selected];
      return forge::variant{forge::variants{
          forge::variant{selected_type},
          decode_value(selected_type, reader, variant_path(path, selected_type), depth + 1U, extensions_allowed),
      }};
   }

   template <typename T>
   void validate_scalar(std::string_view type, const forge::variant& value, binary_writer& writer,
                        std::string_view path) const {
      if (scalar_policy_ != abi_json_scalar_policy::exact) {
         return;
      }
      if (value.is_string() && value.get_string().size() > context_.limits().max_string_bytes) {
         fail(abi_error_code::size_limit, "ABI scalar string exceeds the string limit", type, path, writer.tellp());
      }
      auto diagnostics = std::vector<forge::schema::diagnostic>{};
      forge::schema::validate_exact_input_value<T>(scalar_input(value, type, path, writer.tellp()), path, diagnostics);
      if (!diagnostics.empty()) {
         fail(abi_error_code::invalid_json, "Invalid exact ABI scalar: " + diagnostics.front().message, type, path,
              writer.tellp());
      }
   }

   template <typename T>
   void encode_converted(std::string_view type, const forge::variant& value, binary_writer& writer,
                         std::string_view path) const {
      try {
         if constexpr (forge::schema::integral_value<T> || std::is_floating_point_v<T>) {
            validate_scalar<T>(type, value, writer, path);
         } else if constexpr (std::is_same_v<T, forge::signed_int>) {
            validate_scalar<std::int32_t>(type, value, writer, path);
         } else if constexpr (std::is_same_v<T, forge::unsigned_int>) {
            validate_scalar<std::uint32_t>(type, value, writer, path);
         }
         pack_raw(writer, value_from_json<T>(value), type, path);
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid JSON value for ABI type: " + std::string{error.what()}, type, path,
              writer.tellp());
      }
   }

   template <typename T>
   forge::variant decode_converted(std::string_view type, binary_reader& reader, std::string_view path) const {
      try {
         return json_from_value(unpack_raw<T>(reader, type, path));
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_binary, "Invalid binary value for ABI type: " + std::string{error.what()}, type,
              path, reader.tellp());
      }
   }

   void encode_builtin(std::string_view type, const forge::variant& value, binary_writer& writer, std::string_view path,
                       std::size_t depth) const {
      context_.check(depth, type, path, writer.tellp());
      if (type == "bool") {
         encode_converted<bool>(type, value, writer, path);
      } else if (type == "int8") {
         encode_converted<std::int8_t>(type, value, writer, path);
      } else if (type == "uint8") {
         encode_converted<std::uint8_t>(type, value, writer, path);
      } else if (type == "int16") {
         encode_converted<std::int16_t>(type, value, writer, path);
      } else if (type == "uint16") {
         encode_converted<std::uint16_t>(type, value, writer, path);
      } else if (type == "int32") {
         encode_converted<std::int32_t>(type, value, writer, path);
      } else if (type == "uint32") {
         encode_converted<std::uint32_t>(type, value, writer, path);
      } else if (type == "int64") {
         encode_converted<std::int64_t>(type, value, writer, path);
      } else if (type == "uint64") {
         encode_converted<std::uint64_t>(type, value, writer, path);
      } else if (type == "int128") {
         encode_converted<protocol::int128_t>(type, value, writer, path);
      } else if (type == "uint128") {
         encode_converted<protocol::uint128_t>(type, value, writer, path);
      } else if (type == "varint32") {
         encode_converted<forge::signed_int>(type, value, writer, path);
      } else if (type == "varuint32") {
         encode_converted<forge::unsigned_int>(type, value, writer, path);
      } else if (type == "float32") {
         encode_converted<float>(type, value, writer, path);
      } else if (type == "float64") {
         encode_converted<double>(type, value, writer, path);
      } else if (type == "float128") {
         encode_float128(value, writer, path);
      } else if (type == "time_point") {
         encode_converted<protocol::time_point>(type, value, writer, path);
      } else if (type == "time_point_sec") {
         encode_converted<protocol::time_point_sec>(type, value, writer, path);
      } else if (type == "block_timestamp_type") {
         encode_converted<protocol::block_timestamp_type>(type, value, writer, path);
      } else if (type == "name") {
         encode_converted<protocol::name>(type, value, writer, path);
      } else if (type == "bytes") {
         encode_bytes(value, writer, path);
      } else if (type == "string") {
         encode_string(value, writer, path);
      } else if (type == "checksum160") {
         encode_converted<protocol::checksum160>(type, value, writer, path);
      } else if (type == "checksum256") {
         encode_converted<protocol::checksum256>(type, value, writer, path);
      } else if (type == "checksum512") {
         encode_converted<protocol::checksum512>(type, value, writer, path);
      } else if (type == "public_key") {
         encode_public_key(value, writer, path);
      } else if (type == "signature") {
         encode_signature(value, writer, path);
      } else if (type == "symbol") {
         encode_converted<protocol::symbol>(type, value, writer, path);
      } else if (type == "symbol_code") {
         encode_converted<protocol::symbol_code>(type, value, writer, path);
      } else if (type == "asset") {
         encode_converted<protocol::asset>(type, value, writer, path);
      } else if (type == "extended_asset") {
         encode_converted<protocol::extended_asset>(type, value, writer, path);
      } else {
         fail(abi_error_code::unknown_type, "Unknown ABI built-in type", type, path, writer.tellp());
      }
   }

   forge::variant decode_builtin(std::string_view type, binary_reader& reader, std::string_view path,
                                 std::size_t depth) const {
      context_.check(depth, type, path, reader.tellp());
      if (type == "bool") {
         return decode_converted<bool>(type, reader, path);
      }
      if (type == "int8") {
         return decode_converted<std::int8_t>(type, reader, path);
      }
      if (type == "uint8") {
         return decode_converted<std::uint8_t>(type, reader, path);
      }
      if (type == "int16") {
         return decode_converted<std::int16_t>(type, reader, path);
      }
      if (type == "uint16") {
         return decode_converted<std::uint16_t>(type, reader, path);
      }
      if (type == "int32") {
         return decode_converted<std::int32_t>(type, reader, path);
      }
      if (type == "uint32") {
         return decode_converted<std::uint32_t>(type, reader, path);
      }
      if (type == "int64") {
         return decode_converted<std::int64_t>(type, reader, path);
      }
      if (type == "uint64") {
         return decode_converted<std::uint64_t>(type, reader, path);
      }
      if (type == "int128") {
         return decode_converted<protocol::int128_t>(type, reader, path);
      }
      if (type == "uint128") {
         return decode_converted<protocol::uint128_t>(type, reader, path);
      }
      if (type == "varint32") {
         return decode_converted<forge::signed_int>(type, reader, path);
      }
      if (type == "varuint32") {
         return decode_converted<forge::unsigned_int>(type, reader, path);
      }
      if (type == "float32") {
         return decode_converted<float>(type, reader, path);
      }
      if (type == "float64") {
         return decode_converted<double>(type, reader, path);
      }
      if (type == "float128") {
         return decode_float128(reader, path);
      }
      if (type == "time_point") {
         return decode_converted<protocol::time_point>(type, reader, path);
      }
      if (type == "time_point_sec") {
         return decode_converted<protocol::time_point_sec>(type, reader, path);
      }
      if (type == "block_timestamp_type") {
         return decode_converted<protocol::block_timestamp_type>(type, reader, path);
      }
      if (type == "name") {
         return decode_converted<protocol::name>(type, reader, path);
      }
      if (type == "bytes") {
         return decode_bytes(reader, path);
      }
      if (type == "string") {
         return decode_string(reader, path);
      }
      if (type == "checksum160") {
         return decode_converted<protocol::checksum160>(type, reader, path);
      }
      if (type == "checksum256") {
         return decode_converted<protocol::checksum256>(type, reader, path);
      }
      if (type == "checksum512") {
         return decode_converted<protocol::checksum512>(type, reader, path);
      }
      if (type == "public_key") {
         return decode_public_key(reader, path);
      }
      if (type == "signature") {
         return decode_signature(reader, path);
      }
      if (type == "symbol") {
         return decode_converted<protocol::symbol>(type, reader, path);
      }
      if (type == "symbol_code") {
         return decode_converted<protocol::symbol_code>(type, reader, path);
      }
      if (type == "asset") {
         return decode_converted<protocol::asset>(type, reader, path);
      }
      if (type == "extended_asset") {
         return decode_converted<protocol::extended_asset>(type, reader, path);
      }
      fail(abi_error_code::unknown_type, "Unknown ABI built-in type", type, path, reader.tellp());
   }

   void encode_string(const forge::variant& value, binary_writer& writer, std::string_view path) const {
      validate_scalar<std::string>("string", value, writer, path);
      const auto text = value.as_string();
      if (text.size() > context_.limits().max_string_bytes) {
         fail(abi_error_code::size_limit, "ABI string exceeds the byte limit", "string", path, writer.tellp());
      }
      pack_length(writer, text.size(), "string", path);
      writer.locate("string", path);
      if (!text.empty()) {
         writer.write(text.data(), text.size());
      }
   }

   forge::variant decode_string(binary_reader& reader, std::string_view path) const {
      const auto size = unpack_length(reader, "string", path, context_.limits().max_string_bytes,
                                      "ABI string exceeds the byte limit");
      if (size > reader.remaining()) {
         fail(abi_error_code::invalid_binary, "ABI binary ended inside a string", "string", path, reader.tellp());
      }
      auto text = std::string(size, '\0');
      reader.locate("string", path);
      if (size != 0U) {
         reader.read(text.data(), text.size());
      }
      return forge::variant{std::move(text)};
   }

   void encode_bytes(const forge::variant& value, binary_writer& writer, std::string_view path) const {
      try {
         const auto& text = value.get_string();
         const auto decoded_size = text.size() / 2U + text.size() % 2U;
         if (decoded_size > context_.limits().max_string_bytes) {
            fail(abi_error_code::size_limit, "ABI bytes value exceeds the byte limit", "bytes", path, writer.tellp());
         }
         const auto bytes = forge::codec::hex::decode(text);
         pack_length(writer, bytes.size(), "bytes", path);
         writer.locate("bytes", path);
         if (!bytes.empty()) {
            writer.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
         }
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid hex string for ABI bytes: " + std::string{error.what()}, "bytes",
              path, writer.tellp());
      }
   }

   forge::variant decode_bytes(binary_reader& reader, std::string_view path) const {
      const auto size = unpack_length(reader, "bytes", path, context_.limits().max_string_bytes,
                                      "ABI bytes value exceeds the byte limit");
      if (size > reader.remaining()) {
         fail(abi_error_code::invalid_binary, "ABI binary ended inside a bytes value", "bytes", path, reader.tellp());
      }
      auto bytes = protocol::bytes(size);
      reader.locate("bytes", path);
      if (!bytes.empty()) {
         reader.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
      }
      return forge::variant{forge::codec::hex::encode(bytes)};
   }

   void encode_float128(const forge::variant& value, binary_writer& writer, std::string_view path) const {
      try {
         const auto text = value.as_string();
         if (!text.starts_with("0x") || text.size() != 34U) {
            fail(abi_error_code::invalid_json, "ABI float128 must be '0x' followed by 32 hex digits", "float128", path,
                 writer.tellp());
         }
         const auto bytes = forge::codec::hex::decode(std::string_view{text}.substr(2U));
         writer.locate("float128", path);
         writer.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid ABI float128 value: " + std::string{error.what()}, "float128",
              path, writer.tellp());
      }
   }

   forge::variant decode_float128(binary_reader& reader, std::string_view path) const {
      auto bytes = std::array<std::uint8_t, 16>{};
      reader.locate("float128", path);
      reader.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
      return forge::variant{"0x" + forge::codec::hex::encode(bytes)};
   }

   void encode_public_key(const forge::variant& value, binary_writer& writer, std::string_view path) const {
      try {
         const auto text = value.as_string();
         if (text.size() > context_.limits().max_string_bytes) {
            fail(abi_error_code::size_limit, "ABI public key text exceeds the byte limit", "public_key", path,
                 writer.tellp());
         }
         const auto key = forge::crypto::asymmetric::encoding::forge().parse_public(text);
         context_.check(1U, "public_key", path, writer.tellp());
         pack_raw(writer, key, "public_key", path);
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid ABI public key: " + std::string{error.what()}, "public_key", path,
              writer.tellp());
      }
   }

   forge::variant decode_public_key(binary_reader& reader, std::string_view path) const {
      const auto key = unpack_raw<protocol::public_key>(reader, "public_key", path);
      context_.check(1U, "public_key", path, reader.tellp());
      try {
         return forge::variant{forge::crypto::asymmetric::encoding::forge().format(key)};
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_binary, "Invalid ABI public key: " + std::string{error.what()}, "public_key",
              path, reader.tellp());
      }
   }

   void encode_signature(const forge::variant& value, binary_writer& writer, std::string_view path) const {
      try {
         const auto text = value.as_string();
         if (text.size() > context_.limits().max_string_bytes) {
            fail(abi_error_code::size_limit, "ABI signature text exceeds the byte limit", "signature", path,
                 writer.tellp());
         }
         const auto signature = forge::crypto::asymmetric::encoding::forge().parse_signature(text);
         context_.check(1U, "signature", path, writer.tellp());
         pack_raw(writer, signature, "signature", path);
      } catch (const abi_serialization_error&) {
         throw;
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_json, "Invalid ABI signature: " + std::string{error.what()}, "signature", path,
              writer.tellp());
      }
   }

   forge::variant decode_signature(binary_reader& reader, std::string_view path) const {
      const auto signature = unpack_raw<protocol::signature>(reader, "signature", path);
      context_.check(1U, "signature", path, reader.tellp());
      try {
         return forge::variant{forge::crypto::asymmetric::encoding::forge().format(signature)};
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_binary, "Invalid ABI signature: " + std::string{error.what()}, "signature", path,
              reader.tellp());
      }
   }

   const protocol::abi_def& abi_;
   traversal_context& context_;
   abi_json_scalar_policy scalar_policy_;
   std::map<std::string, enum_definition, std::less<>> enums_;
   std::map<std::string, std::string, std::less<>> aliases_;
   std::map<std::string, const protocol::struct_def*, std::less<>> structs_;
   std::map<std::string, const protocol::variant_def*, std::less<>> variants_;
};

} // namespace

abi_serialization_error::abi_serialization_error(abi_diagnostic diagnostic)
    : forge::exceptions::runtime_coded_exception<abi_error_code>{diagnostic.code, format_diagnostic(diagnostic)},
      diagnostic_{std::move(diagnostic)} {}

const abi_diagnostic& abi_serialization_error::diagnostic() const noexcept {
   return diagnostic_;
}

namespace {

protocol::bytes encode_abi(const protocol::abi_def& abi, const protocol::abi_metadata* metadata, std::string_view type,
                           const forge::variant& value, abi_serialization_limits limits,
                           abi_json_scalar_policy policy = abi_json_scalar_policy::compatible) {
   if (policy != abi_json_scalar_policy::compatible && policy != abi_json_scalar_policy::exact) {
      fail(abi_error_code::invalid_json, "Unknown ABI JSON scalar policy", type, type, 0U);
   }
   auto context = traversal_context{limits};
   auto writer = binary_writer{limits.max_binary_bytes};
   try {
      serializer{abi, context, metadata, policy}.encode(type, value, writer);
      return std::move(writer).take();
   } catch (const abi_serialization_error&) {
      throw;
   } catch (const std::exception& error) {
      fail(abi_error_code::invalid_json, "Unable to convert ABI JSON value: " + std::string{error.what()}, type, type,
           writer.tellp());
   } catch (...) {
      fail(abi_error_code::invalid_json, "Unable to convert ABI JSON value", type, type, writer.tellp());
   }
}

forge::variant decode_abi(const protocol::abi_def& abi, const protocol::abi_metadata* metadata, std::string_view type,
                          std::span<const std::uint8_t> binary, abi_serialization_limits limits) {
   if (binary.size() > limits.max_binary_bytes) {
      fail(abi_error_code::size_limit, "ABI binary size limit exceeded", type, type, 0U);
   }
   auto context = traversal_context{limits};
   auto reader = binary_reader{binary};
   try {
      auto result = serializer{abi, context, metadata}.decode(type, reader);
      if (reader.remaining() != 0U) {
         fail(abi_error_code::trailing_bytes, "ABI binary contains trailing bytes", type, type, reader.tellp());
      }
      return result;
   } catch (const abi_serialization_error&) {
      throw;
   } catch (const std::exception& error) {
      fail(abi_error_code::invalid_binary, "Unable to convert ABI binary value: " + std::string{error.what()}, type,
           type, reader.tellp());
   } catch (...) {
      fail(abi_error_code::invalid_binary, "Unable to convert ABI binary value", type, type, reader.tellp());
   }
}

} // namespace

protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, std::string_view type, const forge::variant& value,
                                abi_serialization_limits limits) {
   return encode_abi(abi, nullptr, type, value, limits);
}

protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, std::string_view type, const forge::variant& value,
                                abi_serialization_limits limits, abi_json_scalar_policy policy) {
   return encode_abi(abi, nullptr, type, value, limits, policy);
}

forge::variant abi_bin_to_json(const protocol::abi_def& abi, std::string_view type,
                               std::span<const std::uint8_t> binary, abi_serialization_limits limits) {
   return decode_abi(abi, nullptr, type, binary, limits);
}

protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                                std::string_view type, const forge::variant& value, abi_serialization_limits limits) {
   return encode_abi(abi, &metadata, type, value, limits);
}

protocol::bytes abi_json_to_bin(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                                std::string_view type, const forge::variant& value, abi_serialization_limits limits,
                                abi_json_scalar_policy policy) {
   return encode_abi(abi, &metadata, type, value, limits, policy);
}

forge::variant abi_bin_to_json(const protocol::abi_def& abi, const protocol::abi_metadata& metadata,
                               std::string_view type, std::span<const std::uint8_t> binary,
                               abi_serialization_limits limits) {
   return decode_abi(abi, &metadata, type, binary, limits);
}

forge::variant action_to_variant(const protocol::action& action, const abi_resolver& resolve,
                                 abi_serialization_limits limits) {
   require_action_within_limits(action, limits);
   const auto hex_data = forge::codec::hex::encode(action.data);
   auto data = forge::variant{hex_data};

   auto abi = std::optional<protocol::abi_def>{};
   try {
      abi = resolve(action.account);
   } catch (const abi_serialization_error&) {
      throw;
   } catch (const std::exception& error) {
      fail(abi_error_code::invalid_abi, "Unable to resolve action ABI: " + std::string{error.what()}, "abi_def",
           action.account.to_string(), 0U);
   } catch (...) {
      fail(abi_error_code::invalid_abi, "Unable to resolve action ABI", "abi_def", action.account.to_string(), 0U);
   }

   if (abi) {
      const auto definition = std::ranges::find(abi->actions, action.name, &protocol::action_def::name);
      if (definition != abi->actions.end() && !definition->type.empty()) {
         try {
            data = abi_bin_to_json(*abi, definition->type, action.data, limits);
         } catch (const abi_serialization_error& error) {
            if (is_action_decode_resource_failure(error.diagnostic().code)) {
               throw;
            }
            data = forge::variant{hex_data};
         }
      }
   }

   return forge::mutable_variant_object{}("account", action.account)("name", action.name)(
       "authorization", action.authorization)("data", std::move(data))("hex_data", hex_data);
}

forge::variant transaction_to_variant(const protocol::transaction& transaction, const abi_resolver& resolve,
                                      abi_serialization_limits limits) {
   if (transaction.context_free_actions.size() > limits.max_container_elements) {
      fail(abi_error_code::size_limit, "Transaction context-free actions exceed the element limit", "transaction",
           "context_free_actions", 0U);
   }
   if (transaction.actions.size() > limits.max_container_elements) {
      fail(abi_error_code::size_limit, "Transaction actions exceed the element limit", "transaction", "actions", 0U);
   }
   for (const auto& action : transaction.context_free_actions) {
      require_action_within_limits(action, limits);
   }
   for (const auto& action : transaction.actions) {
      require_action_within_limits(action, limits);
   }

   auto context_free_actions = forge::variants{};
   context_free_actions.reserve(transaction.context_free_actions.size());
   for (const auto& action : transaction.context_free_actions) {
      context_free_actions.push_back(action_to_variant(action, resolve, limits));
   }

   auto actions = forge::variants{};
   actions.reserve(transaction.actions.size());
   for (const auto& action : transaction.actions) {
      actions.push_back(action_to_variant(action, resolve, limits));
   }

   auto result = forge::mutable_variant_object{}("expiration", transaction.expiration)(
       "ref_block_num", transaction.ref_block_num)("ref_block_prefix", transaction.ref_block_prefix)(
       "max_net_usage_words", transaction.max_net_usage_words)("max_cpu_usage_ms", transaction.max_cpu_usage_ms)(
       "delay_sec", transaction.delay_sec)("context_free_actions", std::move(context_free_actions))("actions",
                                                                                                    std::move(actions));

   auto found_deferred_context = false;
   for (const auto& [id, encoded] : transaction.transaction_extensions) {
      if (id != protocol::deferred_transaction_generation_context::extension_id() || found_deferred_context) {
         fail(abi_error_code::invalid_binary, "Transaction contains an unsupported or duplicate extension",
              "transaction", "transaction_extensions", 0U);
      }
      try {
         result("deferred_transaction_generation",
                forge::raw::unpack_exact<protocol::deferred_transaction_generation_context>(encoded));
      } catch (const std::exception& error) {
         fail(abi_error_code::invalid_binary,
              "Transaction extension contains invalid canonical bytes: " + std::string{error.what()}, "transaction",
              "transaction_extensions", 0U);
      }
      found_deferred_context = true;
   }

   return forge::variant{std::move(result)};
}

} // namespace forge::chain::api
