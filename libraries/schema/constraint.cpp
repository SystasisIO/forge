module;

#include <forge/exceptions/macros.hpp>
#include <boost/regex.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <locale>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

module forge.schema.constraint;

import forge.schema.exceptions;

namespace forge::schema {
namespace {

using object = input_value::object_type;
using array = input_value::array_type;

[[noreturn]] void unsupported(std::string_view reason) {
   FORGE_THROW_EXCEPTION(exceptions::unsupported_export, "unsupported constraint artifact",
                         forge::exceptions::ctx("reason", reason));
}

constexpr std::string_view names[] = {"any", "null", "boolean", "integer", "number", "string", "array", "object"};

bool integer(const input_value& value) {
   return std::holds_alternative<std::int64_t>(value.storage) ||
          std::holds_alternative<std::uint64_t>(value.storage);
}

int compare(const input_value& a, const input_value& b) {
   return std::visit([](const auto& left, const auto& right) -> int {
      using L = std::remove_cvref_t<decltype(left)>;
      using R = std::remove_cvref_t<decltype(right)>;
      if constexpr ((std::same_as<L, std::int64_t> || std::same_as<L, std::uint64_t>) &&
                    (std::same_as<R, std::int64_t> || std::same_as<R, std::uint64_t>)) {
         return std::cmp_less(left, right) ? -1 : std::cmp_greater(left, right) ? 1 : 0;
      } else {
         unsupported("integer bound expected");
      }
   }, a.storage, b.storage);
}

bool equal(const input_value& a, const input_value& b) {
   if (integer(a) && integer(b)) return compare(a, b) == 0;
   if (a.storage.index() != b.storage.index()) return false;
   return std::visit([&](const auto& value) -> bool {
      using T = std::remove_cvref_t<decltype(value)>;
      if constexpr (std::same_as<T, object> || std::same_as<T, array>) {
         return false; // Constants/enums in this subset are scalar only.
      } else {
         return value == std::get<T>(b.storage);
      }
   }, a.storage);
}

// Fixed-width decimal interval, preserving the bound without floating-point conversion.
std::string decimal_interval(std::string low, std::string high) {
   if (low == high) return low;
   if (low == std::string(low.size(), '0') && high == std::string(high.size(), '9'))
      return "[0-9]{" + std::to_string(low.size()) + "}";
   std::size_t prefix = 0;
   while (prefix < low.size() && low[prefix] == high[prefix]) ++prefix;
   const auto count = low.size() - prefix - 1;
   const auto suffix = [count] { return count ? "[0-9]{" + std::to_string(count) + "}" : std::string{}; };
   auto choices = std::vector<std::string>{};
   choices.push_back(std::string{low[prefix]} + decimal_interval(low.substr(prefix + 1), std::string(count, '9')));
   if (high[prefix] - low[prefix] > 1) {
      choices.push_back("[" + std::string{static_cast<char>(low[prefix] + 1)} + "-" +
                        std::string{static_cast<char>(high[prefix] - 1)} + "]" + suffix());
   }
   choices.push_back(std::string{high[prefix]} + decimal_interval(std::string(count, '0'), high.substr(prefix + 1)));
   auto output = low.substr(0, prefix) + "(";
   for (const auto& choice : choices) {
      if (output.back() != '(') output += '|';
      output += choice;
   }
   return output + ")";
}

std::string decimal_pattern(std::uint64_t minimum, std::uint64_t maximum) {
   const auto low = std::to_string(minimum);
   const auto high = std::to_string(maximum);
   auto output = std::string{"^("};
   for (auto length = low.size(); length <= high.size(); ++length) {
      auto first = length == low.size() ? low : "1" + std::string(length - 1, '0');
      auto last = length == high.size() ? high : std::string(length, '9');
      if (output.size() > 2) output += '|';
      output += decimal_interval(std::move(first), std::move(last));
   }
   return output + ")$";
}

[[noreturn]] void pattern_error(const boost::regex_error& error) {
   if (error.code() == boost::regex_constants::error_complexity || error.code() == boost::regex_constants::error_stack)
      FORGE_THROW_EXCEPTION(exceptions::constraint_limit, "constraint pattern exceeded backend resource budget");
   unsupported("pattern rejected by the regex backend");
}

auto compile_pattern(const std::string& pattern) {
   // A TU-local traits type isolates budget-sensitive template instantiations
   // from unrelated Boost.Regex consumers using different private limits.
   struct traits : boost::regex_traits<char> {};
   try {
      auto result = boost::basic_regex<char, traits>{};
      result.imbue(std::locale::classic());
      using flag_type = boost::regex_constants::syntax_option_type;
      constexpr auto flags = static_cast<flag_type>(boost::regex_constants::ECMAScript) |
         static_cast<flag_type>(boost::regbase::no_perl_ex) |
         static_cast<flag_type>(boost::regex_constants::no_mod_m) |
         static_cast<flag_type>(boost::regex_constants::no_mod_s);
      result.assign(pattern, flags);
      return result;
   } catch (const boost::regex_error& error) { pattern_error(error); }
   catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::constraint_limit, "constraint pattern allocation failed");
   }
}

