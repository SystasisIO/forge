module;

#include <any>
#include <algorithm>
#include <bit>
#include <boost/describe.hpp>
#include <boost/mp11.hpp>
#include <forge/exceptions/macros.hpp>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <cctype>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <typeindex>
#include <variant>
#include <utility>
#include <vector>

export module forge.schema.object;

import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.value_kind;
import forge.schema.enums;
import forge.schema.scalar;

namespace forge::schema::detail {

template <std::floating_point Float, std::integral Integer>
[[nodiscard]] constexpr bool integer_exactly_representable(Integer value) {
   using unsigned_type = std::make_unsigned_t<Integer>;

   const auto encoded = static_cast<unsigned_type>(value);
   const auto magnitude = [&] {
      if constexpr (std::signed_integral<Integer>) {
         return value < 0 ? unsigned_type{} - encoded : encoded;
      } else {
         return encoded;
      }
   }();
   if (magnitude == 0) {
      return true;
   }

   const auto width = std::bit_width(magnitude);
   constexpr auto precision = std::numeric_limits<Float>::digits;
   if (width <= precision) {
      return true;
   }

   const auto discarded = width - precision;
   const auto discarded_mask = (unsigned_type{1} << discarded) - 1;
   return (magnitude & discarded_mask) == 0;
}

} // namespace forge::schema::detail

export namespace forge::schema {

template <typename T> struct rules;
template <typename T> struct member_pointer_traits;

template <typename Object, typename Member> struct member_pointer_traits<Member Object::*> {
   using object_type = Object;
   using member_type = Member;
};

template <typename T, auto Member> [[nodiscard]] std::string described_member_name() {
   auto output = std::string{};
   if constexpr (boost::describe::has_describe_members<T>::value) {
      using members =
          boost::describe::describe_members<T, boost::describe::mod_any_access | boost::describe::mod_inherited>;
      boost::mp11::mp_for_each<members>([&](auto descriptor) {
         if (!output.empty()) {
            return;
         }
         if constexpr (std::same_as<std::remove_cv_t<decltype(descriptor.pointer)>, decltype(Member)>) {
            if (descriptor.pointer == Member) {
               output = descriptor.name;
            }
         }
      });
   }
   return output;
}

struct input_value {
   using array_type = std::vector<input_value>;
   using object_type = std::map<std::string, input_value>;
   using storage_type =
       std::variant<std::monostate, bool, std::int64_t, std::uint64_t, double, std::string, array_type, object_type>;

   storage_type storage;

   input_value() = default;
   input_value(bool input) : storage{input} {}
   input_value(std::int64_t input) : storage{input} {}
   input_value(std::uint64_t input) : storage{input} {}
   input_value(double input) : storage{input} {}
   input_value(std::string input) : storage{std::move(input)} {}
   input_value(array_type input) : storage{std::move(input)} {}
   input_value(object_type input) : storage{std::move(input)} {}

   [[nodiscard]] const array_type* as_array() const noexcept {
      return std::get_if<array_type>(&storage);
   }

   [[nodiscard]] const object_type* as_object() const noexcept {
      return std::get_if<object_type>(&storage);
   }
};

template <typename T>
void validate_exact_input_value(const input_value& input, std::string_view path, std::vector<diagnostic>& diagnostics);

template <typename T> [[nodiscard]] T cast_any_to(const std::any& value) {
   using clean_type = std::remove_cvref_t<T>;
   if (value.type() == typeid(clean_type)) {
      return std::any_cast<clean_type>(value);
   }
   if constexpr (std::same_as<clean_type, std::string>) {
      if (value.type() == typeid(const char*)) {
         return std::string{std::any_cast<const char*>(value)};
      }
      if (value.type() == typeid(char*)) {
         return std::string{std::any_cast<char*>(value)};
      }
   } else if constexpr (integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if (value.type() == typeid(int)) {
         return checked_integral_cast<clean_type>(std::any_cast<int>(value));
      }
      if (value.type() == typeid(unsigned int)) {
         return checked_integral_cast<clean_type>(std::any_cast<unsigned int>(value));
      }
      if (value.type() == typeid(long)) {
         return checked_integral_cast<clean_type>(std::any_cast<long>(value));
      }
      if (value.type() == typeid(unsigned long)) {
         return checked_integral_cast<clean_type>(std::any_cast<unsigned long>(value));
      }
      if (value.type() == typeid(long long)) {
         return checked_integral_cast<clean_type>(std::any_cast<long long>(value));
      }
      if (value.type() == typeid(unsigned long long)) {
         return checked_integral_cast<clean_type>(std::any_cast<unsigned long long>(value));
      }
      if (value.type() == typeid(std::int64_t)) {
         return checked_integral_cast<clean_type>(std::any_cast<std::int64_t>(value));
      }
      if (value.type() == typeid(std::uint64_t)) {
         return checked_integral_cast<clean_type>(std::any_cast<std::uint64_t>(value));
      }
   } else if constexpr (std::floating_point<clean_type>) {
      if (value.type() == typeid(float)) {
         return static_cast<T>(std::any_cast<float>(value));
      }
      if (value.type() == typeid(double)) {
         return static_cast<T>(std::any_cast<double>(value));
      }
      if (value.type() == typeid(long double)) {
         return static_cast<T>(std::any_cast<long double>(value));
      }
   } else if constexpr (std::is_enum_v<clean_type>) {
      if (value.type() == typeid(std::string)) {
         auto parsed = clean_type{};
         if (enum_from_config_string(std::any_cast<std::string>(value), parsed)) {
            return parsed;
         }
      }
      if (value.type() == typeid(const char*)) {
         auto parsed = clean_type{};
         if (enum_from_config_string(std::any_cast<const char*>(value), parsed)) {
            return parsed;
         }
      }
      if (value.type() == typeid(char*)) {
         auto parsed = clean_type{};
         if (enum_from_config_string(std::any_cast<char*>(value), parsed)) {
            return parsed;
         }
      }
      if (value.type() == typeid(int)) {
         auto parsed = clean_type{};
         if (enum_from_int(static_cast<std::int64_t>(std::any_cast<int>(value)), parsed)) {
            return parsed;
         }
      }
      if (value.type() == typeid(std::int64_t)) {
         auto parsed = clean_type{};
         if (enum_from_int(std::any_cast<std::int64_t>(value), parsed)) {
            return parsed;
         }
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_value, "schema value has incompatible type");
}

[[nodiscard]] inline std::string append_path(std::string_view base_path, std::string_view field) {
   auto output = std::string{base_path};
   if (!output.empty()) {
      output += ".";
   }
   output += field;
   return output;
}

[[nodiscard]] inline std::string append_index(std::string_view base_path, std::size_t index) {
   auto output = std::string{base_path};
   output += "[";
   output += std::to_string(index);
   output += "]";
   return output;
}

class encoding_error : public std::invalid_argument {
 public:
   encoding_error(std::string path, std::string message)
       : std::invalid_argument{std::move(message)}, path_{std::move(path)} {}

   [[nodiscard]] const std::string& path() const noexcept {
      return path_;
   }

 private:
   std::string path_;
};

[[nodiscard]] inline diagnostic make_path_error(std::string path, std::string code, std::string message) {
   return diagnostic{
       .path = std::move(path),
       .code = std::move(code),
       .level = severity::error,
       .message = std::move(message),
   };
}

[[nodiscard]] inline diagnostic make_path_warning(std::string path, std::string code, std::string message) {
   return diagnostic{
       .path = std::move(path),
       .code = std::move(code),
       .level = severity::warning,
       .message = std::move(message),
   };
}

template <typename T>
[[nodiscard]] T cast_input_to(const input_value& input, std::string_view path, std::vector<diagnostic>& diagnostics);

template <typename T> [[nodiscard]] input_value to_input_value(const T& input, std::string_view path = {});

template <typename T> [[nodiscard]] std::any to_default_any(const T& input) {
   using clean_type = std::remove_cvref_t<T>;
   if constexpr (std::same_as<clean_type, bool>) {
      return std::any{input};
   } else if constexpr (signed_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::int64_t)) {
         return std::any{static_cast<std::int64_t>(input)};
      } else {
         return std::any{input};
      }
   } else if constexpr (unsigned_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::uint64_t)) {
         return std::any{static_cast<std::uint64_t>(input)};
      } else {
         return std::any{input};
      }
   } else if constexpr (std::floating_point<clean_type>) {
      return std::any{static_cast<double>(input)};
   } else if constexpr (std::same_as<clean_type, std::string>) {
      return std::any{input};
   } else if constexpr (std::is_enum_v<clean_type>) {
      if (auto text = enum_to_config_string(input)) {
         return std::any{std::move(*text)};
      }
      using underlying_type = std::underlying_type_t<clean_type>;
      if constexpr (signed_integral_value<underlying_type>) {
         return std::any{static_cast<std::int64_t>(input)};
      } else {
         return std::any{static_cast<std::uint64_t>(input)};
      }
   } else {
      return std::any{input};
   }
}

