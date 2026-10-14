module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
extern char** environ;
#endif

module forge.config.env;

import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.schema.scalar;

namespace forge::config::env {
namespace {

struct field_binding {
   std::string path;
   std::string env_name;
   std::string canonical_env_name;
   config::core::field_descriptor field;
   bool alias = false;
};

struct selected_value {
   field_binding binding;
   std::string actual_name;
   std::string value;
   config::core::source_location location;
};

struct binding_build_result {
   std::map<std::string, field_binding> bindings;
   std::vector<schema::diagnostic> diagnostics;
};

[[nodiscard]] bool supports_field(schema::value_kind kind) {
   return kind != schema::value_kind::object && kind != schema::value_kind::object_list;
}

[[nodiscard]] std::string trim(std::string_view input) {
   auto begin = std::size_t{0};
   while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])) != 0) {
      ++begin;
   }

   auto end = input.size();
   while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
      --end;
   }
   return std::string{input.substr(begin, end - begin)};
}

[[nodiscard]] std::string upper_ascii(std::string input) {
   std::ranges::transform(input, input.begin(), [](unsigned char ch) {
      return static_cast<char>(std::toupper(ch));
   });
   return input;
}

[[nodiscard]] std::string normalize_token(std::string_view input) {
   auto output = std::string{};
   auto previous_separator = false;
   for (const auto ch : input) {
      const auto uch = static_cast<unsigned char>(ch);
      if (std::isalnum(uch) != 0) {
         output.push_back(static_cast<char>(std::toupper(uch)));
         previous_separator = false;
         continue;
      }
      if (ch == '_' || ch == '-' || ch == '.' || ch == '/') {
         if (!output.empty() && !previous_separator) {
            output.push_back('_');
            previous_separator = true;
         }
      }
   }
   if (!output.empty() && output.back() == '_') {
      output.pop_back();
   }
   return output;
}

[[nodiscard]] std::string path_for(std::string_view section, std::string_view field) {
   auto output = std::string{section};
   if (!output.empty()) {
      output += ".";
   }
   output += field;
   return output;
}

[[nodiscard]] std::string env_name_for(std::string_view prefix, std::string_view section, std::string_view field) {
   auto output = normalize_token(prefix);
   const auto section_token = normalize_token(section);
   const auto field_token = normalize_token(field);
   if (!section_token.empty()) {
      output += "_";
      output += section_token;
   }
   if (!field_token.empty()) {
      output += "_";
      output += field_token;
   }
   return output;
}

[[nodiscard]] schema::diagnostic make_diagnostic(std::string path, std::string code, schema::severity level,
                                                 std::string message, config::core::source_location location = {}) {
   auto output = schema::diagnostic{
       .path = std::move(path),
       .code = std::move(code),
       .level = level,
       .message = std::move(message),
   };
   output.line = location.line;
   output.column = location.column;
   return output;
}

[[nodiscard]] bool starts_with_prefix(const std::string& name, const std::string& prefix, bool case_sensitive) {
   if (prefix.empty()) {
      return false;
   }

   const auto prefix_with_separator = prefix + "_";
   if (case_sensitive) {
      return name == prefix || name.starts_with(prefix_with_separator);
   }
   const auto normalized_name = upper_ascii(name);
   const auto normalized_prefix = upper_ascii(prefix);
   return normalized_name == normalized_prefix || normalized_name.starts_with(normalized_prefix + "_");
}

[[nodiscard]] std::string lookup_key(std::string name, bool case_sensitive) {
   return case_sensitive ? std::move(name) : upper_ascii(std::move(name));
}

void add_name_conflict(std::vector<schema::diagnostic>& diagnostics, const field_binding& existing,
                       const field_binding& candidate) {
   diagnostics.push_back(make_diagnostic(
       candidate.env_name, "env.name_conflict", schema::severity::error,
       "environment variable " + candidate.env_name + " maps to both " + existing.path + " and " + candidate.path));
}

void add_binding(binding_build_result& result, field_binding candidate, bool case_sensitive) {
   const auto key = lookup_key(candidate.env_name, case_sensitive);
   const auto found = result.bindings.find(key);
   if (found != result.bindings.end()) {
      if (found->second.path == candidate.path) {
         return;
      }
      add_name_conflict(result.diagnostics, found->second, candidate);
      return;
   }
   result.bindings.emplace(key, std::move(candidate));
}

