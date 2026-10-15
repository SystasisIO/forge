module;

#include <glaze/glaze.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module forge.codec.json;

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.exceptions;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

namespace forge::codec::json {
namespace {

using codec_value = glz::generic_json<glz::num_mode::u64>;

constexpr glz::opts json_read_options{
    .format = glz::JSON,
    .null_terminated = true,
    .comments = false,
    .error_on_unknown_keys = false,
};

constexpr glz::opts json_lazy_options{
    .format = glz::JSON,
    .null_terminated = false,
    .comments = false,
    .error_on_unknown_keys = false,
};

constexpr glz::opts json_write_compact_options{
    .format = glz::JSON,
    .null_terminated = true,
    .comments = false,
    .error_on_unknown_keys = false,
    .skip_null_members = false,
    .prettify = false,
};

constexpr glz::opts json_write_pretty_options{
    .format = glz::JSON,
    .null_terminated = true,
    .comments = false,
    .error_on_unknown_keys = false,
    .skip_null_members = false,
    .prettify = true,
};

template <class... Ts> struct overloaded : Ts... {
   using Ts::operator()...;
};
template <class... Ts> overloaded(Ts...) -> overloaded<Ts...>;

[[nodiscard]] std::string escape_control_bytes_in_json_strings(std::string_view input) {
   constexpr auto* hex = "0123456789abcdef";
   auto output = std::string{};
   output.reserve(input.size());
   auto in_string = false;
   auto escaped = false;

   for (const auto character : input) {
      const auto byte = static_cast<unsigned char>(character);
      if (in_string && !escaped && byte < 0x20U) {
         output += "\\u00";
         output.push_back(hex[(byte >> 4U) & 0x0FU]);
         output.push_back(hex[byte & 0x0FU]);
         continue;
      }

      output.push_back(character);
      if (!in_string) {
         if (character == '"') {
            in_string = true;
         }
         continue;
      }
      if (escaped) {
         escaped = false;
         continue;
      }
      if (character == '\\') {
         escaped = true;
         continue;
      }
      if (character == '"') {
         in_string = false;
      }
   }

   return output;
}

[[nodiscard]] schema::diagnostic make_error(std::string path, std::string code, std::string message) {
   return schema::diagnostic{
       .path = std::move(path),
       .code = std::move(code),
       .level = schema::severity::error,
       .message = std::move(message),
   };
}

[[nodiscard]] std::string object_path(std::string_view path, std::string_view field) {
   if (path.empty()) {
      return std::string{field};
   }
   return std::string{path} + "." + std::string{field};
}

[[nodiscard]] std::string array_path(std::string_view path, std::size_t index) {
   return std::string{path} + "[" + std::to_string(index) + "]";
}

[[nodiscard]] std::string decode_object_key(std::string_view raw_key) {
   auto encoded = std::string{"\""};
   encoded.append(raw_key);
   encoded.push_back('"');

   auto decoded = std::string{};
   if (glz::read<json_read_options>(decoded, encoded)) {
      return std::string{raw_key};
   }
   return decoded;
}

[[nodiscard]] bool validate_unique_object_members(const glz::lazy_json_view<json_lazy_options>& input,
                                                  std::string_view path, std::size_t max_depth, std::size_t depth,
                                                  std::vector<schema::diagnostic>& diagnostics) {
   if (input.is_object()) {
      auto members = std::set<std::string>{};
      for (const auto& entry : input) {
         auto key = decode_object_key(entry.key());
         const auto entry_path = object_path(path, key);
         if (!members.insert(key).second) {
            diagnostics.push_back(make_error(entry_path, "json.duplicate", "duplicate JSON member"));
         }
         if (depth >= max_depth) {
            diagnostics.push_back(make_error(entry_path, "json.depth", "JSON input exceeds configured maximum depth"));
            return false;
         }
         if (!validate_unique_object_members(entry, entry_path, max_depth, depth + 1U, diagnostics)) {
            return false;
         }
      }
      return true;
   }
   if (input.is_array()) {
      auto index = std::size_t{0};
      for (const auto& entry : input) {
         const auto entry_path = array_path(path, index);
         if (depth >= max_depth) {
            diagnostics.push_back(make_error(entry_path, "json.depth", "JSON input exceeds configured maximum depth"));
            return false;
         }
         if (!validate_unique_object_members(entry, entry_path, max_depth, depth + 1U, diagnostics)) {
            return false;
         }
         ++index;
      }
   }
   return true;
}

[[nodiscard]] variant to_variant_value(const codec_value& input);
[[nodiscard]] config::core::value to_config_value(const codec_value& input);
[[nodiscard]] codec_value from_variant_value(const variant& input);
[[nodiscard]] codec_value from_config_value(const config::core::value& input);

[[nodiscard]] variant to_variant_value(const codec_value& input) {
   return std::visit(overloaded{
                         [](std::nullptr_t) -> variant { return variant{}; },
                         [](std::uint64_t value) -> variant { return variant{value}; },
                         [](std::int64_t value) -> variant { return variant{value}; },
                         [](double value) -> variant { return variant{value}; },
                         [](const std::string& value) -> variant { return variant{value}; },
                         [](bool value) -> variant { return variant{value}; },
                         [](const codec_value::array_t& value) -> variant {
                            auto array = variants{};
                            array.reserve(value.size());
                            for (const auto& entry : value) {
                               array.push_back(to_variant_value(entry));
                            }
                            return variant{std::move(array)};
                         },
                         [](const codec_value::object_t& value) -> variant {
                            auto object = mutable_variant_object{};
                            for (const auto& [key, entry] : value) {
                               object.set(key, to_variant_value(entry));
                            }
                            return variant{std::move(object)};
                         },
                     },
                     input.data);
}

[[nodiscard]] config::core::value to_config_value(const codec_value& input) {
   return std::visit(overloaded{
                         [](std::nullptr_t) -> config::core::value { return config::core::value{}; },
                         [](std::uint64_t value) -> config::core::value { return config::core::value{value}; },
                         [](std::int64_t value) -> config::core::value { return config::core::value{value}; },
                         [](double value) -> config::core::value { return config::core::value{value}; },
                         [](const std::string& value) -> config::core::value { return config::core::value{value}; },
                         [](bool value) -> config::core::value { return config::core::value{value}; },
                         [](const codec_value::array_t& value) -> config::core::value {
                            auto array = config::core::value::array_type{};
                            array.reserve(value.size());
                            for (const auto& entry : value) {
                               array.push_back(to_config_value(entry));
                            }
                            return config::core::value{std::move(array)};
                         },
                         [](const codec_value::object_t& value) -> config::core::value {
                            auto object = config::core::value::object_type{};
                            for (const auto& [key, entry] : value) {
                               object.emplace(key, to_config_value(entry));
                            }
                            return config::core::value{std::move(object)};
                         },
                     },
                     input.data);
}

[[nodiscard]] codec_value from_variant_value(const variant& input) {
   auto output = codec_value{};
   switch (input.get_type()) {
   case variant::null_type:
      output = nullptr;
      break;
   case variant::int64_type:
      output = input.as_int64();
      break;
   case variant::uint64_type:
      output = input.as_uint64();
      break;
   case variant::double_type:
      output = input.as_double();
      break;
   case variant::bool_type:
      output = input.as_bool();
      break;
   case variant::string_type:
      output = input.get_string();
      break;
   case variant::array_type: {
      auto array = codec_value::array_t{};
      array.reserve(input.get_array().size());
      for (const auto& entry : input.get_array()) {
         array.push_back(from_variant_value(entry));
      }
      output = std::move(array);
      break;
   }
   case variant::object_type: {
      auto object = codec_value::object_t{};
      for (const auto& entry : input.get_object()) {
         object.insert(std::make_pair(entry.key(), from_variant_value(entry.value())));
      }
      output = std::move(object);
      break;
   }
   case variant::blob_type:
      output = input.as_string();
      break;
   }
   return output;
}

[[nodiscard]] codec_value from_config_value(const config::core::value& input) {
   auto output = codec_value{};
   std::visit(overloaded{
                  [&](std::monostate) { output = nullptr; },
                  [&](bool value) { output = value; },
                  [&](std::int64_t value) { output = value; },
                  [&](std::uint64_t value) { output = value; },
                  [&](double value) { output = value; },
                  [&](const std::string& value) { output = value; },
                  [&](const config::core::value::array_type& value) {
                     auto array = codec_value::array_t{};
                     array.reserve(value.size());
                     for (const auto& entry : value) {
                        array.push_back(from_config_value(entry));
                     }
                     output = std::move(array);
                  },
                  [&](const config::core::value::object_type& value) {
                     auto object = codec_value::object_t{};
                     for (const auto& [key, entry] : value) {
                        object.insert(std::make_pair(key, from_config_value(entry)));
                     }
                     output = std::move(object);
                  },
              },
              input.storage);
   return output;
}

[[nodiscard]] bool exceeds_depth(const variant& value, std::size_t max_depth, std::size_t depth = 0) {
   if (depth > max_depth) {
      return true;
   }
   if (value.is_array()) {
      for (const auto& entry : value.get_array()) {
         if (exceeds_depth(entry, max_depth, depth + 1)) {
            return true;
         }
      }
   } else if (value.is_object()) {
      for (const auto& entry : value.get_object()) {
         if (exceeds_depth(entry.value(), max_depth, depth + 1)) {
            return true;
         }
      }
   }
   return false;
}

[[nodiscard]] bool exceeds_depth(const config::core::value& value, std::size_t max_depth, std::size_t depth = 0) {
   if (depth > max_depth) {
      return true;
   }
   if (const auto* array = value.as_array()) {
      for (const auto& entry : *array) {
         if (exceeds_depth(entry, max_depth, depth + 1)) {
            return true;
         }
      }
   } else if (const auto* object = value.as_object()) {
      for (const auto& [unused, entry] : *object) {
         if (exceeds_depth(entry, max_depth, depth + 1)) {
            return true;
         }
      }
   }
   return false;
}

[[nodiscard]] read_result<codec_value> read_codec_value(std::string_view input, const read_options& options) {
   auto result = read_result<codec_value>{};
   auto parsed = codec_value{};
   if (auto error = glz::read<json_read_options>(parsed, input)) {
      result.diagnostics.push_back(make_error(options.source_name, "json.parse", glz::format_error(error, input)));
      return result;
   }
   if (options.described_records == described_record_policy::exact) {
      auto lazy = glz::lazy_json<json_lazy_options>(input);
      if (!lazy) {
         result.diagnostics.push_back(make_error(options.source_name, "json.parse", "failed to inspect JSON input"));
         return result;
      }
      static_cast<void>(validate_unique_object_members(lazy->root(), {}, options.max_depth, 0U, result.diagnostics));
      if (!result.ok()) {
         return result;
      }
   }
   result.value = std::move(parsed);
   return result;
}

[[nodiscard]] std::string read_file_text(const std::filesystem::path& path,
                                         std::vector<schema::diagnostic>& diagnostics, std::string_view code) {
   auto input = std::ifstream{path, std::ios::binary};
   if (!input) {
      diagnostics.push_back(make_error(path.string(), std::string{code}, "failed to open file"));
      return {};
   }
   return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] bool write_file_text(const std::filesystem::path& path, std::string_view text,
                                   std::vector<schema::diagnostic>& diagnostics, std::string_view code) {
   auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
   if (!output) {
      diagnostics.push_back(make_error(path.string(), std::string{code}, "failed to open file for writing"));
      return false;
   }
   output.write(text.data(), static_cast<std::streamsize>(text.size()));
   if (!output) {
      diagnostics.push_back(make_error(path.string(), std::string{code}, "failed to write file"));
      return false;
   }
   return true;
}

[[nodiscard]] write_result write_codec_value(const codec_value& input, const write_options& options) {
   auto result = write_result{};
   if (std::chrono::system_clock::now() > options.deadline) {
      result.diagnostics.push_back(make_error({}, "json.deadline", "JSON write deadline expired"));
      return result;
   }

   auto text = std::string{};
   const auto error = options.pretty ? glz::write<json_write_pretty_options>(input, text)
                                     : glz::write<json_write_compact_options>(input, text);
   if (error) {
      result.diagnostics.push_back(make_error({}, "json.write", glz::format_error(error, text)));
      return result;
   }
   text = escape_control_bytes_in_json_strings(text);
   if (text.size() > options.max_bytes) {
      result.diagnostics.push_back(make_error({}, "json.max-bytes", "JSON output exceeds configured byte limit"));
      return result;
   }
   result.text = std::move(text);
   return result;
}

} // namespace

namespace detail {

write_result encoding_failure(const schema::encoding_error& error) {
   return write_result{
       .diagnostics = {schema::diagnostic{
           .path = error.path(),
           .code = "json.type",
           .level = schema::severity::error,
           .message = error.what(),
       }},
   };
}

write_result encoding_failure(const std::exception& error) {
   return write_result{
       .diagnostics = {schema::diagnostic{
           .path = {},
           .code = "json.type",
           .level = schema::severity::error,
           .message = error.what(),
       }},
   };
}

} // namespace detail

read_result<variant> read_value(std::string_view input, read_options options) {
   auto parsed = read_codec_value(input, options);
   auto result = read_result<variant>{};
   result.diagnostics = std::move(parsed.diagnostics);
   if (!parsed.ok()) {
      return result;
   }
   result.value = to_variant_value(parsed.value);
   if (exceeds_depth(result.value, options.max_depth)) {
      result.diagnostics.push_back(
          make_error(options.source_name, "json.depth", "JSON input exceeds configured maximum depth"));
   }
   return result;
}

write_result write_value(const variant& input, write_options options) {
   return write_codec_value(from_variant_value(input), options);
}

document parse_document(std::string_view input, document_limits limits)
{
   if (input.size() > limits.max_bytes)
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document exceeds its byte limit");
   }