template <typename T>
concept numeric_value = integral_value<std::remove_cvref_t<T>> || std::floating_point<std::remove_cvref_t<T>>;

template <numeric_value Left, numeric_value Right> [[nodiscard]] constexpr int compare_numeric(Left left, Right right) {
   if constexpr (integral_value<Left> && integral_value<Right>) {
      if constexpr (signed_integral_value<Left> && signed_integral_value<Right>) {
         const auto lhs = static_cast<__int128>(left);
         const auto rhs = static_cast<__int128>(right);
         return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
      } else if constexpr (unsigned_integral_value<Left> && unsigned_integral_value<Right>) {
         const auto lhs = static_cast<unsigned __int128>(left);
         const auto rhs = static_cast<unsigned __int128>(right);
         return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
      } else if constexpr (signed_integral_value<Left>) {
         if (left < 0) {
            return -1;
         }
         const auto lhs = static_cast<unsigned __int128>(left);
         const auto rhs = static_cast<unsigned __int128>(right);
         return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
      } else {
         if (right < 0) {
            return 1;
         }
         const auto lhs = static_cast<unsigned __int128>(left);
         const auto rhs = static_cast<unsigned __int128>(right);
         return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
      }
   } else {
      const auto lhs = static_cast<long double>(left);
      const auto rhs = static_cast<long double>(right);
      return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
   }
}

template <numeric_value Value, numeric_value Min, numeric_value Max>
[[nodiscard]] constexpr int compare_range(Value value, Min minimum, Max maximum) {
   if (compare_numeric(value, minimum) < 0) {
      return -1;
   }
   if (compare_numeric(value, maximum) > 0) {
      return 1;
   }
   return 0;
}

template <numeric_value Value, numeric_value Min, numeric_value Max>
[[nodiscard]] std::optional<int> compare_any_as(const std::any& value, Min minimum, Max maximum) {
   if (value.type() != typeid(Value)) {
      return std::nullopt;
   }
   return compare_range(std::any_cast<Value>(value), minimum, maximum);
}

template <typename... T> struct type_list {};

template <numeric_value Min, numeric_value Max, numeric_value... Value>
[[nodiscard]] int compare_any_range_as(const std::any& value, Min minimum, Max maximum, type_list<Value...>) {
   auto result = std::optional<int>{};
   (
       [&] {
          if (!result) {
             result = compare_any_as<Value>(value, minimum, maximum);
          }
       }(),
       ...);
   if (!result) {
      throw std::invalid_argument{"value cannot be inspected for range validation"};
   }
   return *result;
}

template <numeric_value Min, numeric_value Max>
[[nodiscard]] int compare_any_range(const std::any& value, Min minimum, Max maximum) {
   using types = type_list<bool, char, signed char, unsigned char, wchar_t, char8_t, char16_t, char32_t, short,
                           unsigned short, int, unsigned int, long, unsigned long, long long, unsigned long long,
                           __int128, unsigned __int128, float, double, long double>;
   return compare_any_range_as(value, minimum, maximum, types{});
}

template <typename T>
[[nodiscard]] std::vector<T> decode_object_list(const input_value& input, std::string_view path,
                                                std::vector<diagnostic>& diagnostics);

template <typename T> struct field_rule {
   std::string name;
   std::string member_name;
   std::vector<std::string> aliases;
   value_kind kind = value_kind::string;
   std::type_index type = std::type_index{typeid(void)};
   bool required = false;
   bool optional = false;
   bool secret = false;
   bool deprecated = false;
   std::string deprecated_message;
   std::string description;
   bool has_default = false;
   std::any default_value;
   std::optional<long double> minimum;
   std::optional<long double> maximum;
   std::function<int(const std::any&)> compare_range;
   bool nested_object_list = false;
   std::type_index item_type = std::type_index{typeid(void)};
   std::function<void(T&)> apply_default;
   std::function<void(T&, const std::any&)> assign_any;
   std::function<std::any(const std::any&)> normalize_default;
   std::function<input_value(const std::any&)> default_input;
   std::function<void(T&, const input_value&, std::string_view, std::vector<diagnostic>&)> assign_input;
   std::function<void(const input_value&, std::string_view, std::vector<diagnostic>&)> validate_exact_input;
   std::function<std::any(const T&)> read_any;
   std::function<std::optional<std::any>(const T&)> read_validation_any;
   std::function<input_value(const T&, std::string_view)> read_input;
   std::function<const void*(const T&)> member_address;
   std::function<std::optional<std::size_t>(const T&)> read_size;
   std::vector<std::function<void(const T&, std::string_view, std::vector<diagnostic>&)>> validators;
};