[[nodiscard]] binding_build_result build_bindings(const config::core::component_registry& registry,
                                                  const read_options& options) {
   auto result = binding_build_result{};
   for (const auto& component : registry.components()) {
      for (const auto& field : component.fields) {
         if (!supports_field(field.kind)) {
            continue;
         }
         const auto canonical_env = env_name_for(options.prefix, component.section, field.name);
         const auto canonical_path = path_for(component.section, field.name);
         auto canonical = field_binding{
             .path = canonical_path,
             .env_name = canonical_env,
             .canonical_env_name = canonical_env,
             .field = field,
             .alias = false,
         };
         add_binding(result, std::move(canonical), options.case_sensitive);

         if (!options.allow_aliases) {
            continue;
         }
         for (const auto& alias : field.aliases) {
            const auto alias_env = env_name_for(options.prefix, component.section, alias);
            auto alias_binding = field_binding{
                .path = canonical_path,
                .env_name = alias_env,
                .canonical_env_name = canonical_env,
                .field = field,
                .alias = true,
            };
            add_binding(result, std::move(alias_binding), options.case_sensitive);
         }
      }
   }
   return result;
}

[[nodiscard]] std::vector<schema::diagnostic> validate_write_bindings(const config::core::component_registry& registry,
                                                                      const write_options& options) {
   auto result = binding_build_result{};
   for (const auto& component : registry.components()) {
      for (const auto& field : component.fields) {
         if (field.ingestion_only || !supports_field(field.kind)) {
            continue;
         }
         const auto canonical_env = env_name_for(options.prefix, component.section, field.name);
         const auto canonical_path = path_for(component.section, field.name);
         auto candidate = field_binding{
             .path = canonical_path,
             .env_name = canonical_env,
             .canonical_env_name = canonical_env,
             .field = field,
             .alias = false,
         };
         add_binding(result, std::move(candidate), false);

         for (const auto& alias : field.aliases) {
            auto alias_binding = field_binding{
                .path = canonical_path,
                .env_name = env_name_for(options.prefix, component.section, alias),
                .canonical_env_name = canonical_env,
                .field = field,
                .alias = true,
            };
            add_binding(result, std::move(alias_binding), false);
         }
      }
   }
   return result.diagnostics;
}

[[nodiscard]] bool has_error(const std::vector<schema::diagnostic>& diagnostics) {
   return std::ranges::any_of(
       diagnostics, [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; });
}

[[nodiscard]] config::core::value split_list_value(std::string_view input) {
   auto array = config::core::value::array_type{};
   if (input.empty()) {
      return array;
   }

   auto current = std::string{};
   auto escaped = false;
   for (const auto ch : input) {
      if (escaped) {
         current.push_back(ch);
         escaped = false;
         continue;
      }
      if (ch == '\\') {
         escaped = true;
         continue;
      }
      if (ch == ',') {
         array.emplace_back(current);
         current.clear();
         continue;
      }
      current.push_back(ch);
   }
   if (escaped) {
      current.push_back('\\');
   }
   array.emplace_back(current);
   return array;
}

template <typename T>
[[nodiscard]] T parse_env_scalar(std::string_view input, std::string_view message) {
   try {
      return schema::parse_scalar_text<T>(input);
   } catch (const std::invalid_argument&) {
      throw std::invalid_argument{std::string{message}};
   }
}

[[nodiscard]] config::core::value convert_value(schema::value_kind kind, std::string_view input) {
   switch (kind) {
   case schema::value_kind::boolean: {
      return parse_env_scalar<bool>(input, "expected boolean value");
   }
   case schema::value_kind::signed_integer:
      return parse_env_scalar<std::int64_t>(input, "expected signed integer value");
   case schema::value_kind::unsigned_integer:
      return parse_env_scalar<std::uint64_t>(input, "expected unsigned integer value");
   case schema::value_kind::floating:
      return parse_env_scalar<double>(input, "expected floating point value");
   case schema::value_kind::string:
      return std::string{input};
   case schema::value_kind::string_list:
      return split_list_value(input);
   case schema::value_kind::object:
      throw std::invalid_argument{"structured object environment values are not supported"};
   case schema::value_kind::object_list:
      throw std::invalid_argument{"structured object-list environment values are not supported"};
   }
   throw std::invalid_argument{"unsupported schema value kind"};
}