void check_pattern(const std::string& pattern, std::size_t cap) {
   if (pattern.empty()) return;
   // This is an export-profile check, not a parser or a complexity analyzer.
   // No escapes/POSIX classes: the admitted syntax is shared with JSON Schema.
   if (pattern.size() > std::min(cap, std::size_t{256}) || pattern.front() != '^' || pattern.back() != '$' ||
       pattern.find('\\') != std::string::npos || pattern.find("[:") != std::string::npos ||
       pattern.find("[.") != std::string::npos || pattern.find("[=") != std::string::npos ||
       std::ranges::any_of(pattern, [](unsigned char c) { return c <= 0x20 || c >= 0x7f; }))
      unsupported("pattern is outside the anchored, escape-free ASCII export profile");
   static_cast<void>(compile_pattern(pattern));
}

bool matches_pattern(const std::string& text, const std::string& pattern) {
   static_assert(BOOST_REGEX_MAX_STATE_COUNT == 100000 && BOOST_REGEX_MAX_BLOCKS == 64 && BOOST_REGEX_BLOCKSIZE == 4096);
   const auto expression = compile_pattern(pattern);
   // Boost 1.90 uses an N*S*S estimate for random-access iterators, which can
   // exceed MAX_STATE_COUNT. A zero-copy bidirectional view selects its fixed
   // state-count limit instead. The engine still owns all matching mechanics.
   auto characters = text | std::views::filter([](char) { return true; });
   using iterator = decltype(characters.begin());
   static_assert(std::same_as<typename std::iterator_traits<iterator>::iterator_category,
                              std::bidirectional_iterator_tag>);
   try {
      return boost::regex_match(characters.begin(), characters.end(), expression);
   } catch (const boost::regex_error& error) { pattern_error(error); }
   catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::constraint_limit, "constraint pattern allocation failed");
   }
}

void bounded_value(const input_value& value, const constraint_limits& limits, std::size_t depth,
                   std::size_t& nodes) {
   if (depth > limits.max_depth || ++nodes > limits.max_nodes) unsupported("value exceeds depth/node limit");
   std::visit([&](const auto& item) {
      using T = std::remove_cvref_t<decltype(item)>;
      if constexpr (std::same_as<T, std::string>) {
         if (item.size() > limits.max_string_bytes) unsupported("string exceeds byte limit");
      } else if constexpr (std::same_as<T, array>) {
         if (item.size() > limits.max_container_size) unsupported("array exceeds element limit");
         for (const auto& child : item) bounded_value(child, limits, depth + 1, nodes);
      } else if constexpr (std::same_as<T, object>) {
         if (item.size() > limits.max_container_size) unsupported("object exceeds member limit");
         for (const auto& [name, child] : item) {
            if (name.size() > limits.max_string_bytes || name.find('\0') != std::string::npos)
               unsupported("invalid property name");
            bounded_value(child, limits, depth + 1, nodes);
         }
      } else if constexpr (std::same_as<T, double>) {
         if (!std::isfinite(item)) unsupported("non-finite number");
      }
   }, value.storage);
}