template <typename T> class field_builder;

template <typename T> class object_schema {
 public:
   object_schema() : fields_{std::make_shared<std::vector<field_rule<T>>>()} {}

   template <auto Member> field_builder<T> field(std::string name) {
      using pointer_traits = member_pointer_traits<decltype(Member)>;
      using object_type = typename pointer_traits::object_type;
      using member_type = std::remove_cvref_t<typename pointer_traits::member_type>;
      static_assert(std::same_as<object_type, T>, "schema field member must belong to schema object type");

      auto rule = field_rule<T>{};
      rule.name = std::move(name);
      rule.member_name = described_member_name<T, Member>();
      rule.kind = member_kind<member_type>::value;
      rule.type = std::type_index{typeid(member_type)};
      rule.optional = is_optional<member_type>::value;
      rule.assign_any = [](T& object, const std::any& value) {
         if constexpr (is_optional<member_type>::value) {
            using item_type = typename is_optional<member_type>::value_type;
            if (value.type() == typeid(member_type)) {
               object.*Member = std::any_cast<member_type>(value);
            } else {
               object.*Member = cast_any_to<item_type>(value);
            }
         } else {
            object.*Member = cast_any_to<member_type>(value);
         }
      };
      rule.normalize_default = [](const std::any& value) -> std::any {
         if constexpr (is_optional<member_type>::value) {
            using item_type = typename is_optional<member_type>::value_type;
            if (value.type() == typeid(member_type)) {
               const auto optional_value = std::any_cast<member_type>(value);
               if (!optional_value) {
                  return {};
               }
               return to_default_any(*optional_value);
            }
            return to_default_any(cast_any_to<item_type>(value));
         } else {
            return to_default_any(cast_any_to<member_type>(value));
         }
      };
      rule.default_input = [](const std::any& value) -> input_value {
         if (!value.has_value()) {
            return {};
         }
         if constexpr (is_optional<member_type>::value) {
            using item_type = typename is_optional<member_type>::value_type;
            if (value.type() == typeid(member_type)) {
               const auto optional_value = std::any_cast<member_type>(value);
               if (!optional_value) {
                  return {};
               }
               return to_input_value(*optional_value);
            }
            return to_input_value(cast_any_to<item_type>(value));
         } else {
            if (value.type() == typeid(member_type)) {
               return to_input_value(std::any_cast<member_type>(value));
            }
            return to_input_value(cast_any_to<member_type>(value));
         }
      };
      rule.assign_input = [](T& object, const input_value& value, std::string_view path,
                             std::vector<diagnostic>& diagnostics) {
         try {
            object.*Member = cast_input_to<member_type>(value, path, diagnostics);
         } catch (const std::exception& error) {
            diagnostics.push_back(make_path_error(std::string{path}, "config.type", error.what()));
         }
      };
      rule.validate_exact_input = [](const input_value& value, std::string_view path,
                                     std::vector<diagnostic>& diagnostics) {
         validate_exact_input_value<member_type>(value, path, diagnostics);
      };
      rule.read_any = [](const T& object) -> std::any { return object.*Member; };
      rule.read_validation_any = [](const T& object) -> std::optional<std::any> {
         const auto& value = object.*Member;
         if constexpr (is_optional<member_type>::value) {
            if (!value) {
               return std::nullopt;
            }
            return std::any{*value};
         } else {
            return std::any{value};
         }
      };
      rule.read_input = [](const T& object, std::string_view path) -> input_value {
         return to_input_value(object.*Member, path);
      };
      rule.member_address = [](const T& object) -> const void* { return std::addressof(object.*Member); };
      if constexpr (is_vector<member_type>::value) {
         rule.read_size = [](const T& object) -> std::optional<std::size_t> { return (object.*Member).size(); };
      } else if constexpr (is_optional<member_type>::value &&
                           is_vector<typename is_optional<member_type>::value_type>::value) {
         rule.read_size = [](const T& object) -> std::optional<std::size_t> {
            const auto& value = object.*Member;
            if (!value) {
               return std::nullopt;
            }
            return value->size();
         };
      }
      rule.apply_default = [state = fields_, index = fields_->size()](T& object) {
         const auto& self = (*state)[index];
         if (self.has_default) {
            self.assign_any(object, self.default_value);
         }
      };

      fields_->push_back(std::move(rule));
      return field_builder<T>{*this, fields_->size() - 1};
   }

   [[nodiscard]] const std::vector<field_rule<T>>& fields() const noexcept {
      return *fields_;
   }

   void apply_defaults(T& object) const {
      for (const auto& field : *fields_) {
         field.apply_default(object);
      }
   }

   [[nodiscard]] std::vector<diagnostic> decode_object(const input_value::object_type& input,
                                                       std::string_view base_path, T& output) const {
      auto result = std::vector<diagnostic>{};
      inspect_input_paths(input, base_path, false, result);

      for (const auto& field : *fields_) {
         auto field_path = append_path(base_path, field.name);
         auto lookup = find_input_path(input, field.name);
         if (!lookup.value) {
            for (const auto& alias : field.aliases) {
               auto alias_lookup = find_input_path(input, alias);
               lookup.blocked = lookup.blocked || alias_lookup.blocked;
               if (alias_lookup.value) {
                  field_path = append_path(base_path, alias);
                  lookup.value = alias_lookup.value;
                  break;
               }
            }
         }

         if (!lookup.value) {
            if (field.required && !lookup.blocked) {
               result.push_back(
                   make_path_error(std::move(field_path), "config.required", "required config field is missing"));
            }
            continue;
         }

         if (field.deprecated) {
            result.push_back(make_path_warning(field_path, "config.deprecated",
                                               field.deprecated_message.empty() ? "deprecated config field"
                                                                                : field.deprecated_message));
         }

         field.assign_input(output, *lookup.value, field_path, result);
      }

      auto validation = validate(output, base_path);
      result.insert(result.end(), validation.begin(), validation.end());
      return result;
   }

   [[nodiscard]] input_value::object_type encode_object(const T& input, std::string_view base_path = {}) const {
      return encode_object_with(
          input,
          [](const auto& field, const T& object, std::string_view path) { return field.read_input(object, path); },
          base_path);
   }

   template <typename Encoder>
   [[nodiscard]] input_value::object_type encode_object_with(const T& input, Encoder&& encoder,
                                                             std::string_view base_path = {}) const {
      auto output = input_value::object_type{};
      for (const auto& field : *fields_) {
         auto value = encoder(field, input, append_path(base_path, field.name));
         if (!std::holds_alternative<std::monostate>(value.storage)) {
            set_input_path(output, field.name, std::move(value));
         }
      }
      return output;
   }

   [[nodiscard]] std::vector<diagnostic> validate(const T& object, std::string_view base_path = {}) const {
      auto result = std::vector<diagnostic>{};
      for (const auto& field : *fields_) {
         if (field.compare_range) {
            try {
               const auto any_value = field.read_validation_any ? field.read_validation_any(object)
                                                                : std::optional<std::any>{field.read_any(object)};
               if (!any_value) {
                  continue;
               }
               const auto comparison = field.compare_range(*any_value);
               if (comparison < 0) {
                  result.push_back(
                      make_error(base_path, field.name, "schema.range", "value is below the allowed minimum"));
               } else if (comparison > 0) {
                  result.push_back(
                      make_error(base_path, field.name, "schema.range", "value is above the allowed maximum"));
               }
            } catch (...) {
               result.push_back(
                   make_error(base_path, field.name, "schema.type", "value cannot be inspected for range validation"));
            }
         }
         for (const auto& validator : field.validators) {
            validator(object, base_path, result);
         }
      }
      return result;
   }

   [[nodiscard]] std::vector<diagnostic> validate_exact_input(const input_value::object_type& input,
                                                              std::string_view base_path = {}) const {
      auto result = std::vector<diagnostic>{};
      inspect_input_paths(input, base_path, true, result);

      for (const auto& field : *fields_) {
         auto field_path = append_path(base_path, field.name);
         const input_value* found = nullptr;
         auto blocked = false;
         const auto find_name = [&](const std::string& name) {
            const auto lookup = find_input_path(input, name);
            blocked = blocked || lookup.blocked;
            if (lookup.value) {
               if (found) {
                  result.push_back(make_path_error(append_path(base_path, name), "config.duplicate",
                                                   "multiple names supplied for the same config field"));
               } else {
                  found = lookup.value;
                  field_path = append_path(base_path, name);
               }
            }
         };
         find_name(field.name);
         for (const auto& alias : field.aliases) {
            find_name(alias);
         }

         if (!found) {
            if (!field.optional && !blocked) {
               result.push_back(
                   make_path_error(std::move(field_path), "config.missing", "exact config field is missing"));
            }
            continue;
         }

         field.validate_exact_input(*found, field_path, result);
      }
      return result;
   }

 private:
   friend class field_builder<T>;

   struct path_catalog {
      std::set<std::string> fields;
      std::set<std::string> prefixes;
   };

   struct path_lookup {
      const input_value* value = nullptr;
      bool blocked = false;
   };

   static void set_input_path(input_value::object_type& output, std::string_view name, input_value value) {
      auto* object = &output;
      auto begin = std::size_t{0};
      while (begin <= name.size()) {
         const auto end = name.find('.', begin);
         const auto segment = name.substr(begin, end == std::string_view::npos ? name.size() - begin : end - begin);
         if (segment.empty()) {
            if (end == std::string_view::npos) {
               break;
            }
            begin = end + 1;
            continue;
         }
         if (end == std::string_view::npos) {
            object->insert_or_assign(std::string{segment}, std::move(value));
            return;
         }

         auto& child = (*object)[std::string{segment}];
         if (!child.as_object()) {
            child = input_value::object_type{};
         }
         object = std::get_if<input_value::object_type>(&child.storage);
         begin = end + 1;
      }
   }

   [[nodiscard]] path_catalog known_paths() const {
      auto output = path_catalog{};
      const auto register_name = [&](std::string_view name) {
         output.fields.emplace(name);
         auto separator = name.find('.');
         while (separator != std::string_view::npos) {
            output.prefixes.emplace(name.substr(0, separator));
            separator = name.find('.', separator + 1);
         }
      };
      for (const auto& field : *fields_) {
         register_name(field.name);
         for (const auto& alias : field.aliases) {
            register_name(alias);
         }
      }
      return output;
   }

   [[nodiscard]] static path_lookup find_input_path(const input_value::object_type& input, std::string_view name) {
      auto lookup = path_lookup{};
      auto* object = &input;
      auto begin = std::size_t{0};
      while (begin <= name.size()) {
         const auto end = name.find('.', begin);
         const auto segment = name.substr(begin, end == std::string_view::npos ? name.size() - begin : end - begin);
         if (segment.empty()) {
            if (end == std::string_view::npos) {
               break;
            }
            begin = end + 1;
            continue;
         }

         const auto entry = object->find(std::string{segment});
         if (entry == object->end()) {
            return lookup;
         }
         if (end == std::string_view::npos) {
            lookup.value = &entry->second;
            return lookup;
         }
         object = entry->second.as_object();
         if (!object) {
            lookup.blocked = true;
            return lookup;
         }
         begin = end + 1;
      }
      return lookup;
   }

   void inspect_input_paths(const input_value::object_type& input, std::string_view base_path, bool exact,
                            std::vector<diagnostic>& diagnostics) const {
      const auto paths = known_paths();
      const auto inspect = [&](auto& self, const input_value::object_type& object,
                               std::string_view relative_path) -> void {
         for (const auto& [name, value] : object) {
            auto path = std::string{relative_path};
            if (!path.empty()) {
               path += ".";
            }
            path += name;

            const auto is_field = paths.fields.contains(path);
            const auto is_prefix = paths.prefixes.contains(path);
            if (!is_field && !is_prefix) {
               auto entry =
                   exact ? make_path_error(append_path(base_path, path), "config.unknown", "unknown config field")
                         : make_path_warning(append_path(base_path, path), "config.unknown", "unknown config field");
               diagnostics.push_back(std::move(entry));
            } else if (!is_field && is_prefix) {
               if (const auto* child = value.as_object()) {
                  self(self, *child, path);
               } else {
                  diagnostics.push_back(make_path_error(append_path(base_path, path), "config.type",
                                                        "config path segment must be an object"));
               }
            }
         }
      };
      inspect(inspect, input, std::string_view{});
   }

   field_rule<T>& field_at(std::size_t index) {
      return (*fields_)[index];
   }

   static diagnostic make_error(std::string_view base_path, const std::string& field, std::string code,
                                std::string message) {
      auto path = std::string{base_path};
      if (!path.empty()) {
         path += ".";
      }
      path += field;
      return diagnostic{
          .path = std::move(path), .code = std::move(code), .level = severity::error, .message = std::move(message)};
   }

   std::shared_ptr<std::vector<field_rule<T>>> fields_;
};

