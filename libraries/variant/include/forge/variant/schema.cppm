module;

#include <boost/multi_index_container.hpp>
#include <forge/core/macros.hpp>

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <flat_map>
#include <initializer_list>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module forge.variant.schema;

import forge.reflect.reflect;
import forge.schema.diagnostic;
import forge.schema.object;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.described;
import forge.variant.static_variant;

namespace forge::variant_schema::detail {

template <typename T> using clean_type = std::remove_cv_t<std::remove_reference_t<T>>;

template <typename T> struct optional_traits {
   static constexpr bool value = false;
};

template <typename T> struct optional_traits<std::optional<T>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T> inline constexpr bool is_optional_v = optional_traits<clean_type<T>>::value;

template <typename T> struct pointer_traits {
   static constexpr bool value = false;
};

template <typename T> struct pointer_traits<std::shared_ptr<T>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T> struct pointer_traits<std::unique_ptr<T>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T> inline constexpr bool is_pointer_v = pointer_traits<clean_type<T>>::value;

template <typename T> struct sequence_traits {
   static constexpr bool value = false;
};

template <typename T, typename Allocator> struct sequence_traits<std::vector<T, Allocator>> {
   static constexpr bool value = !std::same_as<T, char>;
   using value_type = T;
};

template <typename T, typename Allocator> struct sequence_traits<std::deque<T, Allocator>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T, std::size_t Size> struct sequence_traits<std::array<T, Size>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T, typename Compare, typename Allocator> struct sequence_traits<std::set<T, Compare, Allocator>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T, typename Hash, typename Equal, typename Allocator>
struct sequence_traits<std::unordered_set<T, Hash, Equal, Allocator>> {
   static constexpr bool value = true;
   using value_type = T;
};

template <typename T> inline constexpr bool is_sequence_v = sequence_traits<clean_type<T>>::value;

template <typename T> struct diagnostic_sequence_traits : sequence_traits<T> {};

template <typename T> struct diagnostic_sequence_traits<std::initializer_list<T>> {
   static constexpr bool value = true;
};

template <typename T> inline constexpr bool is_diagnostic_sequence_v = diagnostic_sequence_traits<clean_type<T>>::value;

template <typename T> inline constexpr bool is_unsigned_byte_vector_v = false;

template <typename Allocator>
inline constexpr bool is_unsigned_byte_vector_v<std::vector<std::uint8_t, Allocator>> = true;

template <typename T> struct multi_index_traits {
   static constexpr bool value = false;
};

template <typename Value, typename IndexSpecifierList, typename Allocator>
struct multi_index_traits<boost::multi_index_container<Value, IndexSpecifierList, Allocator>> {
   static constexpr bool value = true;
   using value_type = Value;
};

template <typename T> inline constexpr bool is_multi_index_v = multi_index_traits<clean_type<T>>::value;

template <typename T> struct pair_traits {
   static constexpr bool value = false;
};

template <typename First, typename Second> struct pair_traits<std::pair<First, Second>> {
   static constexpr bool value = true;
   using first_type = First;
   using second_type = Second;
};

template <typename T> inline constexpr bool is_pair_v = pair_traits<clean_type<T>>::value;

template <typename T> struct associative_traits {
   static constexpr bool value = false;
};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct associative_traits<std::map<Key, Value, Compare, Allocator>> {
   static constexpr bool value = true;
   using key_type = Key;
   using mapped_type = Value;
};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct associative_traits<std::multimap<Key, Value, Compare, Allocator>> {
   static constexpr bool value = true;
   using key_type = Key;
   using mapped_type = Value;
};

template <typename Key, typename Value, typename Hash, typename Equal, typename Allocator>
struct associative_traits<std::unordered_map<Key, Value, Hash, Equal, Allocator>> {
   static constexpr bool value = true;
   using key_type = Key;
   using mapped_type = Value;
};

template <typename Key, typename Value, typename Compare, typename KeyContainer, typename MappedContainer>
struct associative_traits<std::flat_map<Key, Value, Compare, KeyContainer, MappedContainer>> {
   static constexpr bool value = true;
   using key_type = Key;
   using mapped_type = Value;
};

template <typename T> inline constexpr bool is_associative_v = associative_traits<clean_type<T>>::value;

template <typename T> struct variant_traits {
   static constexpr bool value = false;
};

template <typename... T> struct variant_traits<std::variant<T...>> {
   static constexpr bool value = true;
   static constexpr std::size_t size = sizeof...(T);
};

template <typename T> inline constexpr bool is_variant_v = variant_traits<clean_type<T>>::value;

namespace adl {

void diagnostic_is_secret() = delete;
void to_variant() = delete;

struct output_reference {
   variant& value;
   operator variant&() const noexcept {
      return value;
   }
};

template <typename T> [[nodiscard]] bool is_secret(const T& input) {
   if constexpr (requires {
                    { diagnostic_is_secret(input) } -> std::convertible_to<bool>;
                 }) {
      return diagnostic_is_secret(input);
   } else {
      return false;
   }
}

template <typename T>
concept custom_conversion = requires(const T& input, variant& output) { to_variant(input, output_reference{output}); };

template <custom_conversion T> void convert(const T& input, variant& output) {
   to_variant(input, output_reference{output});
}

} // namespace adl

inline constexpr auto diagnostic_depth_limit = std::size_t{64};

struct diagnostic_limit {};

struct diagnostic_identity {
   const void* address = nullptr;
   const void* type = nullptr;
   bool operator==(const diagnostic_identity&) const = default;
};

template <typename T> inline constexpr auto diagnostic_type_key = char{};

struct diagnostic_context {
   std::size_t remaining = MAX_NUM_ARRAY_ELEMENTS;
   bool exhausted = false;
   std::array<diagnostic_identity, diagnostic_depth_limit> path{};
   std::size_t path_size = 0;