void check(const constraint& rule, const constraint_limits& limits, std::size_t depth, std::size_t& nodes) {
   if (depth > limits.max_depth || ++nodes > limits.max_nodes) unsupported("schema exceeds depth/node limit");
   if (static_cast<std::size_t>(rule.kind) >= std::size(names)) unsupported("unknown kind");
   if (rule.properties.size() > limits.max_container_size || rule.required.size() > limits.max_container_size ||
       rule.enum_values.size() > limits.max_container_size || rule.items.size() > 1) unsupported("schema too large");
   if ((!rule.properties.empty() || !rule.required.empty() || rule.additional_properties) &&
       rule.kind != constraint_kind::object) unsupported("object rules on non-object");
   if (!rule.items.empty() && rule.kind != constraint_kind::array) unsupported("items on non-array");
   if ((rule.minimum || rule.maximum) && rule.kind != constraint_kind::integer) unsupported("non-integer bounds");
   if ((rule.minimum && !integer(*rule.minimum)) || (rule.maximum && !integer(*rule.maximum)))
      unsupported("non-integer bounds");
   if (rule.minimum && rule.maximum && compare(*rule.minimum, *rule.maximum) > 0) unsupported("inverted bounds");
   if ((rule.min_size || rule.max_size) && rule.kind != constraint_kind::string &&
       rule.kind != constraint_kind::array && rule.kind != constraint_kind::object) unsupported("size on scalar");
   if (rule.min_size && rule.max_size && *rule.min_size > *rule.max_size) unsupported("inverted size bounds");
   if ((!rule.pattern.empty() || rule.decimal_minimum || rule.decimal_maximum || rule.ascii) &&
       rule.kind != constraint_kind::string) unsupported("string rules on non-string");
   if (rule.decimal_minimum.value_or(0) > rule.decimal_maximum.value_or(UINT64_MAX)) unsupported("inverted decimal bounds");
   check_pattern(rule.pattern, limits.max_string_bytes);
   if (!rule.pattern.empty() && !rule.ascii) unsupported("pattern rules require the ASCII string profile");
   auto required = std::set<std::string>{};
   for (const auto& name : rule.required)
      if (!rule.properties.contains(name) || !required.insert(name).second) unsupported("invalid required property");
   for (const auto& [name, child] : rule.properties) {
      if (name.empty() || name.size() > limits.max_string_bytes || name.find('\0') != std::string::npos)
         unsupported("invalid property name");
      check(child, limits, depth + 1, nodes);
   }
   for (const auto& child : rule.items) check(child, limits, depth + 1, nodes);
   const auto scalar = [&](const input_value& value) {
      if (value.as_object() || value.as_array()) unsupported("non-scalar enum/constant");
      bounded_value(value, limits, depth + 1, nodes);
   };
   for (const auto& value : rule.enum_values) scalar(value);
   if (rule.constant) scalar(*rule.constant);
}

bool matches(constraint_kind kind, const input_value& value) {
   switch (kind) {
   case constraint_kind::any: return true;
   case constraint_kind::null: return std::holds_alternative<std::monostate>(value.storage);
   case constraint_kind::boolean: return std::holds_alternative<bool>(value.storage);
   case constraint_kind::integer: return integer(value);
   case constraint_kind::number: return integer(value) || std::holds_alternative<double>(value.storage);
   case constraint_kind::string: return std::holds_alternative<std::string>(value.storage);
   case constraint_kind::array: return value.as_array();
   case constraint_kind::object: return value.as_object();
   }
   return false;
}