template <typename T> class field_builder {
 public:
   field_builder(object_schema<T> schema, std::size_t index) : schema_{std::move(schema)}, index_{index} {}

   field_builder& required() {
      current().required = true;
      return *this;
   }

   template <typename Value> field_builder& default_value(Value&& value) {
      const auto input = std::any{std::forward<Value>(value)};
      current().default_value = current().normalize_default ? current().normalize_default(input) : input;
      current().has_default = current().default_value.has_value();
      return *this;
   }

   template <numeric_value Min, numeric_value Max> field_builder& range(Min min, Max max) {
      current().minimum = static_cast<long double>(min);
      current().maximum = static_cast<long double>(max);
      current().compare_range = [min, max](const std::any& value) { return compare_any_range(value, min, max); };
      return *this;
   }

   field_builder& secret() {
      current().secret = true;
      return *this;
   }

   field_builder& deprecated(std::string message) {
      current().deprecated = true;
      current().deprecated_message = std::move(message);
      return *this;
   }

   field_builder& description(std::string text) {
      current().description = std::move(text);
      return *this;
   }

   field_builder& alias(std::string name) {
      current().aliases.push_back(std::move(name));
      return *this;
   }

   template <typename Item> field_builder& items() {
      current().nested_object_list = true;
      current().item_type = std::type_index{typeid(Item)};
      return *this;
   }

   field_builder& non_empty() {
      current().validators.push_back([state = schema_.fields_, index = index_](const T& object,
                                                                               std::string_view base_path,
                                                                               std::vector<diagnostic>& diagnostics) {
         const auto& field = (*state)[index];
         if (field.kind != value_kind::string) {
            return;
         }
         const auto any_value = field.read_validation_any ? field.read_validation_any(object)
                                                          : std::optional<std::any>{field.read_any(object)};
         if (!any_value) {
            return;
         }
         const auto& value = std::any_cast<const std::string&>(*any_value);
         if (value.empty()) {
            diagnostics.push_back(
                make_path_error(append_path(base_path, field.name), "schema.non_empty", "value must not be empty"));
         }
      });
      return *this;
   }

   field_builder& min_items(std::size_t count) {
      current().validators.push_back(
          [state = schema_.fields_, index = index_, count](const T& object, std::string_view base_path,
                                                           std::vector<diagnostic>& diagnostics) {
             const auto& field = (*state)[index];
             if (!field.read_size) {
                return;
             }
             const auto size = field.read_size(object);
             if (size && *size < count) {
                diagnostics.push_back(make_path_error(append_path(base_path, field.name), "schema.min_items",
                                                      "list has fewer items than allowed"));
             }
          });
      return *this;
   }

   field_builder& max_items(std::size_t count) {
      current().validators.push_back(
          [state = schema_.fields_, index = index_, count](const T& object, std::string_view base_path,
                                                           std::vector<diagnostic>& diagnostics) {
             const auto& field = (*state)[index];
             if (!field.read_size) {
                return;
             }
             const auto size = field.read_size(object);
             if (size && *size > count) {
                diagnostics.push_back(make_path_error(append_path(base_path, field.name), "schema.max_items",
                                                      "list has more items than allowed"));
             }
          });
      return *this;
   }

   field_builder& each_non_empty() {
      current().validators.push_back([state = schema_.fields_, index = index_](const T& object,
                                                                               std::string_view base_path,
                                                                               std::vector<diagnostic>& diagnostics) {
         const auto& field = (*state)[index];
         if (field.kind != value_kind::string_list) {
            return;
         }
         const auto any_value = field.read_validation_any ? field.read_validation_any(object)
                                                          : std::optional<std::any>{field.read_any(object)};
         if (!any_value) {
            return;
         }
         const auto& values = std::any_cast<const std::vector<std::string>&>(*any_value);
         for (std::size_t i = 0; i < values.size(); ++i) {
            if (values[i].empty()) {
               diagnostics.push_back(make_path_error(append_index(append_path(base_path, field.name), i),
                                                     "schema.non_empty", "list item must not be empty"));
            }
         }
      });
      return *this;
   }

   template <auto Member> field_builder& unique_by() {
      using item_type = typename member_pointer_traits<decltype(Member)>::object_type;
      using member_type = std::remove_cvref_t<typename member_pointer_traits<decltype(Member)>::member_type>;
      current().validators.push_back([state = schema_.fields_, index = index_](const T& object,
                                                                               std::string_view base_path,
                                                                               std::vector<diagnostic>& diagnostics) {
         const auto& field = (*state)[index];
         const auto any_value = field.read_validation_any ? field.read_validation_any(object)
                                                          : std::optional<std::any>{field.read_any(object)};
         if (!any_value) {
            return;
         }
         const auto& values = std::any_cast<const std::vector<item_type>&>(*any_value);
         auto seen = std::set<member_type>{};
         for (const auto& item : values) {
            if (!seen.insert(item.*Member).second) {
               diagnostics.push_back(
                   make_path_error(append_path(base_path, field.name), "schema.unique", "list items must be unique"));
               return;
            }
         }
      });
      return *this;
   }

   template <auto Member> field_builder field(std::string name) {
      return schema_.template field<Member>(std::move(name));
   }

   [[nodiscard]] operator object_schema<T>() const {
      return schema_;
   }

 private:
   field_rule<T>& current() {
      return schema_.field_at(index_);
   }

   object_schema<T> schema_;
   std::size_t index_ = 0;
};