   const auto decoded = read_value(input, {.max_depth = limits.max_depth});
   if (!decoded.ok())
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document is invalid");
   }

   const auto encoded = write_value(decoded.value, {.max_bytes = limits.max_bytes});
   if (!encoded.ok())
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document exceeds its byte limit");
   }

   return document{encoded.text};
}

variant document_value(const document& input, document_limits limits)
{
   if (input.text.size() > limits.max_bytes)
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document exceeds its byte limit");
   }

   auto decoded = read_value(input.text, {.max_depth = limits.max_depth});
   if (!decoded.ok())
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document is invalid");
   }

   return std::move(decoded.value);
}

void to_variant(const document& input, variant& output)
{
   output = document_value(input);
}

void from_variant(const variant& input, document& output)
{
   const auto encoded = write_value(input, {.max_bytes = document_limits{}.max_bytes});
   if (!encoded.ok())
   {
      FORGE_THROW_EXCEPTION(schema::exceptions::invalid_value, "JSON document exceeds its byte limit");
   }

   output = parse_document(encoded.text);
}

read_result<config::core::document> read_document(std::string_view input, read_options options) {
   auto parsed = read_codec_value(input, options);
   auto result = read_result<config::core::document>{};
   result.diagnostics = std::move(parsed.diagnostics);
   if (!parsed.ok()) {
      return result;
   }

   auto root = to_config_value(parsed.value);
   if (exceeds_depth(root, options.max_depth)) {
      result.diagnostics.push_back(
          make_error(options.source_name, "json.depth", "JSON input exceeds configured maximum depth"));
      return result;
   }
   const auto* object = root.as_object();
   if (!object) {
      result.diagnostics.push_back(
          make_error(options.source_name, "json.document", "JSON config document root must be an object"));
      return result;
   }
   result.value.root = *object;
   return result;
}

