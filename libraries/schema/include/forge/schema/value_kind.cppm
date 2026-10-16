module;

#include <boost/describe.hpp>

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

export module forge.schema.value_kind;

export namespace forge::schema {

enum class value_kind {
   boolean,
   signed_integer,
   unsigned_integer,
   floating,
   string,
   string_list,
   object,
   object_list,
   dynamic,
};

// Specializations own validated conversion to/from input_value. Schema never
// imports a format codec or a dynamic-value backend.
template <typename T> struct value_adapter;

template <typename T> struct dependent_false : std::false_type {};

template <typename T>
concept integral_value = std::integral<std::remove_cvref_t<T>> || std::same_as<std::remove_cvref_t<T>, __int128> ||
                         std::same_as<std::remove_cvref_t<T>, unsigned __int128>;

template <typename T>
concept signed_integral_value = integral_value<T> && (std::signed_integral<std::remove_cvref_t<T>> ||
                                                      std::same_as<std::remove_cvref_t<T>, __int128>);

template <typename T>
concept unsigned_integral_value = integral_value<T> && (std::unsigned_integral<std::remove_cvref_t<T>> ||
                                                        std::same_as<std::remove_cvref_t<T>, unsigned __int128>);

template <typename T> struct unsigned_integral_type {
   using type = std::make_unsigned_t<std::remove_cvref_t<T>>;
};

template <> struct unsigned_integral_type<__int128> {
   using type = unsigned __int128;
};

template <> struct unsigned_integral_type<unsigned __int128> {
   using type = unsigned __int128;
};

template <integral_value T> using unsigned_integral_t = typename unsigned_integral_type<std::remove_cvref_t<T>>::type;

template <typename T> struct is_vector : std::false_type {};

template <typename T, typename Allocator> struct is_vector<std::vector<T, Allocator>> : std::true_type {};

template <typename T> struct vector_item;

template <typename T, typename Allocator> struct vector_item<std::vector<T, Allocator>> {
   using type = T;
};

template <typename T> struct is_vector_enum : std::false_type {};

template <typename T, typename Allocator>
struct is_vector_enum<std::vector<T, Allocator>> : std::bool_constant<std::is_enum_v<T>> {};

template <typename T> struct is_optional : std::false_type {};

template <typename T> struct is_optional<std::optional<T>> : std::true_type {
   using value_type = T;
};

template <typename T> struct member_kind {
   static constexpr value_kind value = [] {
      using clean_type = std::remove_cvref_t<T>;
      if constexpr (is_optional<clean_type>::value) {
         return member_kind<typename is_optional<clean_type>::value_type>::value;
      } else if constexpr (requires { value_adapter<clean_type>::kind; }) {
         return value_adapter<clean_type>::kind;
      } else if constexpr (std::same_as<clean_type, bool>) {
         return value_kind::boolean;
      } else if constexpr (signed_integral_value<clean_type>) {
         return value_kind::signed_integer;
      } else if constexpr (unsigned_integral_value<clean_type>) {
         return value_kind::unsigned_integer;
      } else if constexpr (std::floating_point<clean_type>) {
         return value_kind::floating;
      } else if constexpr (std::same_as<clean_type, std::string>) {
         return value_kind::string;
      } else if constexpr (std::is_enum_v<clean_type>) {
         return value_kind::string;
      } else if constexpr (std::same_as<clean_type, std::vector<std::string>>) {
         return value_kind::string_list;
      } else if constexpr (is_vector_enum<clean_type>::value) {
         return value_kind::string_list;
      } else if constexpr (is_vector<clean_type>::value) {
         return value_kind::object_list;
      } else if constexpr (boost::describe::has_describe_members<clean_type>::value) {
         return value_kind::object;
      } else {
         static_assert(dependent_false<clean_type>::value, "unsupported FORGE schema field type");
      }
   }();
};

} // namespace forge::schema
