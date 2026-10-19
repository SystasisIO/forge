module;

#include <boost/multi_index_container.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <flat_map>
#include <limits>
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

export module forge.codec.json:exact;

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
import forge.reflect.reflect;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;
import forge.variant.schema;
import forge.variant.static_variant;

namespace forge::codec::json::detail {

template <typename T> using clean_type = std::remove_cv_t<std::remove_reference_t<T>>;

inline constexpr auto dynamic_sequence_extent = std::numeric_limits<std::size_t>::max();

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

template <typename T>
inline constexpr bool is_microseconds_duration_v = std::same_as<clean_type<T>, std::chrono::microseconds>;

template <typename T>
inline constexpr bool is_chrono_time_point_v =
    std::same_as<clean_type<T>, std::chrono::sys_time<std::chrono::microseconds>> ||
    std::same_as<clean_type<T>, std::chrono::sys_seconds>;

template <typename T> inline constexpr bool is_byte_vector_v = false;

template <typename Allocator> inline constexpr bool is_byte_vector_v<std::vector<char, Allocator>> = true;

template <typename T> inline constexpr bool is_unsigned_byte_vector_v = false;

template <typename Allocator>
inline constexpr bool is_unsigned_byte_vector_v<std::vector<std::uint8_t, Allocator>> = true;

template <typename T> struct sequence_traits {
   static constexpr bool value = false;
};

template <typename T, typename Allocator> struct sequence_traits<std::vector<T, Allocator>> {
   static constexpr bool value = !std::same_as<T, char>;
   static constexpr std::size_t extent = dynamic_sequence_extent;
   using value_type = T;
};

template <typename T, typename Allocator> struct sequence_traits<std::deque<T, Allocator>> {
   static constexpr bool value = true;
   static constexpr std::size_t extent = dynamic_sequence_extent;
   using value_type = T;
};

template <typename T, std::size_t Size> struct sequence_traits<std::array<T, Size>> {
   static constexpr bool value = true;
   static constexpr std::size_t extent = Size;
   using value_type = T;
};

template <typename T, typename Compare, typename Allocator> struct sequence_traits<std::set<T, Compare, Allocator>> {
   static constexpr bool value = true;
   static constexpr std::size_t extent = dynamic_sequence_extent;
   using value_type = T;
};

template <typename T, typename Hash, typename Equal, typename Allocator>
struct sequence_traits<std::unordered_set<T, Hash, Equal, Allocator>> {
   static constexpr bool value = true;
   static constexpr std::size_t extent = dynamic_sequence_extent;
   using value_type = T;
};

template <typename T> inline constexpr bool is_sequence_v = sequence_traits<clean_type<T>>::value;

template <typename T> struct multi_index_traits {
   static constexpr bool value = false;
};

template <typename Value, typename IndexSpecifierList, typename Allocator>
struct multi_index_traits<boost::multi_index_container<Value, IndexSpecifierList, Allocator>> {
   static constexpr bool value = true;
   using value_type = Value;
};

template <typename T> inline constexpr bool is_multi_index_v = multi_index_traits<clean_type<T>>::value;

template <typename T> struct unique_sequence_traits {
   static constexpr bool value = false;
};

template <typename T, typename Compare, typename Allocator>
struct unique_sequence_traits<std::set<T, Compare, Allocator>> {
   static constexpr bool value = true;
   using seen_type = std::set<T, Compare>;
};

template <typename T, typename Hash, typename Equal, typename Allocator>
struct unique_sequence_traits<std::unordered_set<T, Hash, Equal, Allocator>> {
   static constexpr bool value = true;
   using seen_type = std::unordered_set<T, Hash, Equal>;
};

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
   static constexpr bool unique = true;
   using key_type = Key;
   using mapped_type = Value;
   using seen_type = std::set<Key, Compare>;
};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct associative_traits<std::multimap<Key, Value, Compare, Allocator>> {
   static constexpr bool value = true;
   static constexpr bool unique = false;
   using key_type = Key;
   using mapped_type = Value;
   using seen_type = std::set<Key, Compare>;
};