void inspect(const constraint& rule, const input_value& value, std::string path, std::vector<diagnostic>& errors) {
   const auto error = [&](std::string code, std::string message) {
      errors.push_back({.path = path, .code = std::move(code), .level = severity::error, .message = std::move(message)});
   };
   if (rule.nullable && std::holds_alternative<std::monostate>(value.storage)) return;
   if (!matches(rule.kind, value)) { error("schema.type", "unexpected JSON kind"); return; }
   if (rule.constant && !equal(value, *rule.constant)) error("schema.constant", "constant mismatch");
   if (!rule.enum_values.empty() && std::ranges::none_of(rule.enum_values, [&](const auto& v) { return equal(value, v); }))
      error("schema.enum", "value is outside enum");
   if (rule.kind == constraint_kind::any) return;
   if (integer(value)) {
      if (rule.minimum && compare(value, *rule.minimum) < 0) error("schema.range", "below minimum");
      if (rule.maximum && compare(value, *rule.maximum) > 0) error("schema.range", "above maximum");
   }
   auto size = std::optional<std::size_t>{};
   if (const auto* text = std::get_if<std::string>(&value.storage)) {
      // JSON Schema lengths count Unicode scalar values, not UTF-8 code units.
      size = std::ranges::count_if(*text, [](unsigned char c) { return (c & 0xc0) != 0x80; });
      if (rule.ascii && std::ranges::any_of(*text, [](unsigned char c) { return c > 0x7f; }))
         error("schema.ascii", "ASCII string required");
      if (!rule.pattern.empty() && !matches_pattern(*text, rule.pattern)) error("schema.pattern", "pattern mismatch");
      if (rule.decimal_minimum || rule.decimal_maximum) {
         auto number = std::uint64_t{};
         const auto [end, status] = std::from_chars(text->data(), text->data() + text->size(), number);
         if (text->empty() || (text->size() > 1 && text->front() == '0') || text->front() == '+' ||
             status != std::errc{} || end != text->data() + text->size() ||
             number < rule.decimal_minimum.value_or(0) || number > rule.decimal_maximum.value_or(UINT64_MAX))
            error("schema.decimal", "invalid canonical decimal or outside bounds");
      }
   }
   if (const auto* values = value.as_array()) {
      size = values->size();
      if (!rule.items.empty()) for (std::size_t i = 0; i < values->size(); ++i)
         inspect(rule.items.front(), (*values)[i], path + "[" + std::to_string(i) + "]", errors);
   }
   if (const auto* fields = value.as_object()) {
      size = fields->size();
      for (const auto& name : rule.required) if (!fields->contains(name))
         errors.push_back({.path = path + "." + name, .code = "schema.missing", .level = severity::error,
                           .message = "required property missing"});
      for (const auto& [name, child] : *fields) {
         const auto found = rule.properties.find(name);
         if (found != rule.properties.end()) inspect(found->second, child, path + "." + name, errors);
         else if (!rule.additional_properties)
            errors.push_back({.path = path + "." + name, .code = "schema.unknown", .level = severity::error,
                              .message = "unknown property"});
      }
   }
   if (size && rule.min_size && *size < *rule.min_size) error("schema.length", "below minimum size");
   if (size && rule.max_size && *size > *rule.max_size) error("schema.length", "above maximum size");
}