void add_deprecated_diagnostic(read_result<config::core::document>& result, const selected_value& selected,
                               const read_options& options) {
   if (!selected.binding.field.deprecated) {
      return;
   }
   result.diagnostics.push_back(make_diagnostic(
       selected.binding.path, "env.deprecated",
       options.allow_deprecated ? schema::severity::warning : schema::severity::error,
       selected.binding.field.deprecated_message.empty() ? "deprecated environment variable"
                                                         : selected.binding.field.deprecated_message,
       selected.location));
}

void select_value(read_result<config::core::document>& result, std::map<std::string, selected_value>& selected,
                  field_binding binding, const environment_variable& variable, const read_options& options) {
   auto candidate = selected_value{
       .binding = std::move(binding),
       .actual_name = variable.name,
       .value = variable.value,
       .location = variable.location,
   };

   const auto found = selected.find(candidate.binding.path);
   if (found == selected.end()) {
      selected.emplace(candidate.binding.path, std::move(candidate));
      return;
   }

   auto& existing = found->second;
   if (lookup_key(existing.actual_name, options.case_sensitive) == lookup_key(candidate.actual_name, options.case_sensitive)) {
      result.diagnostics.push_back(make_diagnostic(candidate.binding.path, "env.duplicate", schema::severity::warning,
                                                   "duplicate environment variable; later value wins",
                                                   candidate.location));
      existing = std::move(candidate);
      return;
   }

   if (existing.value == candidate.value) {
      return;
   }

   const auto level = options.strict_alias_conflicts ? schema::severity::error : schema::severity::warning;
   result.diagnostics.push_back(make_diagnostic(
       candidate.binding.path, "env.alias_conflict", level,
       existing.actual_name + " and " + candidate.actual_name + " both set different values", candidate.location));

   const auto candidate_is_canonical = !candidate.binding.alias;
   const auto existing_is_alias = existing.binding.alias;
   if (candidate_is_canonical && existing_is_alias) {
      existing = std::move(candidate);
   }
}

[[nodiscard]] std::optional<std::string> parse_quoted_value(std::string_view value, char quote,
                                                            std::string& parse_error) {
   if (value.size() < 2 || value.back() != quote) {
      parse_error = "unterminated quoted value";
      return std::nullopt;
   }

   const auto inner = value.substr(1, value.size() - 2);
   if (quote == '\'') {
      return std::string{inner};
   }

   auto output = std::string{};
   auto escaped = false;
   for (const auto ch : inner) {
      if (!escaped) {
         if (ch == '\\') {
            escaped = true;
         } else {
            output.push_back(ch);
         }
         continue;
      }

      switch (ch) {
      case 'n':
         output.push_back('\n');
         break;
      case 'r':
         output.push_back('\r');
         break;
      case 't':
         output.push_back('\t');
         break;
      case '\\':
      case '"':
         output.push_back(ch);
         break;
      default:
         output.push_back(ch);
         break;
      }
      escaped = false;
   }
   if (escaped) {
      output.push_back('\\');
   }
   return output;
}

[[nodiscard]] std::optional<environment_variable> parse_dotenv_line(std::string_view line, std::size_t line_number,
                                                                    const read_options& options,
                                                                    std::vector<schema::diagnostic>& diagnostics) {
   if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
   }

   auto text = trim(line);
   if (text.empty() || text.starts_with("#")) {
      return std::nullopt;
   }

   if (text.starts_with("export") && (text.size() == 6 || std::isspace(static_cast<unsigned char>(text[6])) != 0)) {
      text = trim(std::string_view{text}.substr(6));
   }

   const auto equals = text.find('=');
   if (equals == std::string::npos) {
      auto location = config::core::source_location{.source = options.source_name, .line = line_number, .column = 1};
      diagnostics.push_back(
          make_diagnostic(options.source_name, "env.parse", schema::severity::error, "expected KEY=value", location));
      return std::nullopt;
   }

   auto name = trim(std::string_view{text}.substr(0, equals));
   auto value_text = trim(std::string_view{text}.substr(equals + 1));
   if (name.empty()) {
      auto location = config::core::source_location{.source = options.source_name, .line = line_number, .column = 1};
      diagnostics.push_back(make_diagnostic(options.source_name, "env.parse", schema::severity::error,
                                            "environment variable name must not be empty", location));
      return std::nullopt;
   }

   auto value = std::string{};
   if (!value_text.empty() && (value_text.front() == '"' || value_text.front() == '\'')) {
      auto parse_error = std::string{};
      auto parsed = parse_quoted_value(value_text, value_text.front(), parse_error);
      if (!parsed) {
         auto location = config::core::source_location{.source = options.source_name, .line = line_number,
                                                .column = equals + 2};
         diagnostics.push_back(
             make_diagnostic(options.source_name, "env.parse", schema::severity::error, parse_error, location));
         return std::nullopt;
      }
      value = std::move(*parsed);
   } else {
      value = std::move(value_text);
   }

   return environment_variable{
       .name = std::move(name),
       .value = std::move(value),
       .location = {.source = options.source_name, .line = line_number, .column = 1},
   };
}