template <typename Key, typename Value, typename Hash, typename Equal, typename Allocator>
struct associative_traits<std::unordered_map<Key, Value, Hash, Equal, Allocator>> {
   static constexpr bool value = true;
   static constexpr bool unique = true;
   using key_type = Key;
   using mapped_type = Value;
   using seen_type = std::unordered_set<Key, Hash, Equal>;
};

template <typename Key, typename Value, typename Compare, typename KeyContainer, typename MappedContainer>
struct associative_traits<std::flat_map<Key, Value, Compare, KeyContainer, MappedContainer>> {
   static constexpr bool value = true;
   static constexpr bool unique = true;
   using key_type = Key;
   using mapped_type = Value;
   using seen_type = std::set<Key, Compare>;
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

[[nodiscard]] std::string field_path(std::string_view path, std::string_view field);

[[nodiscard]] std::string element_path(std::string_view path, std::size_t index);

void add_exact_error(std::vector<schema::diagnostic>& diagnostics, std::string path, std::string code,
                     std::string message);

[[nodiscard]] bool matches_canonical_json_value(const variant& source, const variant& canonical);

template <typename T>
void validate_canonical_string_adapter(const variant& source, std::string_view path,
                                       std::vector<schema::diagnostic>& diagnostics, std::string_view description,
                                       std::string_view code = "json.type") {
   if (!source.is_string()) {
      add_exact_error(diagnostics, std::string{path}, std::string{code},
                      std::string{description} + " must be a JSON string");
      return;
   }

   try {
      const auto value = source.template as<T>();
      auto canonical = variant{};
      to_variant(value, canonical);
      if (!canonical.is_string() || canonical.get_string() != source.get_string()) {
         add_exact_error(diagnostics, std::string{path}, std::string{code},
                         std::string{description} + " must use its canonical JSON spelling");
      }
   } catch (const std::exception& error) {
      add_exact_error(diagnostics, std::string{path}, std::string{code}, error.what());
   }
}

template <typename T>
bool validate_canonical_variant_adapter(const variant& source, std::string_view path,
                                        std::vector<schema::diagnostic>& diagnostics, std::string_view description,
                                        bool require_non_object_canonical = false) {
   try {
      const auto value = source.template as<T>();
      auto canonical = variant{};
      to_variant(value, canonical);
      if (require_non_object_canonical && canonical.is_object()) {
         return false;
      }
      if (!matches_canonical_json_value(source, canonical)) {
         add_exact_error(diagnostics, std::string{path}, "json.type",
                         std::string{description} + " must use its canonical JSON representation");
      }
      return true;
   } catch (const std::exception&) {
      return false;
   }
}

template <typename T> [[nodiscard]] bool uses_non_object_variant_representation() {
   if constexpr (std::default_initializable<T> &&
                 requires(const T& input, variant& output) { to_variant(input, output); }) {
      try {
         const auto value = T{};
         auto canonical = variant{};
         to_variant(value, canonical);
         return !canonical.is_object();
      } catch (const std::exception&) {
      }
   }
   return false;
}

template <typename T>
void validate_exact(const variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics);

template <typename T>
void materialize_schema_records(variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics);

[[nodiscard]] schema::input_value to_schema_input(const variant& source);

template <typename T> [[nodiscard]] variant to_schema_aware_variant(const T& input) {
   return variant_schema::encode(input);
}

void append_schema_diagnostics(std::vector<schema::diagnostic>& output, std::vector<schema::diagnostic> diagnostics);

template <typename Variant, std::size_t Index = 0>
void validate_variant_payload(std::size_t selected, const variant& payload, std::string_view path,
                              std::vector<schema::diagnostic>& diagnostics) {
   if constexpr (Index < std::variant_size_v<Variant>) {
      if (selected == Index) {
         validate_exact<std::variant_alternative_t<Index, Variant>>(payload, path, diagnostics);
         return;
      }
      validate_variant_payload<Variant, Index + 1>(selected, payload, path, diagnostics);
   }
}

template <typename T>
void validate_exact(const variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics) {
   using value_type = clean_type<T>;

   if constexpr (is_optional_v<value_type>) {
      if (!source.is_null()) {
         validate_exact<typename optional_traits<value_type>::value_type>(source, path, diagnostics);
      }
   } else if constexpr (is_pointer_v<value_type>) {
      if (!source.is_null()) {
         validate_exact<typename pointer_traits<value_type>::value_type>(source, path, diagnostics);
      }
   } else if constexpr (std::same_as<value_type, forge::variant_object>) {
      if (!source.is_object()) {
         add_exact_error(diagnostics, std::string{path}, "json.object", "dynamic object must be a JSON object");
      }
   } else if constexpr (std::same_as<value_type, bool> || schema::integral_value<value_type> ||
                        std::floating_point<value_type> || std::same_as<value_type, std::string>) {
      try {
         const auto input = to_schema_input(source);
         auto nested = std::vector<schema::diagnostic>{};
         schema::validate_exact_input_value<value_type>(input, path, nested);
         append_schema_diagnostics(diagnostics, std::move(nested));
      } catch (const std::exception& error) {
         add_exact_error(diagnostics, std::string{path}, "json.type", error.what());
      }
   } else if constexpr (reflect::is_described_enum_v<value_type>) {
      validate_canonical_string_adapter<value_type>(source, path, diagnostics, "described enum");
   } else if constexpr (is_microseconds_duration_v<value_type>) {
      try {
         const auto input = to_schema_input(source);
         auto nested = std::vector<schema::diagnostic>{};
         schema::validate_exact_input_value<std::int64_t>(input, path, nested);
         append_schema_diagnostics(diagnostics, std::move(nested));
      } catch (const std::exception& error) {
         add_exact_error(diagnostics, std::string{path}, "json.type", error.what());
      }
   } else if constexpr (is_chrono_time_point_v<value_type>) {
      validate_canonical_string_adapter<value_type>(source, path, diagnostics, "chrono time point");
   } else if constexpr (is_byte_vector_v<value_type>) {
      validate_canonical_string_adapter<value_type>(source, path, diagnostics, "hexadecimal byte vector");
   } else if constexpr (is_unsigned_byte_vector_v<value_type>) {
      if (!source.is_array()) {
         add_exact_error(diagnostics, std::string{path}, "json.array", "byte vector must be a JSON array");
         return;
      }
      const auto& elements = source.get_array();
      for (auto index = std::size_t{}; index < elements.size(); ++index) {
         const auto& element = elements[index];
         const auto valid =
             (element.is_uint64() && element.as_uint64() <= std::numeric_limits<std::uint8_t>::max()) ||
             (element.is_int64() && element.as_int64() >= 0 &&
              static_cast<std::uint64_t>(element.as_int64()) <= std::numeric_limits<std::uint8_t>::max());
         if (!valid) {
            validate_exact<std::uint8_t>(element, element_path(path, index), diagnostics);
         }
      }
   } else if constexpr (std::same_as<value_type, forge::blob>) {
      validate_canonical_string_adapter<value_type>(source, path, diagnostics, "Base64 blob");
   } else if constexpr (reflect::is_described_object_v<value_type>) {
      // Some described value types intentionally use a canonical string adapter.
      // Their own from_variant overload remains the authority for that scalar form.
      if (source.is_string()) {
         validate_canonical_string_adapter<value_type>(source, path, diagnostics, "described scalar adapter",
                                                       "json.object");
         return;
      }
      if (!source.is_object()) {
         if (validate_canonical_variant_adapter<value_type>(source, path, diagnostics, "described scalar adapter",
                                                            true)) {
            return;
         }
         add_exact_error(diagnostics, std::string{path}, "json.object", "described record must be a JSON object");
         return;
      }
      if (uses_non_object_variant_representation<value_type>()) {
         add_exact_error(diagnostics, std::string{path}, "json.type",
                         "described scalar adapter must use its canonical JSON representation");
         return;
      }

      const auto& object = source.get_object();
      const auto rules = schema::rules<value_type>::define();
      if (!rules.fields().empty()) {
         try {
            const auto input = to_schema_input(source);
            append_schema_diagnostics(diagnostics, rules.validate_exact_input(*input.as_object(), path));
         } catch (const std::exception& error) {
            add_exact_error(diagnostics, std::string{path}, "json.type", error.what());
         }
         return;
      }

      auto known = std::set<std::string>{};
      for (const auto& field : rules.fields()) {
         known.emplace(field.name);
         known.insert(field.aliases.begin(), field.aliases.end());
      }

      reflect::for_each_member<value_type>([&](const char* name, auto member) {
         const auto rule = std::ranges::find_if(
             rules.fields(), [name](const auto& field) { return field.member_name == std::string_view{name}; });
         auto expected_name = std::string{name};
         auto found = object.end();
         if (rule != rules.fields().end()) {
            expected_name = rule->name;
            found = object.find(rule->name);
            for (auto alias = rule->aliases.begin(); found == object.end() && alias != rule->aliases.end(); ++alias) {
               found = object.find(*alias);
            }
         } else {
            known.emplace(name);
            found = object.find(name);
         }

         using member_type = clean_type<decltype(std::declval<value_type>().*member)>;
         if (found == object.end()) {
            if constexpr (!is_optional_v<member_type>) {
               add_exact_error(diagnostics, field_path(path, expected_name), "json.missing", "missing JSON field");
            }
            return;
         }
         validate_exact<member_type>(found->value(), field_path(path, found->key()), diagnostics);
      });

      for (const auto& entry : object) {
         if (!known.contains(entry.key())) {
            add_exact_error(diagnostics, field_path(path, entry.key()), "json.unknown", "unknown JSON field");
         }
      }
   } else if constexpr (is_variant_v<value_type>) {
      // Public-key and signature variants use canonical string encodings.
      if (source.is_string()) {
         validate_canonical_string_adapter<value_type>(source, path, diagnostics, "variant scalar adapter",
                                                       "json.variant");
         return;
      }
      if (!source.is_array()) {
         add_exact_error(diagnostics, std::string{path}, "json.variant", "variant must be encoded as [index, payload]");
         return;
      }

      const auto& elements = source.get_array();
      if (elements.size() != 2U) {
         add_exact_error(diagnostics, std::string{path}, "json.variant",
                         "variant must contain exactly an index and payload");
         return;
      }
      if (!elements[0].is_int64() && !elements[0].is_uint64()) {
         add_exact_error(diagnostics, element_path(path, 0U), "json.variant", "variant index must be an integer");
         return;
      }

      if (elements[0].is_int64() && elements[0].as_int64() < 0) {
         add_exact_error(diagnostics, element_path(path, 0U), "json.variant", "variant index is out of range");
         return;
      }

      const auto selected = elements[0].as_uint64();
      if (selected >= variant_traits<value_type>::size) {
         add_exact_error(diagnostics, element_path(path, 0U), "json.variant", "variant index is out of range");
         return;
      }
      validate_variant_payload<value_type>(selected, elements[1], element_path(path, 1U), diagnostics);
   } else if constexpr (is_multi_index_v<value_type>) {
      if (!source.is_array()) {
         add_exact_error(diagnostics, std::string{path}, "json.array", "multi-index container must be a JSON array");
         return;
      }

      const auto& elements = source.get_array();
      auto seen = value_type{};
      for (std::size_t index = 0; index < elements.size(); ++index) {
         const auto entry_path = element_path(path, index);
         const auto diagnostic_count = diagnostics.size();
         validate_exact<typename multi_index_traits<value_type>::value_type>(elements[index], entry_path, diagnostics);
         if (std::ranges::any_of(
                 diagnostics.begin() + static_cast<std::ptrdiff_t>(diagnostic_count), diagnostics.end(),
                 [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; })) {
            continue;
         }
         try {
            auto normalized = elements[index];
            materialize_schema_records<typename multi_index_traits<value_type>::value_type>(normalized, entry_path,
                                                                                            diagnostics);
            const auto value = normalized.template as<typename multi_index_traits<value_type>::value_type>();
            const auto previous_size = seen.size();
            seen.insert(value);
            if (seen.size() == previous_size) {
               add_exact_error(diagnostics, entry_path, "json.duplicate",
                               "element violates a unique multi-index constraint");
            }
         } catch (const std::exception&) {
            // Conversion reports the canonical type diagnostic after structural validation.
         }
      }
   } else if constexpr (is_associative_v<value_type>) {
      if (!source.is_array()) {
         add_exact_error(diagnostics, std::string{path}, "json.array", "associative container must be a JSON array");
         return;
      }

      const auto& elements = source.get_array();
      auto seen = typename associative_traits<value_type>::seen_type{};
      for (std::size_t index = 0; index < elements.size(); ++index) {
         const auto entry_path = element_path(path, index);
         const auto diagnostic_count = diagnostics.size();
         validate_exact<std::pair<typename associative_traits<value_type>::key_type,
                                  typename associative_traits<value_type>::mapped_type>>(elements[index], entry_path,
                                                                                         diagnostics);
         if (std::ranges::any_of(
                 diagnostics.begin() + static_cast<std::ptrdiff_t>(diagnostic_count), diagnostics.end(),
                 [](const schema::diagnostic& entry) { return entry.level == schema::severity::error; })) {
            continue;
         }
         if constexpr (associative_traits<value_type>::unique) {
            if (!elements[index].is_array() || elements[index].get_array().size() != 2U) {
               continue;
            }
            try {
               auto normalized = elements[index].get_array()[0];
               materialize_schema_records<typename associative_traits<value_type>::key_type>(
                   normalized, element_path(entry_path, 0U), diagnostics);
               const auto key = normalized.template as<typename associative_traits<value_type>::key_type>();
               if (!seen.insert(key).second) {
                  add_exact_error(diagnostics, element_path(entry_path, 0U), "json.duplicate",
                                  "duplicate key in unique associative container");
               }
            } catch (const std::exception&) {
               // Conversion reports the canonical type diagnostic after structural validation.
            }
         }
      }
   } else if constexpr (is_pair_v<value_type>) {
      if (!source.is_array() || source.get_array().size() != 2U) {
         add_exact_error(diagnostics, std::string{path}, "json.pair", "pair must contain exactly a key and value");
         return;
      }

      const auto& elements = source.get_array();
      validate_exact<typename pair_traits<value_type>::first_type>(elements[0], element_path(path, 0U), diagnostics);
      validate_exact<typename pair_traits<value_type>::second_type>(elements[1], element_path(path, 1U), diagnostics);
   } else if constexpr (is_sequence_v<value_type>) {
      if (!source.is_array()) {
         add_exact_error(diagnostics, std::string{path}, "json.array", "sequence must be a JSON array");
         return;
      }

      const auto& elements = source.get_array();
      if constexpr (sequence_traits<value_type>::extent != dynamic_sequence_extent) {
         if (elements.size() != sequence_traits<value_type>::extent) {
            add_exact_error(diagnostics, std::string{path}, "json.array", "JSON array has an unexpected size");
            return;
         }
      }
      for (std::size_t index = 0; index < elements.size(); ++index) {
         validate_exact<typename sequence_traits<value_type>::value_type>(elements[index], element_path(path, index),
                                                                          diagnostics);
      }
      if constexpr (unique_sequence_traits<value_type>::value) {
         auto seen = typename unique_sequence_traits<value_type>::seen_type{};
         for (std::size_t index = 0; index < elements.size(); ++index) {
            try {
               auto normalized = elements[index];
               materialize_schema_records<typename sequence_traits<value_type>::value_type>(
                   normalized, element_path(path, index), diagnostics);
               const auto value = normalized.template as<typename sequence_traits<value_type>::value_type>();
               if (!seen.insert(value).second) {
                  add_exact_error(diagnostics, element_path(path, index), "json.duplicate",
                                  "duplicate element in unique sequence");
               }
            } catch (const std::exception&) {
               // Conversion reports the canonical type diagnostic after structural validation.
            }
         }
      }
   } else if constexpr (requires(const variant& input, value_type& output) { from_variant(input, output); }) {
      if constexpr (requires(const value_type& input, variant& output) { to_variant(input, output); }) {
         if (!validate_canonical_variant_adapter<value_type>(source, path, diagnostics, "scalar adapter")) {
            try {
               static_cast<void>(source.template as<value_type>());
            } catch (const std::exception& error) {
               add_exact_error(diagnostics, std::string{path}, "json.type", error.what());
            }
         }
      }
   }
}

template <typename T>
void materialize_schema_records(variant& source, std::string_view path, std::vector<schema::diagnostic>& diagnostics) {
   append_schema_diagnostics(diagnostics, variant_schema::materialize<T>(source, path));
}

} // namespace forge::codec::json::detail