input_value encode(const constraint& rule, bool standard) {
   if (standard && rule.nullable) {
      auto non_null = rule;
      non_null.nullable = false;
      auto alternatives = array{encode(non_null, true)};
      alternatives.emplace_back(object{{"type", input_value{std::string{"null"}}}});
      return input_value{object{{"anyOf", input_value{std::move(alternatives)}}}};
   }
   auto result = object{};
   if (rule.nullable) result.emplace("nullable", true);
   if (rule.kind != constraint_kind::any) result.emplace(standard ? "type" : "kind", std::string{names[static_cast<std::size_t>(rule.kind)]});
   if (rule.kind == constraint_kind::object) {
      auto fields = object{};
      for (const auto& [name, child] : rule.properties) fields.emplace(name, encode(child, standard));
      result.emplace("properties", std::move(fields));
      auto required = array{};
      for (const auto& name : rule.required) required.emplace_back(name);
      result.emplace("required", std::move(required));
      result.emplace("additionalProperties", rule.additional_properties);
   }
   if (!rule.items.empty()) result.emplace("items", encode(rule.items.front(), standard));
   const auto size_key = [&](bool min) {
      if (rule.kind == constraint_kind::string) return min ? "minLength" : "maxLength";
      if (rule.kind == constraint_kind::array) return min ? "minItems" : "maxItems";
      return min ? "minProperties" : "maxProperties";
   };
   if (rule.min_size) result.emplace(size_key(true), static_cast<std::uint64_t>(*rule.min_size));
   if (rule.max_size) result.emplace(size_key(false), static_cast<std::uint64_t>(*rule.max_size));
   const auto strict_end = [](const std::string& pattern) {
      return "^(?:" + pattern.substr(1, pattern.size() - 2) + ")(?![\\s\\S])";
   };
   if (!rule.pattern.empty()) result.emplace("pattern", standard ? strict_end(rule.pattern) : rule.pattern);
   auto conjunction = array{};
   if (rule.ascii) {
      if (standard) conjunction.emplace_back(object{{"pattern", input_value{std::string{"^[\\x00-\\x7f]*(?![\\s\\S])"}}}});
      else result.emplace("ascii", true);
   }
   if (rule.minimum) result.emplace("minimum", *rule.minimum);
   if (rule.maximum) result.emplace("maximum", *rule.maximum);
   if (!rule.enum_values.empty()) result.emplace("enum", rule.enum_values);
   if (rule.constant) result.emplace("const", *rule.constant);
   if (rule.decimal_minimum || rule.decimal_maximum) {
      if (standard) {
         auto bound = strict_end(decimal_pattern(rule.decimal_minimum.value_or(0), rule.decimal_maximum.value_or(UINT64_MAX)));
         if (!rule.pattern.empty()) {
            conjunction.emplace_back(object{{"pattern", input_value{std::move(bound)}}});
         } else result.emplace("pattern", std::move(bound));
      } else {
         if (rule.decimal_minimum) result.emplace("decimalMinimum", *rule.decimal_minimum);
         if (rule.decimal_maximum) result.emplace("decimalMaximum", *rule.decimal_maximum);
      }
   }
   if (!conjunction.empty()) result.emplace("allOf", std::move(conjunction));
   return input_value{std::move(result)};
}

const object& as_object(const input_value& value) {
   if (const auto* fields = value.as_object()) return *fields;
   unsupported("object expected");
}

const std::string& as_string(const input_value& value) {
   if (const auto* text = std::get_if<std::string>(&value.storage)) return *text;
   unsupported("string expected");
}

std::uint64_t as_uint(const input_value& value) {
   if (const auto* n = std::get_if<std::uint64_t>(&value.storage)) return *n;
   if (const auto* n = std::get_if<std::int64_t>(&value.storage); n && *n >= 0) return static_cast<std::uint64_t>(*n);
   unsupported("unsigned integer expected");
}

constraint decode(const input_value& value) {
   const auto& fields = as_object(value);
   auto rule = constraint{};
   for (const auto& [name, item] : fields) {
      if (name == "kind") {
         const auto text = as_string(item);
         const auto found = std::ranges::find(names, text);
         if (found == std::end(names)) unsupported("unknown kind");
         rule.kind = static_cast<constraint_kind>(found - std::begin(names));
      } else if (name == "properties") {
         for (const auto& [key, child] : as_object(item)) rule.properties.emplace(key, decode(child));
      } else if (name == "required") {
         if (!item.as_array()) unsupported("required must be array");
         for (const auto& entry : *item.as_array()) rule.required.push_back(as_string(entry));
      } else if (name == "additionalProperties") {
         if (!std::holds_alternative<bool>(item.storage)) unsupported("additionalProperties must be boolean");
         rule.additional_properties = std::get<bool>(item.storage);
      } else if (name == "items") rule.items.push_back(decode(item));
      else if (name == "ascii") {
         if (!std::holds_alternative<bool>(item.storage)) unsupported("ascii must be boolean");
         rule.ascii = std::get<bool>(item.storage);
      }
      else if (name == "nullable") {
         if (!std::holds_alternative<bool>(item.storage)) unsupported("nullable must be boolean");
         rule.nullable = std::get<bool>(item.storage);
      }
      else if (name == "minLength" || name == "minItems" || name == "minProperties") rule.min_size = as_uint(item);
      else if (name == "maxLength" || name == "maxItems" || name == "maxProperties") rule.max_size = as_uint(item);
      else if (name == "pattern") rule.pattern = as_string(item);
      else if (name == "minimum") rule.minimum = item;
      else if (name == "maximum") rule.maximum = item;
      else if (name == "decimalMinimum") rule.decimal_minimum = as_uint(item);
      else if (name == "decimalMaximum") rule.decimal_maximum = as_uint(item);
      else if (name == "const") rule.constant = item;
      else if (name == "enum") {
         if (!item.as_array()) unsupported("enum must be array");
         rule.enum_values = *item.as_array();
      } else unsupported("unknown keyword (references and code are forbidden)");
   }
   const auto expected = encode(rule, false);
   // Reject alternate size spellings or duplicated semantic keywords, not silently normalize them.
   if (as_object(expected).size() != fields.size()) unsupported("non-canonical constraint keywords");
   for (const auto& [name, unused] : fields) if (!as_object(expected).contains(name)) unsupported("wrong keyword for kind");
   return rule;
}

} // namespace