write_result write_document(const config::core::document& input, write_options options) {
   return write_codec_value(from_config_value(config::core::value{input.root}), options);
}

read_result<variant> load_value(const std::filesystem::path& path, read_options options) {
   auto diagnostics = std::vector<schema::diagnostic>{};
   const auto text = read_file_text(path, diagnostics, "json.io");
   if (!diagnostics.empty()) {
      return read_result<variant>{.diagnostics = std::move(diagnostics)};
   }
   if (options.source_name.empty()) {
      options.source_name = path.string();
   }
   return read_value(text, std::move(options));
}

write_result save_value(const std::filesystem::path& path, const variant& input, write_options options) {
   auto result = write_value(input, std::move(options));
   if (!result.ok()) {
      return result;
   }
   if (!write_file_text(path, result.text, result.diagnostics, "json.io")) {
      return result;
   }
   return result;
}

read_result<config::core::document> load_document(const std::filesystem::path& path, read_options options) {
   auto diagnostics = std::vector<schema::diagnostic>{};
   const auto text = read_file_text(path, diagnostics, "json.io");
   if (!diagnostics.empty()) {
      return read_result<config::core::document>{.diagnostics = std::move(diagnostics)};
   }
   if (options.source_name.empty()) {
      options.source_name = path.string();
   }
   return read_document(text, std::move(options));
}

write_result save_document(const std::filesystem::path& path, const config::core::document& input,
                           write_options options) {
   auto result = write_document(input, std::move(options));
   if (!result.ok()) {
      return result;
   }
   if (!write_file_text(path, result.text, result.diagnostics, "json.io")) {
      return result;
   }
   return result;
}

} // namespace forge::codec::json
