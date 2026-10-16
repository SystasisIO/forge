module;

#include <boost/multi_index_container.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <deque>
#include <exception>
#include <filesystem>
#include <flat_map>
#include <limits>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module forge.codec.json;

export import forge.codec.json.document;

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
import forge.reflect.reflect;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;
import forge.variant.static_variant;

import :exact;

export namespace forge::codec::json {

enum class unknown_field_policy {
   ignore,
   warn,
   error,
};

enum class described_record_policy {
   permissive,
   exact,
};

struct read_options {
   std::string source_name;
   std::size_t max_depth = 128;
   unknown_field_policy unknown_fields = unknown_field_policy::warn;
   described_record_policy described_records = described_record_policy::permissive;
};

struct write_options {
   bool pretty = false;
   std::size_t max_bytes = std::numeric_limits<std::size_t>::max();
   std::chrono::system_clock::time_point deadline = std::chrono::system_clock::time_point::max();
};

template <typename T> struct read_result {
   T value{};
   std::vector<schema::diagnostic> diagnostics;

   [[nodiscard]] bool ok() const {
      return std::ranges::none_of(
          diagnostics, [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; });
   }
};

struct write_result {
   std::string text;
   std::vector<schema::diagnostic> diagnostics;

   [[nodiscard]] bool ok() const {
      return std::ranges::none_of(
          diagnostics, [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; });
   }
};

namespace detail {

[[nodiscard]] write_result encoding_failure(const schema::encoding_error& error);
[[nodiscard]] write_result encoding_failure(const std::exception& error);

} // namespace detail

[[nodiscard]] read_result<variant> read_value(std::string_view input, read_options options = {});
[[nodiscard]] write_result write_value(const variant& input, write_options options = {});

[[nodiscard]] document parse_document(std::string_view input, document_limits limits = {});
[[nodiscard]] variant document_value(const document& input, document_limits limits = {});
void to_variant(const document& input, variant& output);
void from_variant(const variant& input, document& output);
[[nodiscard]] read_result<config::core::document> read_document(std::string_view input, read_options options = {});
[[nodiscard]] write_result write_document(const config::core::document& input, write_options options = {});

[[nodiscard]] read_result<variant> load_value(const std::filesystem::path& path, read_options options = {});
[[nodiscard]] write_result save_value(const std::filesystem::path& path, const variant& input,
                                      write_options options = {});
[[nodiscard]] read_result<config::core::document> load_document(const std::filesystem::path& path,
                                                                read_options options = {});
[[nodiscard]] write_result save_document(const std::filesystem::path& path, const config::core::document& input,
                                         write_options options = {});

namespace detail {

template <typename T>
[[nodiscard]] read_result<T> decode_schema_input(const schema::input_value& input,
                                                const schema::object_schema<T>& rules, const read_options& options) {
   auto output = read_result<T>{};
   const auto* object = input.as_object();
   if (!object) {
      output.diagnostics.push_back(schema::diagnostic{
         .path = options.source_name,
         .code = "json.document",
         .level = schema::severity::error,
         .message = "JSON config document root must be an object",
      });
      return output;
   }
   if (options.described_records == described_record_policy::exact) {
      append_schema_diagnostics(output.diagnostics, rules.validate_exact_input(*object));
      if (!output.ok()) {
         return output;
      }
   }
   rules.apply_defaults(output.value);
   for (auto entry : rules.decode_object(*object, {}, output.value)) {
      if (entry.code == "config.unknown") {
         if (options.unknown_fields == unknown_field_policy::ignore) {
            continue;
         }
         entry.code = "json.unknown";
         if (options.unknown_fields == unknown_field_policy::error) {
            entry.level = schema::severity::error;
         }
      }
      output.diagnostics.push_back(std::move(entry));
   }
   return output;
}

template <typename T>
[[nodiscard]] read_result<T> decode_variant(variant input, const schema::object_schema<T>& rules,
                                           const read_options& options) {
   auto output = read_result<T>{};
   rules.apply_defaults(output.value);

   if (options.described_records == described_record_policy::exact) {
      detail::validate_exact<T>(input, {}, output.diagnostics);
      if (!output.ok()) {
         return output;
      }
   }

   const auto schema_diagnostic_offset = output.diagnostics.size();
   detail::materialize_schema_records<T>(input, {}, output.diagnostics);
   if (options.described_records == described_record_policy::permissive) {
      auto first = output.diagnostics.begin() + static_cast<std::ptrdiff_t>(schema_diagnostic_offset);
      if (options.unknown_fields == unknown_field_policy::ignore) {
         output.diagnostics.erase(
             std::remove_if(first, output.diagnostics.end(),
                            [](const schema::diagnostic& entry) { return entry.code == "json.unknown"; }),
             output.diagnostics.end());
      } else if (options.unknown_fields == unknown_field_policy::error) {
         for (auto iterator = first; iterator != output.diagnostics.end(); ++iterator) {
            if (iterator->code == "json.unknown") {
               iterator->level = schema::severity::error;
            }
         }
      }
   }
   if (!output.ok()) {
      return output;
   }

   if constexpr (requires(const variant& source, T& target) { from_variant(source, target); }) {
      try {
         from_variant(input, output.value);
      } catch (const std::exception& error) {
         output.diagnostics.push_back(schema::diagnostic{
             .path = {},
             .code = "json.type",
             .level = schema::severity::error,
             .message = error.what(),
         });
         return output;
      }
   } else {
      output.diagnostics.push_back(schema::diagnostic{
          .path = {},
          .code = "json.type",
          .level = schema::severity::error,
          .message = "type is not readable from JSON without schema rules or forge::from_variant",
      });
      return output;
   }

   if (options.unknown_fields != unknown_field_policy::ignore && input.is_object()) {
      auto known = std::set<std::string>{};
      for (const auto& field : rules.fields()) {
         known.insert(field.name);
         known.insert(field.aliases.begin(), field.aliases.end());
      }
      if (!known.empty()) {
         for (const auto& entry : input.get_object()) {
            if (!known.contains(entry.key())) {
               output.diagnostics.push_back(schema::diagnostic{
                   .path = entry.key(),
                   .code = "json.unknown",
                   .level = options.unknown_fields == unknown_field_policy::error ? schema::severity::error
                                                                                  : schema::severity::warning,
                   .message = "unknown JSON field",
               });
            }
         }
      }
   }

   auto validation = rules.validate(output.value);
   output.diagnostics.insert(output.diagnostics.end(), validation.begin(), validation.end());
   return output;
}

} // namespace detail

// Converts an already parsed JSON value. Syntax, duplicate-member and depth checks belong to read_value/read/load.
// The value is owned because schema materialization may normalize records after exact validation.
template <typename T> [[nodiscard]] read_result<T> decode(variant input, read_options options = {}) {
   const auto rules = schema::rules<T>::define();
   if (!rules.fields().empty()) {
      try {
         return detail::decode_schema_input<T>(detail::to_schema_input(input), rules, options);
      } catch (const std::exception& error) {
         return read_result<T>{.diagnostics = {schema::diagnostic{
            .path = {},
            .code = "json.type",
            .level = schema::severity::error,
            .message = error.what(),
         }}};
      }
   }
   return detail::decode_variant<T>(std::move(input), rules, options);
}

template <typename T> [[nodiscard]] read_result<T> read(std::string_view input, read_options options = {}) {
   const auto rules = schema::rules<T>::define();
   if (!rules.fields().empty()) {
      auto parsed = read_document(input, options);
      if (!parsed.ok()) {
         return read_result<T>{.diagnostics = std::move(parsed.diagnostics)};
      }
      auto result = detail::decode_schema_input<T>(
         config::core::to_schema_value(config::core::value{parsed.value.root}), rules, options);
      result.diagnostics.insert(result.diagnostics.begin(), std::make_move_iterator(parsed.diagnostics.begin()),
                                 std::make_move_iterator(parsed.diagnostics.end()));
      return result;
   }
   auto parsed = read_value(input, options);
   if (!parsed.ok()) {
      return read_result<T>{.diagnostics = std::move(parsed.diagnostics)};
   }
   auto result = detail::decode_variant<T>(std::move(parsed.value), rules, options);
   result.diagnostics.insert(result.diagnostics.begin(), std::make_move_iterator(parsed.diagnostics.begin()),
                              std::make_move_iterator(parsed.diagnostics.end()));
   return result;
}

template <typename T> [[nodiscard]] read_result<T> load(const std::filesystem::path& path, read_options options = {}) {
   const auto rules = schema::rules<T>::define();
   if (!rules.fields().empty()) {
      auto parsed = load_document(path, options);
      if (!parsed.ok()) {
         return read_result<T>{.diagnostics = std::move(parsed.diagnostics)};
      }
      auto result = detail::decode_schema_input<T>(
         config::core::to_schema_value(config::core::value{parsed.value.root}), rules, options);
      result.diagnostics.insert(result.diagnostics.begin(), std::make_move_iterator(parsed.diagnostics.begin()),
                                 std::make_move_iterator(parsed.diagnostics.end()));
      return result;
   }
   auto parsed = load_value(path, options);
   if (!parsed.ok()) {
      return read_result<T>{.diagnostics = std::move(parsed.diagnostics)};
   }
   auto result = detail::decode_variant<T>(std::move(parsed.value), rules, options);
   result.diagnostics.insert(result.diagnostics.begin(), std::make_move_iterator(parsed.diagnostics.begin()),
                              std::make_move_iterator(parsed.diagnostics.end()));
   return result;
}

template <typename T> [[nodiscard]] write_result write(const T& input, write_options options = {}) {
   try {
      const auto rules = schema::rules<T>::define();
      if (!rules.fields().empty()) {
         return write_document(config::core::encode(input), std::move(options));
      }
      if constexpr (requires(const T& source, variant& output) { to_variant(source, output); }) {
         return write_value(detail::to_schema_aware_variant(input), std::move(options));
      } else {
         return write_result{
             .diagnostics = {schema::diagnostic{
                 .path = {},
                 .code = "json.type",
                 .level = schema::severity::error,
                 .message = "type is not writable to JSON without schema rules or forge::to_variant",
             }},
         };
      }
   } catch (const schema::encoding_error& error) {
      return detail::encoding_failure(error);
   } catch (const std::exception& error) {
      return detail::encoding_failure(error);
   }
}

template <typename T>
[[nodiscard]] write_result save(const std::filesystem::path& path, const T& input, write_options options = {}) {
   try {
      const auto rules = schema::rules<T>::define();
      if (!rules.fields().empty()) {
         return save_document(path, config::core::encode(input), std::move(options));
      }
      if constexpr (requires(const T& source, variant& output) { to_variant(source, output); }) {
         return save_value(path, detail::to_schema_aware_variant(input), std::move(options));
      } else {
         return write_result{
             .diagnostics = {schema::diagnostic{
                 .path = {},
                 .code = "json.type",
                 .level = schema::severity::error,
                 .message = "type is not writable to JSON without schema rules or forge::to_variant",
             }},
         };
      }
   } catch (const schema::encoding_error& error) {
      return detail::encoding_failure(error);
   } catch (const std::exception& error) {
      return detail::encoding_failure(error);
   }
}

} // namespace forge::codec::json

export namespace forge::schema {

template <> struct value_adapter<forge::codec::json::document> {
   static constexpr value_kind kind = value_kind::dynamic;

   [[nodiscard]] static input_value encode(const forge::codec::json::document& input);
   [[nodiscard]] static forge::codec::json::document decode(const input_value& input);
};

} // namespace forge::schema