[[nodiscard]] std::vector<environment_variable> process_environment() {
   auto result = std::vector<environment_variable>{};
#if defined(_WIN32)
   auto wide_to_utf8 = [](std::wstring_view input) -> std::optional<std::string> {
      if (input.empty()) {
         return std::string{};
      }
      if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
         return std::nullopt;
      }

      const auto input_size = static_cast<int>(input.size());
      const auto required =
          WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), input_size, nullptr, 0, nullptr, nullptr);
      if (required <= 0) {
         return std::nullopt;
      }

      auto output = std::string(static_cast<std::size_t>(required), '\0');
      const auto written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(), input_size, output.data(),
                                               required, nullptr, nullptr);
      if (written != required) {
         return std::nullopt;
      }
      return output;
   };

   const auto block = GetEnvironmentStringsW();
   if (!block) {
      return result;
   }
   for (auto* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1) {
      auto wide = std::wstring{entry};
      const auto equals = wide.find(L'=');
      if (equals == std::wstring::npos || equals == 0) {
         continue;
      }
      auto name = wide_to_utf8(std::wstring_view{wide}.substr(0, equals));
      auto value = wide_to_utf8(std::wstring_view{wide}.substr(equals + 1));
      if (!name || !value) {
         continue;
      }
      result.push_back({.name = std::move(*name), .value = std::move(*value), .location = {.source = "process env"}});
   }
   FreeEnvironmentStringsW(block);
#else
   for (auto current = environ; current && *current; ++current) {
      auto entry = std::string_view{*current};
      const auto equals = entry.find('=');
      if (equals == std::string_view::npos) {
         continue;
      }
      result.push_back({
          .name = std::string{entry.substr(0, equals)},
          .value = std::string{entry.substr(equals + 1)},
          .location = {.source = "process env"},
      });
   }
#endif
   return result;
}

[[nodiscard]] std::string escape_list_entry(std::string_view input) {
   auto output = std::string{};
   for (const auto ch : input) {
      if (ch == ',' || ch == '\\') {
         output.push_back('\\');
      }
      output.push_back(ch);
   }
   return output;
}

[[nodiscard]] std::string quote_if_needed(std::string_view input) {
   if (input.empty()) {
      return {};
   }

   const auto needs_quote = std::ranges::any_of(input, [](char ch) {
      return std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '#' || ch == '"' || ch == '\\' || ch == '\n' ||
             ch == '\r' || ch == '\t';
   });
   if (!needs_quote) {
      return std::string{input};
   }

   auto output = std::string{"\""};
   for (const auto ch : input) {
      switch (ch) {
      case '\n':
         output += "\\n";
         break;
      case '\r':
         output += "\\r";
         break;
      case '\t':
         output += "\\t";
         break;
      case '\\':
      case '"':
         output.push_back('\\');
         output.push_back(ch);
         break;
      default:
         output.push_back(ch);
         break;
      }
   }
   output.push_back('"');
   return output;
}

[[nodiscard]] std::string stringify_value(const config::core::value& input) {
   if (const auto* bool_value = std::get_if<bool>(&input.storage)) {
      return *bool_value ? "true" : "false";
   }
   if (const auto* signed_value = std::get_if<std::int64_t>(&input.storage)) {
      return std::to_string(*signed_value);
   }
   if (const auto* unsigned_value = std::get_if<std::uint64_t>(&input.storage)) {
      return std::to_string(*unsigned_value);
   }
   if (const auto* double_value = std::get_if<double>(&input.storage)) {
      auto stream = std::ostringstream{};
      stream << *double_value;
      return stream.str();
   }
   if (const auto* string_value = std::get_if<std::string>(&input.storage)) {
      return quote_if_needed(*string_value);
   }
   if (const auto* array = input.as_array()) {
      auto output = std::string{};
      for (const auto& entry : *array) {
         if (!output.empty()) {
            output.push_back(',');
         }
         const auto* string_value = std::get_if<std::string>(&entry.storage);
         if (string_value) {
            output += escape_list_entry(*string_value);
         }
      }
      return output;
   }
   return {};
}