template <typename T>
[[nodiscard]] T cast_input_to(const input_value& input, std::string_view path, std::vector<diagnostic>& diagnostics) {
   using clean_type = std::remove_cvref_t<T>;
   if constexpr (is_optional<clean_type>::value) {
      using item_type = typename is_optional<clean_type>::value_type;
      if (std::holds_alternative<std::monostate>(input.storage)) {
         return clean_type{};
      }
      return cast_input_to<item_type>(input, path, diagnostics);
   } else if constexpr (std::same_as<clean_type, bool>) {
      if (const auto* value = std::get_if<bool>(&input.storage)) {
         return *value;
      }
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         auto parsed = false;
         if (parse_bool_text(*text, parsed)) {
            return parsed;
         }
      }
   } else if constexpr (signed_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if (const auto* value = std::get_if<std::int64_t>(&input.storage)) {
         return checked_integral_cast<clean_type>(*value);
      }
      if (const auto* value = std::get_if<std::uint64_t>(&input.storage)) {
         return checked_integral_cast<clean_type>(*value);
      }
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         return parse_scalar_text<clean_type>(*text);
      }
   } else if constexpr (unsigned_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if (const auto* value = std::get_if<std::uint64_t>(&input.storage)) {
         return checked_integral_cast<clean_type>(*value);
      }
      if (const auto* value = std::get_if<std::int64_t>(&input.storage); value && *value >= 0) {
         return checked_integral_cast<clean_type>(*value);
      }
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         return parse_scalar_text<clean_type>(*text);
      }
   } else if constexpr (std::floating_point<clean_type>) {
      if (const auto* value = std::get_if<double>(&input.storage)) {
         return static_cast<T>(*value);
      }
      if (const auto* value = std::get_if<std::int64_t>(&input.storage)) {
         return static_cast<T>(*value);
      }
      if (const auto* value = std::get_if<std::uint64_t>(&input.storage)) {
         return static_cast<T>(*value);
      }
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         return parse_scalar_text<clean_type>(*text);
      }
   } else if constexpr (std::is_enum_v<clean_type>) {
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         auto parsed = clean_type{};
         if (enum_from_config_string(*text, parsed)) {
            return parsed;
         }
      }
      if (const auto* value = std::get_if<std::int64_t>(&input.storage)) {
         auto parsed = clean_type{};
         if (enum_from_int(*value, parsed)) {
            return parsed;
         }
      }
      if (const auto* value = std::get_if<std::uint64_t>(&input.storage);
          value && *value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
         auto parsed = clean_type{};
         if (enum_from_int(static_cast<std::int64_t>(*value), parsed)) {
            return parsed;
         }
      }
   } else if constexpr (std::same_as<clean_type, std::string>) {
      if (const auto* value = std::get_if<std::string>(&input.storage)) {
         return *value;
      }
   } else if constexpr (canonical_string_scalar<clean_type>) {
      if (const auto* text = std::get_if<std::string>(&input.storage)) {
         return parse_scalar_text<clean_type>(*text);
      }
   } else if constexpr (std::same_as<clean_type, std::vector<std::string>>) {
      if (const auto* values = input.as_array()) {
         auto output = std::vector<std::string>{};
         output.reserve(values->size());
         for (std::size_t i = 0; i < values->size(); ++i) {
            if (const auto* text = std::get_if<std::string>(&(*values)[i].storage)) {
               output.push_back(*text);
               continue;
            }
            diagnostics.push_back(make_path_error(append_index(path, i), "config.type", "list entry is not a string"));
         }
         return output;
      }
   } else if constexpr (is_vector_enum<clean_type>::value) {
      using enum_type = typename vector_item<clean_type>::type;
      if (const auto* values = input.as_array()) {
         auto output = clean_type{};
         output.reserve(values->size());
         for (std::size_t i = 0; i < values->size(); ++i) {
            auto parsed = enum_type{};
            if (const auto* text = std::get_if<std::string>(&(*values)[i].storage)) {
               if (enum_from_config_string(*text, parsed)) {
                  output.push_back(parsed);
                  continue;
               }
               diagnostics.push_back(make_path_error(append_index(path, i), "config.enum", "unknown enum value"));
               continue;
            }
            if (const auto* value = std::get_if<std::int64_t>(&(*values)[i].storage)) {
               if (enum_from_int(*value, parsed)) {
                  output.push_back(parsed);
                  continue;
               }
               diagnostics.push_back(make_path_error(append_index(path, i), "config.enum", "unknown enum value"));
               continue;
            }
            diagnostics.push_back(make_path_error(append_index(path, i), "config.type", "list entry is not an enum"));
         }
         return output;
      }
   } else if constexpr (is_vector<clean_type>::value) {
      using item_type = typename vector_item<clean_type>::type;
      return decode_object_list<item_type>(input, path, diagnostics);
   } else if constexpr (boost::describe::has_describe_members<clean_type>::value) {
      if (const auto* object = input.as_object()) {
         auto output = clean_type{};
         const auto nested_rules = rules<clean_type>::define();
         if (!nested_rules.fields().empty()) {
            nested_rules.apply_defaults(output);
            auto nested = nested_rules.decode_object(*object, path, output);
            diagnostics.insert(diagnostics.end(), nested.begin(), nested.end());
            return output;
         }

         using members = boost::describe::describe_members<clean_type, boost::describe::mod_any_access |
                                                                           boost::describe::mod_inherited>;
         boost::mp11::mp_for_each<members>([&](auto descriptor) {
            const auto found = object->find(descriptor.name);
            if (found == object->end()) {
               return;
            }
            const auto member_path = append_path(path, descriptor.name);
            using member_type = std::remove_cvref_t<decltype(output.*descriptor.pointer)>;
            try {
               output.*descriptor.pointer = cast_input_to<member_type>(found->second, member_path, diagnostics);
            } catch (const std::exception& error) {
               diagnostics.push_back(make_path_error(member_path, "config.type", error.what()));
            }
         });
         return output;
      }
   }

   FORGE_THROW_EXCEPTION(exceptions::invalid_value, "config value has incompatible type");
}