   constexpr void consume() {
      if (remaining == 0) {
         exhausted = true;
         throw diagnostic_limit{};
      }
      --remaining;
   }

   [[nodiscard]] constexpr bool contains(diagnostic_identity identity) const noexcept {
      for (std::size_t index = 0; index < path_size; ++index) {
         if (path[index] == identity) {
            return true;
         }
      }
      return false;
   }
};

struct diagnostic_pointer_scope {
   diagnostic_context& context;

   constexpr diagnostic_pointer_scope(diagnostic_context& current, diagnostic_identity identity) : context{current} {
      if (context.path_size == context.path.size()) {
         throw diagnostic_limit{};
      }
      context.path[context.path_size++] = identity;
   }

   diagnostic_pointer_scope(const diagnostic_pointer_scope&) = delete;
   diagnostic_pointer_scope& operator=(const diagnostic_pointer_scope&) = delete;

   constexpr ~diagnostic_pointer_scope() {
      --context.path_size;
   }
};

template <typename Pointer> [[nodiscard]] diagnostic_identity pointer_identity(const Pointer& input) {
   using value_type = clean_type<typename pointer_traits<clean_type<Pointer>>::value_type>;
   return {input.get(), std::addressof(diagnostic_type_key<value_type>)};
}

template <typename T, typename Visitor> void for_each_diagnostic_member(const T& input, Visitor&& visitor) {
   if constexpr (reflect::is_described_object_v<T>) {
      reflect::for_each_member<T>([&](const char* name, auto member) { visitor(name, input.*member); });
   } else if constexpr (reflect::is_diagnostic_aggregate_v<T>) {
      reflect::for_each_aggregate_member(input, std::forward<Visitor>(visitor));
   }
}

template <typename T>
[[nodiscard]] bool requires_diagnostic_walk(const T& input, std::size_t depth, diagnostic_context& context) {
   using value_type = clean_type<T>;
   context.consume();
   if (depth >= diagnostic_depth_limit || adl::is_secret(input)) {
      return true;
   }
   if constexpr (is_optional_v<value_type>) {
      return input && requires_diagnostic_walk(*input, depth + 1, context);
   } else if constexpr (is_pointer_v<value_type>) {
      if (!input) {
         return false;
      }
      const auto identity = pointer_identity(input);
      if (context.contains(identity)) {
         return true;
      }
      const auto scope = diagnostic_pointer_scope{context, identity};
      return requires_diagnostic_walk(*input, depth + 1, context);
   } else if constexpr (is_variant_v<value_type>) {
      return std::visit([&](const auto& value) { return requires_diagnostic_walk(value, depth + 1, context); }, input);
   } else if constexpr (is_diagnostic_sequence_v<value_type> || is_multi_index_v<value_type> ||
                        is_associative_v<value_type>) {
      for (const auto& entry : input) {
         if (requires_diagnostic_walk(entry, depth + 1, context)) {
            return true;
         }
      }
      return false;
   } else if constexpr (is_pair_v<value_type>) {
      return requires_diagnostic_walk(input.first, depth + 1, context) ||
             requires_diagnostic_walk(input.second, depth + 1, context);
   } else if constexpr (std::is_class_v<value_type> && !std::same_as<value_type, variant>) {
      if (schema::has_explicit_rules_v<value_type> || !schema::rules<value_type>::define().fields().empty()) {
         return true;
      }
      auto result = false;
      for_each_diagnostic_member(input, [&](std::string_view, const auto& member) {
         if (!result) {
            result = requires_diagnostic_walk(member, depth + 1, context);
         }
      });
      return result;
   } else {
      return false;
   }
}

[[nodiscard]] inline std::string field_path(std::string_view path, std::string_view field) {
   if (path.empty()) {
      return std::string{field};
   }
   return std::string{path} + "." + std::string{field};
}

[[nodiscard]] inline std::string element_path(std::string_view path, std::size_t index) {
   return std::string{path} + "[" + std::to_string(index) + "]";
}

[[nodiscard]] inline schema::input_value to_schema_input(const variant& source) {
   switch (source.get_type()) {
   case variant::null_type:
      return {};
   case variant::int64_type:
      return schema::input_value{source.as_int64()};
   case variant::uint64_type:
      return schema::input_value{source.as_uint64()};
   case variant::double_type:
      return schema::input_value{source.as_double()};
   case variant::bool_type:
      return schema::input_value{source.as_bool()};
   case variant::string_type:
      return schema::input_value{source.get_string()};
   case variant::array_type: {
      auto output = schema::input_value::array_type{};
      output.reserve(source.get_array().size());
      for (const auto& entry : source.get_array()) {
         output.push_back(to_schema_input(entry));
      }
      return schema::input_value{std::move(output)};
   }
   case variant::object_type: {
      auto output = schema::input_value::object_type{};
      for (const auto& entry : source.get_object()) {
         output.emplace(entry.key(), to_schema_input(entry.value()));
      }
      return schema::input_value{std::move(output)};
   }
   case variant::blob_type:
      throw std::invalid_argument{"schema records cannot contain blob values"};
   }
   throw std::invalid_argument{"unsupported schema value"};
}

[[nodiscard]] inline variant from_schema_input(const schema::input_value& source) {
   return std::visit(
       []<typename Value>(const Value& value) -> variant {
          using value_type = clean_type<Value>;
          if constexpr (std::same_as<value_type, std::monostate>) {
             return {};
          } else if constexpr (std::same_as<value_type, schema::input_value::array_type>) {
             auto output = variants{};
             output.reserve(value.size());
             for (const auto& entry : value) {
                output.push_back(from_schema_input(entry));
             }
             return variant{std::move(output)};
          } else if constexpr (std::same_as<value_type, schema::input_value::object_type>) {
             auto output = mutable_variant_object{};
             for (const auto& [name, entry] : value) {
                output.set(name, from_schema_input(entry));
             }
             return variant{std::move(output)};
          } else {
             return variant{value};
          }
       },
       source.storage);
}

template <typename T>
[[nodiscard]] variant encode_diagnostic_value(const T& input, std::size_t depth, diagnostic_context& context);

template <typename T>
[[nodiscard]] schema::input_value encode_diagnostic_field(const schema::field_rule<T>& field, const T& input,
                                                          std::size_t depth, diagnostic_context& context) {
   if (context.exhausted) {
      throw diagnostic_limit{};
   }
   if (field.secret) {
      return schema::input_value{std::string{"<redacted>"}};
   }

   auto matches = std::size_t{};
   const auto* address = field.member_address ? field.member_address(input) : nullptr;
   const auto matches_member = [&](std::string_view name, const auto& member) {
      using member_type = clean_type<decltype(member)>;
      if (field.type != std::type_index{typeid(member_type)}) {
         return false;
      }
      if constexpr (reflect::is_described_object_v<T>) {
         return !field.member_name.empty() && field.member_name == name;
      } else {
         return address && address == std::addressof(member);
      }
   };

   for_each_diagnostic_member(input, [&](std::string_view name, const auto& member) {
      if (matches_member(name, member)) {
         ++matches;
      }
   });
   if (matches != 1) {
      return schema::input_value{std::string{"<unsupported>"}};
   }

   auto output = schema::input_value{};
   for_each_diagnostic_member(input, [&](std::string_view name, const auto& member) {
      if (matches_member(name, member)) {
         if constexpr (is_optional_v<decltype(member)>) {
            if (!member) {
               return;
            }
         }
         output = to_schema_input(encode_diagnostic_value(member, depth + 1, context));
      }
   });
   return output;
}

template <typename T>
[[nodiscard]] variant encode_diagnostic_unchecked(const T& input, std::size_t depth, diagnostic_context& context) {
   using value_type = clean_type<T>;
   context.consume();
   if (adl::is_secret(input)) {
      return variant{"<redacted>"};
   }
   if (depth >= diagnostic_depth_limit) {
      return variant{"<diagnostic-depth-limit>"};
   }

   if constexpr (std::same_as<value_type, std::nullptr_t> || std::same_as<value_type, std::monostate>) {
      return {};
   } else if constexpr (std::same_as<value_type, __int128> || std::same_as<value_type, unsigned __int128>) {
      auto output = variant{};
      forge::to_variant(input, output);
      return output;
   } else if constexpr (std::is_arithmetic_v<value_type>) {
      if constexpr (std::same_as<value_type, long double>) {
         auto output = variant{};
         forge::to_variant(input, output);
         return output;
      } else if constexpr (std::same_as<value_type, char> || std::same_as<value_type, wchar_t> ||
                           std::same_as<value_type, char8_t> || std::same_as<value_type, char16_t> ||
                           std::same_as<value_type, char32_t>) {
         return variant{static_cast<std::int64_t>(input)};
      } else {
         return variant{input};
      }
   } else if constexpr (std::same_as<value_type, std::string> || std::same_as<value_type, std::string_view>) {
      return variant{std::string{input}};
   } else if constexpr (std::same_as<value_type, char*> || std::same_as<value_type, const char*> ||
                        (std::is_array_v<value_type> &&
                         std::same_as<std::remove_cv_t<std::remove_extent_t<value_type>>, char>)) {
      return input ? variant{static_cast<const char*>(input)} : variant{};
   } else if constexpr (std::same_as<value_type, wchar_t*> || std::same_as<value_type, const wchar_t*> ||
                        (std::is_array_v<value_type> &&
                         std::same_as<std::remove_cv_t<std::remove_extent_t<value_type>>, wchar_t>)) {
      return input ? variant{static_cast<const wchar_t*>(input)} : variant{};
   } else if constexpr (is_optional_v<value_type>) {
      return input ? encode_diagnostic_value(*input, depth + 1, context) : variant{};
   } else if constexpr (is_pointer_v<value_type>) {
      if (!input) {
         return {};
      }
      const auto identity = pointer_identity(input);
      if (context.contains(identity)) {
         return variant{"<diagnostic-depth-limit>"};
      }
      const auto scope = diagnostic_pointer_scope{context, identity};
      return encode_diagnostic_value(*input, depth + 1, context);
   } else if constexpr (std::same_as<value_type, blob> || std::same_as<value_type, std::vector<char>>) {
      auto output = variant{};
      forge::to_variant(input, output);
      return output;
   } else if constexpr (std::same_as<value_type, variant>) {
      if (input.is_array()) {
         return encode_diagnostic_value(input.get_array(), depth + 1, context);
      }
      if (input.is_object()) {
         return encode_diagnostic_value(input.get_object(), depth + 1, context);
      }
      return input;
   } else if constexpr (std::same_as<value_type, variant_object> || std::same_as<value_type, mutable_variant_object>) {
      auto output = mutable_variant_object{};
      for (const auto& entry : input) {
         output.set(entry.key(), encode_diagnostic_value(entry.value(), depth + 1, context));
         if (context.exhausted) {
            break;
         }
      }
      return variant{std::move(output)};
   } else if constexpr (is_variant_v<value_type>) {
      return variant{variants{
          variant{static_cast<std::uint64_t>(input.index())},
          std::visit([&](const auto& value) { return encode_diagnostic_value(value, depth + 1, context); }, input)}};
   } else if constexpr (is_diagnostic_sequence_v<value_type> || is_multi_index_v<value_type> ||
                        is_associative_v<value_type>) {
      constexpr auto max_elements =
          is_unsigned_byte_vector_v<value_type> ? MAX_SIZE_OF_BYTE_ARRAYS : MAX_NUM_ARRAY_ELEMENTS;
      if (input.size() > max_elements) {
         return variant{"<diagnostic-size-limit>"};
      }
      auto output = variants{};
      output.reserve(std::min(input.size(), context.remaining + 1));
      for (const auto& entry : input) {
         output.push_back(encode_diagnostic_value(entry, depth + 1, context));
         if (context.exhausted) {
            break;
         }
      }
      return variant{std::move(output)};
   } else if constexpr (is_pair_v<value_type>) {
      return variant{variants{encode_diagnostic_value(input.first, depth + 1, context),
                              encode_diagnostic_value(input.second, depth + 1, context)}};
   } else {
      const auto rules = schema::rules<value_type>::define();
      if (schema::has_explicit_rules_v<value_type> || !rules.fields().empty()) {
         return from_schema_input(schema::input_value{
             rules.encode_object_with(input, [&](const auto& field, const auto& object, std::string_view) {
                return encode_diagnostic_field(field, object, depth, context);
             })});
      }
      if constexpr (adl::custom_conversion<value_type>) {
         if (!requires_diagnostic_walk(input, depth, context)) {
            auto output = variant{};
            adl::convert(input, output);
            return encode_diagnostic_value(output, depth + 1, context);
         }
      }
      if constexpr (reflect::is_described_object_v<value_type> || reflect::is_diagnostic_aggregate_v<value_type>) {
         auto output = mutable_variant_object{};
         for_each_diagnostic_member(input, [&](std::string_view name, const auto& member) {
            if (context.exhausted) {
               return;
            }
            if constexpr (is_optional_v<decltype(member)>) {
               if (!member) {
                  return;
               }
            }
            output.set(std::string{name}, encode_diagnostic_value(member, depth + 1, context));
         });
         return variant{std::move(output)};
      } else if constexpr (requires(variant& output) { forge::to_variant(input, output); }) {
         auto output = variant{};
         forge::to_variant(input, output);
         return encode_diagnostic_value(output, depth + 1, context);
      } else {
         return variant{"<unsupported>"};
      }
   }
}

template <typename T>
[[nodiscard]] variant encode_diagnostic_value(const T& input, std::size_t depth, diagnostic_context& context) {
   try {
      return encode_diagnostic_unchecked(input, depth, context);
   } catch (const diagnostic_limit&) {
      return variant{"<diagnostic-depth-limit>"};
   } catch (...) {
      return variant{"<diagnostic-error>"};
   }
}

template <typename T> void apply_encoding(const T& input, variant& output, std::string_view path);

template <typename T> void apply_encoding(const T& input, variant& output, std::string_view path) {
   using value_type = clean_type<T>;

   if constexpr (is_optional_v<value_type>) {
      if (input) {
         apply_encoding(*input, output, path);
      }
   } else if constexpr (is_pointer_v<value_type>) {
      if (input) {
         apply_encoding(*input, output, path);
      }
   } else if constexpr (reflect::is_described_object_v<value_type>) {
      const auto rules = schema::rules<value_type>::define();
      if (!rules.fields().empty()) {
         output = from_schema_input(schema::input_value{rules.encode_object(input, path)});
         return;
      }
      if (!output.is_object()) {
         return;
      }

      auto object = mutable_variant_object{output.get_object()};
      reflect::for_each_member<value_type>([&](const char* name, auto member) {
         const auto found = object.find(name);
         if (found != object.end()) {
            apply_encoding(input.*member, found->value(), field_path(path, name));
         }
      });
      output = variant{std::move(object)};
   } else if constexpr (is_variant_v<value_type>) {
      if (!output.is_string() && output.is_array() && output.get_array().size() == 2U) {
         std::visit([&](const auto& value) { apply_encoding(value, output.get_array()[1], element_path(path, 1U)); },
                    input);
      }
   } else if constexpr (is_unsigned_byte_vector_v<value_type>) {
      return;
   } else if constexpr (is_multi_index_v<value_type> || is_sequence_v<value_type>) {
      if (!output.is_array()) {
         return;
      }
      auto& encoded = output.get_array();
      auto current = input.begin();
      for (std::size_t index = 0; index < encoded.size() && current != input.end(); ++index, ++current) {
         apply_encoding(*current, encoded[index], element_path(path, index));
      }
   } else if constexpr (is_associative_v<value_type>) {
      if (!output.is_array()) {
         return;
      }
      auto& encoded = output.get_array();
      auto current = input.begin();
      for (std::size_t index = 0; index < encoded.size() && current != input.end(); ++index, ++current) {
         if (!encoded[index].is_array() || encoded[index].get_array().size() != 2U) {
            continue;
         }
         apply_encoding(current->first, encoded[index].get_array()[0], element_path(element_path(path, index), 0U));
         apply_encoding(current->second, encoded[index].get_array()[1], element_path(element_path(path, index), 1U));
      }
   } else if constexpr (is_pair_v<value_type>) {
      if (output.is_array() && output.get_array().size() == 2U) {
         apply_encoding(input.first, output.get_array()[0], element_path(path, 0U));
         apply_encoding(input.second, output.get_array()[1], element_path(path, 1U));
      }
   }
}

template <typename T>
void materialize(variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics);

template <typename Variant, std::size_t Index = 0>
void materialize_variant_payload(std::size_t selected, variant& payload, std::string_view path,
                                 std::vector<schema::diagnostic>& diagnostics) {
   if constexpr (Index < std::variant_size_v<Variant>) {
      if (selected == Index) {
         materialize<std::variant_alternative_t<Index, Variant>>(payload, path, diagnostics);
         return;
      }
      materialize_variant_payload<Variant, Index + 1>(selected, payload, path, diagnostics);
   }
}

template <typename T>
void materialize(variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics) {
   using value_type = clean_type<T>;

   if constexpr (is_optional_v<value_type>) {
      if (!source.is_null()) {
         materialize<typename optional_traits<value_type>::value_type>(source, path, diagnostics);
      }
   } else if constexpr (is_pointer_v<value_type>) {
      if (!source.is_null()) {
         materialize<typename pointer_traits<value_type>::value_type>(source, path, diagnostics);
      }
   } else if constexpr (reflect::is_described_object_v<value_type>) {
      const auto rules = schema::rules<value_type>::define();
      if (!rules.fields().empty()) {
         try {
            const auto input = to_schema_input(source);
            const auto* object = input.as_object();
            if (!object) {
               diagnostics.push_back(
                   schema::make_path_error(std::string{path}, "config.type", "schema-bound record must be an object"));
               return;
            }
            auto output = value_type{};
            rules.apply_defaults(output);
            auto nested = rules.decode_object(*object, path, output);
            const auto has_errors = std::ranges::any_of(
                nested, [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; });
            diagnostics.insert(diagnostics.end(), std::make_move_iterator(nested.begin()),
                               std::make_move_iterator(nested.end()));
            if (!has_errors) {
               source = variant{output};
            }
         } catch (const std::exception& error) {
            diagnostics.push_back(schema::make_path_error(std::string{path}, "config.type", std::string{error.what()}));
         }
         return;
      }
      if (source.is_string()) {
         return;
      }

      if (!source.is_object()) {
         return;
      }
      auto object = mutable_variant_object{source.get_object()};
      reflect::for_each_member<value_type>([&](const char* name, auto member) {
         const auto found = object.find(name);
         if (found == object.end()) {
            return;
         }
         using member_type = clean_type<decltype(std::declval<value_type>().*member)>;
         materialize<member_type>(found->value(), field_path(path, name), diagnostics);
      });
      source = variant{std::move(object)};
   } else if constexpr (is_variant_v<value_type>) {
      if (source.is_string() || !source.is_array() || source.get_array().size() < 2U) {
         return;
      }
      auto& elements = source.get_array();
      auto selected = std::uint64_t{};
      try {
         selected = elements[0].as_uint64();
      } catch (const std::exception&) {
         return;
      }
      if (selected >= variant_traits<value_type>::size) {
         return;
      }
      materialize_variant_payload<value_type>(selected, elements[1], element_path(path, 1U), diagnostics);
   } else if constexpr (is_multi_index_v<value_type>) {
      if (!source.is_array()) {
         return;
      }
      auto& elements = source.get_array();
      for (std::size_t index = 0; index < elements.size(); ++index) {
         materialize<typename multi_index_traits<value_type>::value_type>(elements[index], element_path(path, index),
                                                                          diagnostics);
      }
   } else if constexpr (is_unsigned_byte_vector_v<value_type>) {
      return;
   } else if constexpr (is_sequence_v<value_type>) {
      if (!source.is_array()) {
         return;
      }
      auto& elements = source.get_array();
      for (std::size_t index = 0; index < elements.size(); ++index) {
         materialize<typename sequence_traits<value_type>::value_type>(elements[index], element_path(path, index),
                                                                       diagnostics);
      }
   } else if constexpr (is_associative_v<value_type>) {
      if (!source.is_array()) {
         return;
      }
      auto& elements = source.get_array();
      for (std::size_t index = 0; index < elements.size(); ++index) {
         if (!elements[index].is_array()) {
            continue;
         }
         auto& pair = elements[index].get_array();
         if (!pair.empty()) {
            materialize<typename associative_traits<value_type>::key_type>(
                pair[0], element_path(element_path(path, index), 0U), diagnostics);
         }
         if (pair.size() > 1U) {
            materialize<typename associative_traits<value_type>::mapped_type>(
                pair[1], element_path(element_path(path, index), 1U), diagnostics);
         }
      }
   } else if constexpr (is_pair_v<value_type>) {
      if (!source.is_array()) {
         return;
      }
      auto& elements = source.get_array();
      if (!elements.empty()) {
         materialize<typename pair_traits<value_type>::first_type>(elements[0], element_path(path, 0U), diagnostics);
      }
      if (elements.size() > 1U) {
         materialize<typename pair_traits<value_type>::second_type>(elements[1], element_path(path, 1U), diagnostics);
      }
   }
}

} // namespace forge::variant_schema::detail

export namespace forge::variant_schema {

template <typename T> [[nodiscard]] variant encode(const T& input) {
   auto output = variant{};
   to_variant(input, output);
   detail::apply_encoding(input, output, {});
   return output;
}

template <typename T> [[nodiscard]] variant encode_diagnostic(const T& input) {
   auto context = detail::diagnostic_context{};
   return detail::encode_diagnostic_value(input, 0, context);
}

template <typename T>
[[nodiscard]] std::vector<schema::diagnostic> materialize(variant& source, std::string_view path = {}) {
   auto diagnostics = std::vector<schema::diagnostic>{};
   detail::materialize<T>(source, path, diagnostics);
   return diagnostics;
}

} // namespace forge::variant_schema