[[nodiscard]] config::core::value default_or_empty(const config::core::field_descriptor& field, const write_options& options) {
   if (field.secret) {
      return options.secret_example_placeholder;
   }
   if (options.include_defaults && field.has_default) {
      return field.default_value;
   }
   if (field.kind == schema::value_kind::string_list) {
      return config::core::value::array_type{};
   }
   return std::string{};
}

[[nodiscard]] write_result save_text(const std::filesystem::path& path, std::string text) {
   auto output = write_result{.text = std::move(text)};
   auto file = std::ofstream{path};
   if (!file) {
      output.diagnostics.push_back(make_diagnostic(path.string(), "env.save", schema::severity::error,
                                                   "failed to open env file for writing"));
      return output;
   }
   file << output.text;
   if (!file) {
      output.diagnostics.push_back(
          make_diagnostic(path.string(), "env.save", schema::severity::error, "failed to write env file"));
   }
   return output;
}

} // namespace

std::optional<std::filesystem::path> home_directory() {
   if (const auto* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
      return std::filesystem::path{home};
   }
#if defined(_WIN32)
   if (const auto* profile = std::getenv("USERPROFILE"); profile != nullptr && *profile != '\0') {
      return std::filesystem::path{profile};
   }
#endif
   return std::nullopt;
}

std::string variable_name(std::string_view section, std::string_view field, const write_options& options) {
   return env_name_for(options.prefix, section, field);
}

std::string variable_name(std::string_view section, std::string_view field, const read_options& options) {
   return env_name_for(options.prefix, section, field);
}

read_result<config::core::document> read_variables(const std::vector<environment_variable>& variables,
                                             const config::core::component_registry& registry, read_options options) {
   auto result = read_result<config::core::document>{};
   if (normalize_token(options.prefix).empty()) {
      result.diagnostics.push_back(
          make_diagnostic({}, "env.prefix", schema::severity::error, "environment prefix must not be empty"));
      return result;
   }

   auto binding_result = build_bindings(registry, options);
   result.diagnostics.insert(result.diagnostics.end(), binding_result.diagnostics.begin(),
                             binding_result.diagnostics.end());
   if (has_error(result.diagnostics)) {
      return result;
   }

   auto selected = std::map<std::string, selected_value>{};
   const auto prefix = normalize_token(options.prefix);

   for (const auto& variable : variables) {
      if (!starts_with_prefix(variable.name, prefix, options.case_sensitive)) {
         continue;
      }

      const auto found = binding_result.bindings.find(lookup_key(variable.name, options.case_sensitive));
      if (found == binding_result.bindings.end()) {
         if (options.unknown_variables != unknown_variable_policy::ignore) {
            result.diagnostics.push_back(make_diagnostic(
                variable.name, "env.unknown",
                options.unknown_variables == unknown_variable_policy::error ? schema::severity::error
                                                                            : schema::severity::warning,
                "unknown environment variable with configured prefix", variable.location));
         }
         continue;
      }

      select_value(result, selected, found->second, variable, options);
   }

   for (const auto& [path, selected_entry] : selected) {
      add_deprecated_diagnostic(result, selected_entry, options);
      try {
         result.value.set(path, convert_value(selected_entry.binding.field.kind, selected_entry.value),
                          selected_entry.location);
      } catch (const std::exception& error) {
         const auto* typed = dynamic_cast<const forge::exceptions::base*>(&error);
         result.diagnostics.push_back(make_diagnostic(
             path, "env.convert", schema::severity::error,
             typed ? typed->message() : "environment variable has an invalid value", selected_entry.location));
      }
   }

   return result;
}