void check_artifact(const artifact& input, constraint_limits limits) {
   if (input.version != 1) unsupported("unsupported version");
   auto nodes = std::size_t{};
   check(input.root, limits, 1, nodes);
}

constraint refine_constraint(const constraint& type, const constraint& refinement) {
   auto result = type;
   if (refinement.kind != constraint_kind::any && refinement.kind != type.kind)
      unsupported("refinement changes JSON kind");
   if (refinement.min_size) result.min_size = std::max(result.min_size.value_or(0), *refinement.min_size);
   if (refinement.max_size) result.max_size = std::min(result.max_size.value_or(*refinement.max_size), *refinement.max_size);
   if (refinement.minimum && (!result.minimum || compare(*result.minimum, *refinement.minimum) < 0)) result.minimum = refinement.minimum;
   if (refinement.maximum && (!result.maximum || compare(*result.maximum, *refinement.maximum) > 0)) result.maximum = refinement.maximum;
   if (!refinement.pattern.empty()) {
      if (!result.pattern.empty() && result.pattern != refinement.pattern) unsupported("multiple patterns");
      result.pattern = refinement.pattern;
   }
   if (refinement.constant) result.constant = refinement.constant;
   result.ascii = result.ascii || refinement.ascii;
   result.nullable = result.nullable || refinement.nullable;
   if (!refinement.enum_values.empty()) result.enum_values = refinement.enum_values;
   if (refinement.decimal_minimum) result.decimal_minimum = refinement.decimal_minimum;
   if (refinement.decimal_maximum) result.decimal_maximum = refinement.decimal_maximum;
   if (!refinement.items.empty()) {
      if (result.items.size() != 1) unsupported("item refinement without item type");
      result.items.front() = refine_constraint(result.items.front(), refinement.items.front());
   }
   return result;
}

std::vector<diagnostic> validate(const constraint& rules, const input_value& input, std::string_view path,
                                 constraint_limits limits) {
   auto errors = std::vector<diagnostic>{};
   check_artifact(artifact{.root = rules}, limits);
   try {
      auto nodes = std::size_t{};
      bounded_value(input, limits, 1, nodes);
   } catch (const exceptions::unsupported_export&) {
      errors.push_back({.path = std::string{path}, .code = "schema.limit", .level = severity::error,
                        .message = "value exceeds structural limits"});
      return errors;
   }
   inspect(rules, input, std::string{path}, errors);
   return errors;
}

std::vector<diagnostic> validate(const artifact& rules, const input_value& input, constraint_limits limits) {
   check_artifact(rules, limits);
   return validate(rules.root, input, "$", limits);
}

input_value json_schema(const artifact& input, constraint_limits limits) {
   check_artifact(input, limits);
   return encode(input.root, true);
}

input_value export_artifact(const artifact& input, constraint_limits limits) {
   check_artifact(input, limits);
   return input_value{object{{"version", input_value{std::uint64_t{input.version}}}, {"schema", encode(input.root, false)}}};
}

artifact load_artifact(const input_value& local_input, constraint_limits limits) {
   auto nodes = std::size_t{};
   bounded_value(local_input, limits, 1, nodes);
   const auto& fields = as_object(local_input);
   if (fields.size() != 2 || !fields.contains("version") || !fields.contains("schema") || as_uint(fields.at("version")) != 1)
      unsupported("invalid envelope/version");
   auto result = artifact{.root = decode(fields.at("schema"))};
   check_artifact(result, limits);
   return result;
}

} // namespace forge::schema