template <typename T>
[[nodiscard]] std::vector<T> decode_object_list(const input_value& input, std::string_view path,
                                                std::vector<diagnostic>& diagnostics) {
   const auto* values = input.as_array();
   if (!values) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_value, "config value has incompatible type");
   }

   auto output = std::vector<T>{};
   output.reserve(values->size());
   const auto nested_rules = rules<T>::define();
   for (std::size_t i = 0; i < values->size(); ++i) {
      const auto item_path = append_index(path, i);
      if constexpr (std::constructible_from<T, std::string>) {
         if (const auto* text = std::get_if<std::string>(&(*values)[i].storage)) {
            auto item = [&] {
               if constexpr (canonical_string_scalar<T>) {
                  return parse_scalar_text<T>(*text);
               } else {
                  return T{*text};
               }
            }();
            auto nested = nested_rules.validate(item, item_path);
            diagnostics.insert(diagnostics.end(), nested.begin(), nested.end());
            output.push_back(std::move(item));
            continue;
         }
      }
      if constexpr (!boost::describe::has_describe_members<T>::value) {
         try {
            output.push_back(cast_input_to<T>((*values)[i], item_path, diagnostics));
         } catch (const std::exception& error) {
            diagnostics.push_back(make_path_error(item_path, "config.type", error.what()));
         }
         continue;
      }

      const auto* object = (*values)[i].as_object();
      if (!object) {
         diagnostics.push_back(make_path_error(item_path, "config.type", "list entry is not an object"));
         continue;
      }
      if (!nested_rules.fields().empty()) {
         auto item = T{};
         nested_rules.apply_defaults(item);
         auto nested = nested_rules.decode_object(*object, item_path, item);
         diagnostics.insert(diagnostics.end(), nested.begin(), nested.end());
         output.push_back(std::move(item));
         continue;
      }
      try {
         output.push_back(cast_input_to<T>((*values)[i], item_path, diagnostics));
      } catch (const std::exception& error) {
         diagnostics.push_back(make_path_error(item_path, "config.type", error.what()));
      }
   }
   return output;
}