read_result<config::core::document> read_document(std::string_view input, const config::core::component_registry& registry,
                                            read_options options) {
   if (options.source_name.empty()) {
      options.source_name = ".env";
   }

   auto variables = std::vector<environment_variable>{};
   auto diagnostics = std::vector<schema::diagnostic>{};
   auto seen = std::map<std::string, config::core::source_location>{};
   auto stream = std::istringstream{std::string{input}};
   auto line = std::string{};
   auto line_number = std::size_t{0};
   while (std::getline(stream, line)) {
      ++line_number;
      auto parsed = parse_dotenv_line(line, line_number, options, diagnostics);
      if (!parsed) {
         continue;
      }

      const auto key = lookup_key(parsed->name, options.case_sensitive);
      if (seen.contains(key)) {
         diagnostics.push_back(make_diagnostic(parsed->name, "env.duplicate", schema::severity::warning,
                                               "duplicate dotenv variable; later value wins", parsed->location));
      }
      seen.insert_or_assign(key, parsed->location);
      variables.push_back(std::move(*parsed));
   }

   auto result = read_variables(variables, registry, std::move(options));
   result.diagnostics.insert(result.diagnostics.begin(), diagnostics.begin(), diagnostics.end());
   return result;
}

read_result<config::core::document> load_document(const std::filesystem::path& path, const config::core::component_registry& registry,
                                            read_options options) {
   auto file = std::ifstream{path};
   if (!file) {
      auto result = read_result<config::core::document>{};
      result.diagnostics.push_back(make_diagnostic(path.string(), "env.load", schema::severity::error,
                                                   "failed to open env file for reading"));
      return result;
   }
   auto buffer = std::ostringstream{};
   buffer << file.rdbuf();
   if (options.source_name.empty()) {
      options.source_name = path.string();
   }
   return read_document(buffer.str(), registry, std::move(options));
}

read_result<config::core::document> read_process_document(const config::core::component_registry& registry, read_options options) {
   if (options.source_name.empty() || options.source_name == "env") {
      options.source_name = "process env";
   }
   auto variables = process_environment();
   for (auto& variable : variables) {
      variable.location.source = options.source_name;
   }
   return read_variables(variables, registry, std::move(options));
}

write_result write_document(const config::core::document& document, const config::core::component_registry& registry,
                            write_options options) {
   auto output = write_result{};
   if (normalize_token(options.prefix).empty()) {
      output.diagnostics.push_back(
          make_diagnostic({}, "env.prefix", schema::severity::error, "environment prefix must not be empty"));
      return output;
   }
   output.diagnostics = validate_write_bindings(registry, options);
   if (has_error(output.diagnostics)) {
      return output;
   }

   for (const auto& component : registry.components()) {
      for (const auto& field : component.fields) {
         if (field.ingestion_only || !supports_field(field.kind)) {
            continue;
         }
         const auto path = path_for(component.section, field.name);
         const auto* found = document.try_get(path);
         if (!found) {
            continue;
         }
         output.text += env_name_for(options.prefix, component.section, field.name);
         output.text += "=";
         output.text += field.secret ? options.secret_value_placeholder : stringify_value(*found);
         output.text += "\n";
      }
   }
   return output;
}

write_result write_example(const config::core::component_registry& registry, write_options options) {
   auto output = write_result{};
   if (normalize_token(options.prefix).empty()) {
      output.diagnostics.push_back(
          make_diagnostic({}, "env.prefix", schema::severity::error, "environment prefix must not be empty"));
      return output;
   }
   output.diagnostics = validate_write_bindings(registry, options);
   if (has_error(output.diagnostics)) {
      return output;
   }

   for (const auto& component : registry.components()) {
      for (const auto& field : component.fields) {
         if (field.ingestion_only || !supports_field(field.kind)) {
            continue;
         }
         if (options.include_comments && !field.description.empty()) {
            output.text += "# ";
            output.text += field.description;
            output.text += "\n";
         }
         if (options.include_comments && field.secret) {
            output.text += "# Secret value. Prefer a platform secret manager in production.\n";
         }
         output.text += env_name_for(options.prefix, component.section, field.name);
         output.text += "=";
         output.text += stringify_value(default_or_empty(field, options));
         output.text += "\n\n";
      }
   }
   return output;
}

write_result save_document(const std::filesystem::path& path, const config::core::document& document,
                           const config::core::component_registry& registry, write_options options) {
   auto written = write_document(document, registry, std::move(options));
   if (!written.ok()) {
      return written;
   }
   return save_text(path, std::move(written.text));
}

write_result save_example(const std::filesystem::path& path, const config::core::component_registry& registry,
                          write_options options) {
   auto written = write_example(registry, std::move(options));
   if (!written.ok()) {
      return written;
   }
   return save_text(path, std::move(written.text));
}

} // namespace forge::config::env