template <typename T>
void validate_exact_input_value(const input_value& input, std::string_view path, std::vector<diagnostic>& diagnostics) {
   using clean_type = std::remove_cvref_t<T>;
   if constexpr (is_optional<clean_type>::value) {
      if (!std::holds_alternative<std::monostate>(input.storage)) {
         validate_exact_input_value<typename is_optional<clean_type>::value_type>(input, path, diagnostics);
      }
   } else if constexpr (std::same_as<clean_type, bool>) {
      if (!std::holds_alternative<bool>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "boolean field must be a boolean value"));
      }
   } else if constexpr (signed_integral_value<clean_type>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::int64_t)) {
         const auto in_range = std::holds_alternative<std::int64_t>(input.storage)
                                   ? std::in_range<clean_type>(std::get<std::int64_t>(input.storage))
                               : std::holds_alternative<std::uint64_t>(input.storage)
                                   ? std::in_range<clean_type>(std::get<std::uint64_t>(input.storage))
                                   : false;
         if (!std::holds_alternative<std::int64_t>(input.storage) &&
             !std::holds_alternative<std::uint64_t>(input.storage)) {
            diagnostics.push_back(
                make_path_error(std::string{path}, "config.type", "signed integer field must be an integer value"));
         } else if (!in_range) {
            diagnostics.push_back(
                make_path_error(std::string{path}, "config.range", "signed integer field is out of range"));
         }
      } else if (!std::holds_alternative<std::string>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "wide signed integer field must be a decimal string"));
      } else if (const auto* text = std::get_if<std::string>(&input.storage)) {
         try {
            const auto value = parse_scalar_text<clean_type>(*text);
            if (format_integral_text(value) != *text) {
               diagnostics.push_back(make_path_error(std::string{path}, "config.type",
                                                     "wide signed integer must use canonical decimal spelling"));
            }
         } catch (const std::exception& error) {
            diagnostics.push_back(make_path_error(std::string{path}, "config.range", error.what()));
         }
      }
   } else if constexpr (unsigned_integral_value<clean_type>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::uint64_t)) {
         const auto in_range = std::holds_alternative<std::int64_t>(input.storage)
                                   ? std::in_range<clean_type>(std::get<std::int64_t>(input.storage))
                               : std::holds_alternative<std::uint64_t>(input.storage)
                                   ? std::in_range<clean_type>(std::get<std::uint64_t>(input.storage))
                                   : false;
         if (!std::holds_alternative<std::int64_t>(input.storage) &&
             !std::holds_alternative<std::uint64_t>(input.storage)) {
            diagnostics.push_back(
                make_path_error(std::string{path}, "config.type", "unsigned integer field must be an integer value"));
         } else if (!in_range) {
            diagnostics.push_back(
                make_path_error(std::string{path}, "config.range", "unsigned integer field is out of range"));
         }
      } else if (!std::holds_alternative<std::string>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "wide unsigned integer field must be a decimal string"));
      } else if (const auto* text = std::get_if<std::string>(&input.storage)) {
         try {
            const auto value = parse_scalar_text<clean_type>(*text);
            if (format_integral_text(value) != *text) {
               diagnostics.push_back(make_path_error(std::string{path}, "config.type",
                                                     "wide unsigned integer must use canonical decimal spelling"));
            }
         } catch (const std::exception& error) {
            diagnostics.push_back(make_path_error(std::string{path}, "config.range", error.what()));
         }
      }
   } else if constexpr (std::floating_point<clean_type>) {
      if (!std::holds_alternative<double>(input.storage) && !std::holds_alternative<std::int64_t>(input.storage) &&
          !std::holds_alternative<std::uint64_t>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "floating-point field must be a numeric value"));
         return;
      }

      const auto integer_is_exact =
          std::holds_alternative<std::int64_t>(input.storage)
              ? detail::integer_exactly_representable<clean_type>(std::get<std::int64_t>(input.storage))
          : std::holds_alternative<std::uint64_t>(input.storage)
              ? detail::integer_exactly_representable<clean_type>(std::get<std::uint64_t>(input.storage))
              : true;
      if (!integer_is_exact) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.range",
                                               "value is not exactly representable by the floating-point field"));
         return;
      }

      const auto numeric = std::holds_alternative<double>(input.storage)
                               ? static_cast<long double>(std::get<double>(input.storage))
                           : std::holds_alternative<std::int64_t>(input.storage)
                               ? static_cast<long double>(std::get<std::int64_t>(input.storage))
                               : static_cast<long double>(std::get<std::uint64_t>(input.storage));
      if (!std::isfinite(numeric) || numeric < static_cast<long double>(std::numeric_limits<clean_type>::lowest()) ||
          numeric > static_cast<long double>((std::numeric_limits<clean_type>::max)())) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.range", "floating-point field is out of range"));
         return;
      }

      const auto converted = static_cast<clean_type>(numeric);
      if (numeric != 0.0L && converted == static_cast<clean_type>(0)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.range", "floating-point field underflows to zero"));
      } else if (static_cast<long double>(converted) != numeric) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.range",
                                               "value is not exactly representable by the floating-point field"));
      }
   } else if constexpr (std::same_as<clean_type, std::string>) {
      if (!std::holds_alternative<std::string>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "string field must be a string value"));
      }
   } else if constexpr (canonical_string_scalar<clean_type>) {
      if (!std::holds_alternative<std::string>(input.storage)) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.type",
                                               "scalar field must contain its canonical config spelling"));
         return;
      }
      const auto& text = std::get<std::string>(input.storage);
      try {
         const auto value = parse_scalar_text<clean_type>(text);
         const auto canonical = format_scalar_text(value);
         if (!canonical || *canonical != text) {
            diagnostics.push_back(make_path_error(std::string{path}, "config.type",
                                                  "scalar field must contain its canonical config spelling"));
         }
      } catch (const std::exception& error) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.type", error.what()));
      }
   } else if constexpr (std::is_enum_v<clean_type>) {
      if (!std::holds_alternative<std::string>(input.storage)) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "enum field must contain its canonical config name"));
         return;
      }

      auto parsed = clean_type{};
      const auto& text = std::get<std::string>(input.storage);
      const auto valid = enum_from_config_string(text, parsed);
      const auto canonical = valid ? enum_to_config_string(parsed) : std::nullopt;
      if (!canonical || *canonical != text) {
         diagnostics.push_back(
             make_path_error(std::string{path}, "config.type", "enum field must contain its canonical config name"));
      }
   } else if constexpr (is_vector<clean_type>::value) {
      const auto* values = input.as_array();
      if (!values) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.type", "exact list must be an array"));
         return;
      }
      using item_type = typename vector_item<clean_type>::type;
      for (std::size_t index = 0; index < values->size(); ++index) {
         if constexpr (canonical_string_scalar<item_type>) {
            if (std::holds_alternative<std::string>((*values)[index].storage)) {
               const auto& text = std::get<std::string>((*values)[index].storage);
               try {
                  const auto value = parse_scalar_text<item_type>(text);
                  const auto canonical = format_scalar_text(value);
                  if (!canonical || *canonical != text) {
                     diagnostics.push_back(make_path_error(append_index(path, index), "config.type",
                                                           "scalar adapter must use its canonical config spelling"));
                  }
               } catch (const std::exception& error) {
                  diagnostics.push_back(make_path_error(append_index(path, index), "config.type", error.what()));
               }
               continue;
            }
         } else if constexpr (std::constructible_from<item_type, std::string>) {
            if (std::holds_alternative<std::string>((*values)[index].storage)) {
               continue;
            }
         }
         validate_exact_input_value<item_type>((*values)[index], append_index(path, index), diagnostics);
      }
   } else if constexpr (boost::describe::has_describe_members<clean_type>::value) {
      const auto* object = input.as_object();
      if (!object) {
         diagnostics.push_back(make_path_error(std::string{path}, "config.type", "exact record must be an object"));
         return;
      }

      const auto nested_rules = rules<clean_type>::define();
      if (!nested_rules.fields().empty()) {
         auto nested = nested_rules.validate_exact_input(*object, path);
         diagnostics.insert(diagnostics.end(), nested.begin(), nested.end());
         return;
      }

      auto known_fields = std::set<std::string>{};
      using members = boost::describe::describe_members<clean_type, boost::describe::mod_any_access |
                                                                        boost::describe::mod_inherited>;
      boost::mp11::mp_for_each<members>([&](auto descriptor) {
         known_fields.emplace(descriptor.name);
         const auto found = object->find(descriptor.name);
         using member_type = std::remove_cvref_t<decltype(std::declval<clean_type>().*descriptor.pointer)>;
         if (found == object->end()) {
            if constexpr (!is_optional<member_type>::value) {
               diagnostics.push_back(make_path_error(append_path(path, descriptor.name), "config.missing",
                                                     "exact config field is missing"));
            }
            return;
         }
         validate_exact_input_value<member_type>(found->second, append_path(path, descriptor.name), diagnostics);
      });

      for (const auto& [name, ignored] : *object) {
         if (!known_fields.contains(name)) {
            diagnostics.push_back(make_path_error(append_path(path, name), "config.unknown", "unknown config field"));
         }
      }
   }
}

template <typename T> [[nodiscard]] input_value to_input_value(const T& input, std::string_view path) {
   using clean_type = std::remove_cvref_t<T>;
   if constexpr (is_optional<clean_type>::value) {
      if (!input.has_value()) {
         return input_value{};
      }
      return to_input_value(*input, path);
   } else if constexpr (std::same_as<clean_type, bool>) {
      return input_value{input};
   } else if constexpr (signed_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::int64_t)) {
         return input_value{static_cast<std::int64_t>(input)};
      } else {
         return input_value{format_integral_text(input)};
      }
   } else if constexpr (unsigned_integral_value<clean_type> && !std::same_as<clean_type, bool>) {
      if constexpr (sizeof(clean_type) <= sizeof(std::uint64_t)) {
         return input_value{static_cast<std::uint64_t>(input)};
      } else {
         return input_value{format_integral_text(input)};
      }
   } else if constexpr (std::same_as<clean_type, long double>) {
      throw encoding_error{std::string{path}, "long double schema fields are not supported by config codecs"};
   } else if constexpr (std::floating_point<clean_type>) {
      return input_value{static_cast<double>(input)};
   } else if constexpr (std::same_as<clean_type, std::string>) {
      return input_value{input};
   } else if constexpr (std::is_enum_v<clean_type>) {
      if (auto text = enum_to_config_string(input)) {
         return input_value{std::move(*text)};
      }
      using underlying_type = std::underlying_type_t<clean_type>;
      if constexpr (signed_integral_value<underlying_type>) {
         return input_value{static_cast<std::int64_t>(input)};
      } else {
         return input_value{static_cast<std::uint64_t>(input)};
      }
   } else if constexpr (canonical_string_scalar<clean_type>) {
      auto text = std::optional<std::string>{};
      try {
         text = format_scalar_text(input);
      } catch (const std::exception& error) {
         throw encoding_error{std::string{path}, error.what()};
      }
      if (!text) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_value, "scalar adapter has no canonical config spelling",
                               forge::exceptions::ctx("path", std::string{path}));
      }
      return input_value{*text};
   } else if constexpr (std::same_as<clean_type, std::vector<std::string>>) {
      auto array = input_value::array_type{};
      array.reserve(input.size());
      for (const auto& item : input) {
         array.emplace_back(item);
      }
      return input_value{std::move(array)};
   } else if constexpr (is_vector_enum<clean_type>::value) {
      auto array = input_value::array_type{};
      array.reserve(input.size());
      for (std::size_t index = 0; index < input.size(); ++index) {
         array.push_back(to_input_value(input[index], append_index(path, index)));
      }
      return input_value{std::move(array)};
   } else if constexpr (is_vector<clean_type>::value) {
      auto array = input_value::array_type{};
      array.reserve(input.size());
      for (std::size_t index = 0; index < input.size(); ++index) {
         array.push_back(to_input_value(input[index], append_index(path, index)));
      }
      return input_value{std::move(array)};
   } else {
      const auto nested_rules = rules<clean_type>::define();
      if (!nested_rules.fields().empty()) {
         return input_value{nested_rules.encode_object(input, path)};
      }
      if constexpr (boost::describe::has_describe_members<clean_type>::value) {
         auto object = input_value::object_type{};
         using members = boost::describe::describe_members<clean_type, boost::describe::mod_any_access |
                                                                           boost::describe::mod_inherited>;
         boost::mp11::mp_for_each<members>([&](auto descriptor) {
            auto value = to_input_value(input.*descriptor.pointer, append_path(path, descriptor.name));
            if (!std::holds_alternative<std::monostate>(value.storage)) {
               object.emplace(descriptor.name, std::move(value));
            }
         });
         return input_value{std::move(object)};
      } else {
         return input_value{};
      }
   }
}

template <typename T> [[nodiscard]] object_schema<T> object() {
   return object_schema<T>{};
}

template <typename T> struct rules {
   using default_definition = void;

   [[nodiscard]] static object_schema<T> define() {
      return object<T>();
   }
};

template <typename T>
inline constexpr bool has_explicit_rules_v = !requires { typename rules<std::remove_cvref_t<T>>::default_definition; };

} // namespace forge::schema
