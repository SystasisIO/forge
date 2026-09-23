module;

#include <boost/describe.hpp>
#include <boost/mp11.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

export module forge.api.http.openapi;

import forge.api.core.descriptor;
import forge.api.core.connection;
import forge.api.core.server_supplied;
import forge.api.http.mapping;
import forge.api.http.parameters;
import forge.core.type_name;
import forge.reflect.reflect;
import forge.schema.scalar;
import forge.variant.value;

export namespace forge::api::http {

struct openapi_info {
   std::string title;
   std::string version;
   std::string description;
   std::vector<std::string> servers;
};

enum class openapi_field_source {
   value,
   query,
   header,
   cookie,
   body,
   form,
   body_stream,
   body_bytes,
   upload,
   server_supplied,
};

struct openapi_field {
   std::string name;
   forge::variant schema;
   bool required = true;
   openapi_field_source source = openapi_field_source::value;
   bool json_parameter = false;
};

struct openapi_query_object {
   std::string name;
   forge::variant schema;
   std::vector<std::string> fields;
   bool required = true;
};

enum class openapi_response_body {
   codec,
   binary,
   none,
};

struct openapi_operation {
   route mapping;
   forge::variant request_schema;
   forge::variant response_schema;
   std::vector<openapi_field> request_fields;
   std::vector<openapi_query_object> query_objects;
   bool positional_request = false;
   openapi_response_body response_body = openapi_response_body::codec;
};

template <typename T> struct json_schema_traits;
template <typename T> struct openapi_query_object_traits;

namespace detail {

template <typename T> using clean_type = std::remove_cvref_t<T>;

template <typename T> struct optional_traits : std::false_type {};
template <typename T> struct optional_traits<std::optional<T>> : std::true_type {
   using value_type = T;
};

template <typename T> struct sequence_traits : std::false_type {};
template <typename T, typename Allocator> struct sequence_traits<std::vector<T, Allocator>> : std::true_type {
   using value_type = T;
};
template <typename T, typename Allocator> struct sequence_traits<std::deque<T, Allocator>> : std::true_type {
   using value_type = T;
};
template <typename T, std::size_t Size> struct sequence_traits<std::array<T, Size>> : std::true_type {
   using value_type = T;
   static constexpr auto size = Size;
};
template <typename T, typename Compare, typename Allocator>
struct sequence_traits<std::set<T, Compare, Allocator>> : std::true_type {
   using value_type = T;
};
template <typename T, typename Hash, typename Equal, typename Allocator>
struct sequence_traits<std::unordered_set<T, Hash, Equal, Allocator>> : std::true_type {
   using value_type = T;
};

template <typename T> struct map_traits : std::false_type {};
template <typename Key, typename Value, typename Compare, typename Allocator>
struct map_traits<std::map<Key, Value, Compare, Allocator>> : std::true_type {
   using key_type = Key;
   using mapped_type = Value;
};
template <typename Key, typename Value, typename Hash, typename Equal, typename Allocator>
struct map_traits<std::unordered_map<Key, Value, Hash, Equal, Allocator>> : std::true_type {
   using key_type = Key;
   using mapped_type = Value;
};

template <typename T> struct pair_traits : std::false_type {};
template <typename First, typename Second> struct pair_traits<std::pair<First, Second>> : std::true_type {
   using first_type = First;
   using second_type = Second;
};

template <typename T> struct tuple_traits : std::false_type {};
template <typename... Values> struct tuple_traits<std::tuple<Values...>> : std::true_type {
   using type = std::tuple<Values...>;
};

template <typename T> struct variant_traits : std::false_type {};
template <typename... Values> struct variant_traits<std::variant<Values...>> : std::true_type {
   using type = std::variant<Values...>;
};

[[nodiscard]] inline forge::variant schema_object(std::string type) {
   return forge::variant{forge::mutable_variant_object{}("type", std::move(type))};
}

[[nodiscard]] inline forge::variant unconstrained_schema(std::string_view type_name) {
   return forge::variant{forge::mutable_variant_object{}("x-forge-cpp-type", std::string{type_name})};
}

template <typename T> [[nodiscard]] forge::variant make_json_schema();

template <typename Tuple, std::size_t... Index>
[[nodiscard]] forge::variants tuple_schemas(std::index_sequence<Index...>) {
   auto output = forge::variants{};
   output.reserve(sizeof...(Index));
   (output.push_back(make_json_schema<std::tuple_element_t<Index, Tuple>>()), ...);
   return output;
}

template <typename Variant, std::size_t... Index>
[[nodiscard]] forge::variants variant_schemas(std::index_sequence<Index...>) {
   auto output = forge::variants{};
   output.reserve(sizeof...(Index));
   (output.push_back(make_json_schema<std::variant_alternative_t<Index, Variant>>()), ...);
   return output;
}

template <typename T> [[nodiscard]] forge::variant make_json_schema() {
   using value_type = clean_type<T>;
   if constexpr (requires { json_schema_traits<value_type>::make(); }) {
      return json_schema_traits<value_type>::make();
   } else if constexpr (std::same_as<value_type, void> || std::same_as<value_type, std::tuple<>>) {
      return forge::variant{forge::mutable_variant_object{}("type", "object")("maxProperties", std::uint64_t{0})};
   } else if constexpr (std::same_as<value_type, forge::variant>) {
      return unconstrained_schema(forge::type_name<value_type>());
   } else if constexpr (optional_traits<value_type>::value) {
      auto choices = forge::variants{};
      choices.push_back(make_json_schema<typename optional_traits<value_type>::value_type>());
      choices.emplace_back(forge::mutable_variant_object{}("type", "null"));
      return forge::variant{forge::mutable_variant_object{}("anyOf", std::move(choices))};
   } else if constexpr (std::same_as<value_type, bool>) {
      return schema_object("boolean");
   } else if constexpr (std::integral<value_type>) {
      auto schema = forge::mutable_variant_object{}("type", "integer");
      if constexpr (sizeof(value_type) <= sizeof(std::int32_t)) {
         schema("format", std::is_signed_v<value_type> ? "int32" : "uint32");
      } else if constexpr (sizeof(value_type) <= sizeof(std::int64_t)) {
         schema("format", std::is_signed_v<value_type> ? "int64" : "uint64");
      }
      if constexpr (std::unsigned_integral<value_type>) {
         schema("minimum", std::uint64_t{0});
      }
      return forge::variant{std::move(schema)};
   } else if constexpr (std::floating_point<value_type>) {
      return forge::variant{forge::mutable_variant_object{}("type", "number")(
          "format", sizeof(value_type) <= sizeof(float) ? "float" : "double")};
   } else if constexpr (std::same_as<value_type, std::string> || forge::schema::canonical_string_scalar<value_type>) {
      return schema_object("string");
   } else if constexpr (std::is_enum_v<value_type> && forge::reflect::is_described_enum_v<value_type>) {
      auto values = forge::variants{};
      using enumerators = boost::describe::describe_enumerators<value_type>;
      values.reserve(boost::mp11::mp_size<enumerators>::value);
      boost::mp11::mp_for_each<enumerators>([&](auto descriptor) { values.emplace_back(descriptor.name); });
      return forge::variant{forge::mutable_variant_object{}("type", "string")("enum", std::move(values))};
   } else if constexpr (sequence_traits<value_type>::value) {
      auto schema = forge::mutable_variant_object{}("type", "array")(
          "items", make_json_schema<typename sequence_traits<value_type>::value_type>());
      if constexpr (requires { sequence_traits<value_type>::size; }) {
         schema("minItems", static_cast<std::uint64_t>(sequence_traits<value_type>::size))(
             "maxItems", static_cast<std::uint64_t>(sequence_traits<value_type>::size));
      }
      return forge::variant{std::move(schema)};
   } else if constexpr (pair_traits<value_type>::value) {
      auto items = forge::variants{};
      items.push_back(make_json_schema<typename pair_traits<value_type>::first_type>());
      items.push_back(make_json_schema<typename pair_traits<value_type>::second_type>());
      return forge::variant{forge::mutable_variant_object{}("type", "array")("prefixItems", std::move(items))(
          "minItems", std::uint64_t{2})("maxItems", std::uint64_t{2})};
   } else if constexpr (tuple_traits<value_type>::value) {
      constexpr auto size = std::tuple_size_v<value_type>;
      return forge::variant{forge::mutable_variant_object{}("type", "array")(
          "prefixItems", tuple_schemas<value_type>(std::make_index_sequence<size>{}))(
          "minItems", static_cast<std::uint64_t>(size))("maxItems", static_cast<std::uint64_t>(size))};
   } else if constexpr (map_traits<value_type>::value) {
      if constexpr (std::same_as<typename map_traits<value_type>::key_type, std::string>) {
         return forge::variant{forge::mutable_variant_object{}("type", "object")(
             "additionalProperties", make_json_schema<typename map_traits<value_type>::mapped_type>())};
      } else {
         using entry_type =
             std::pair<typename map_traits<value_type>::key_type, typename map_traits<value_type>::mapped_type>;
         return forge::variant{
             forge::mutable_variant_object{}("type", "array")("items", make_json_schema<entry_type>())};
      }
   } else if constexpr (variant_traits<value_type>::value) {
      constexpr auto size = std::variant_size_v<value_type>;
      auto payloads = variant_schemas<value_type>(std::make_index_sequence<size>{});
      return forge::variant{forge::mutable_variant_object{}("type", "array")(
          "prefixItems",
          forge::variants{forge::variant{forge::mutable_variant_object{}("type", "integer")(
                              "minimum", std::uint64_t{0})("maximum", static_cast<std::uint64_t>(size - 1U))},
                          forge::variant{forge::mutable_variant_object{}("oneOf", std::move(payloads))}})(
          "minItems", std::uint64_t{2})("maxItems", std::uint64_t{2})};
   } else if constexpr (forge::reflect::is_described_object_v<value_type>) {
      auto properties = forge::mutable_variant_object{};
      auto required = forge::variants{};
      forge::reflect::for_each_member<value_type>([&](const char* name, auto member) {
         using member_type = clean_type<decltype(std::declval<value_type>().*member)>;
         properties.set(name, make_json_schema<member_type>());
         if constexpr (!optional_traits<member_type>::value) {
            required.emplace_back(name);
         }
      });
      auto schema = forge::mutable_variant_object{}("type", "object")("properties", std::move(properties))(
          "additionalProperties", false);
      if (!required.empty()) {
         schema("required", std::move(required));
      }
      return forge::variant{std::move(schema)};
   } else {
      return unconstrained_schema(forge::type_name<value_type>());
   }
}

template <typename T> [[nodiscard]] consteval bool json_parameter_field() {
   using field_type = clean_type<T>;
   if constexpr (optional_traits<field_type>::value) {
      return json_parameter_field<typename optional_traits<field_type>::value_type>();
   } else {
      return detail::byte_vector_field<field_type>::value;
   }
}

template <typename T> [[nodiscard]] openapi_field request_field(std::string name = {}) {
   using field_type = clean_type<T>;
   if constexpr (is_query<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_query<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::query,
                           .json_parameter = json_parameter_field<typename is_query<field_type>::value_type>()};
   } else if constexpr (is_header<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_header<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::header,
                           .json_parameter = json_parameter_field<typename is_header<field_type>::value_type>()};
   } else if constexpr (is_cookie<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_cookie<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::cookie,
                           .json_parameter = json_parameter_field<typename is_cookie<field_type>::value_type>()};
   } else if constexpr (is_body<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_body<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::body};
   } else if constexpr (is_form<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_form<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::form};
   } else if constexpr (is_form_field<field_type>::value) {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<typename is_form_field<field_type>::value_type>(),
                           .required = false,
                           .source = openapi_field_source::form};
   } else if constexpr (is_body_stream_v<field_type>) {
      return openapi_field{.name = std::move(name),
                           .schema = unconstrained_schema(forge::type_name<field_type>()),
                           .required = false,
                           .source = openapi_field_source::body_stream};
   } else if constexpr (is_body_bytes_v<field_type>) {
      return openapi_field{.name = std::move(name),
                           .schema = unconstrained_schema(forge::type_name<field_type>()),
                           .required = false,
                           .source = openapi_field_source::body_bytes};
   } else if constexpr (is_upload_file_v<field_type>) {
      return openapi_field{.name = std::move(name),
                           .schema = unconstrained_schema(forge::type_name<field_type>()),
                           .required = false,
                           .source = openapi_field_source::upload};
   } else {
      return openapi_field{.name = std::move(name),
                           .schema = make_json_schema<field_type>(),
                           .required = !optional_traits<field_type>::value,
                           .json_parameter = json_parameter_field<field_type>()};
   }
}

template <typename Request> [[nodiscard]] std::vector<openapi_field> request_fields() {
   auto output = std::vector<openapi_field>{};
   using request_type = clean_type<Request>;
   if constexpr (forge::reflect::is_described_object_v<request_type>) {
      forge::reflect::for_each_member<request_type>([&](const char* name, auto member) {
         using member_type = clean_type<decltype(std::declval<request_type>().*member)>;
         output.push_back(request_field<member_type>(name));
      });
   }
   return output;
}

template <typename Request> [[nodiscard]] std::vector<openapi_query_object> query_objects() {
   using request_type = clean_type<Request>;
   if constexpr (requires { openapi_query_object_traits<request_type>::make(); }) {
      return openapi_query_object_traits<request_type>::make();
   } else {
      return {};
   }
}

template <typename Tuple, std::size_t... Index>
[[nodiscard]] std::vector<openapi_field> positional_fields(std::index_sequence<Index...>) {
   auto output = std::vector<openapi_field>{};
   output.reserve(sizeof...(Index));
   (output.push_back([] {
      using argument_type = clean_type<std::tuple_element_t<Index, Tuple>>;
      if constexpr (forge::api::core::server_supplied_value<argument_type>) {
         return openapi_field{.required = false, .source = openapi_field_source::server_supplied};
      } else {
         return request_field<argument_type>();
      }
   }()), ...);
   return output;
}

template <auto Method> [[nodiscard]] std::vector<openapi_field> positional_fields() {
   using tuple_type = http_method_argument_tuple_t<Method>;
   return positional_fields<tuple_type>(std::make_index_sequence<std::tuple_size_v<tuple_type>>{});
}

template <auto Method, typename Request, typename Response>
[[nodiscard]] openapi_operation make_openapi_operation(route mapping) {
   static_assert(std::same_as<clean_type<Request>, clean_type<http_method_request_t<Method>>>);
   static_assert(std::same_as<clean_type<Response>, clean_type<forge::api::core::method_response_t<Method>>>);
   constexpr auto positional = is_positional_http_method_v<Method, Request>;
   constexpr auto response_body = [] {
      if constexpr (is_empty_response_v<Response>) {
         return openapi_response_body::none;
      } else if constexpr (is_bytes_response_v<Response> || response_needs_stream_v<Response>) {
         return openapi_response_body::binary;
      } else {
         return openapi_response_body::codec;
      }
   }();
   return openapi_operation{.mapping = std::move(mapping),
                            .request_schema = make_json_schema<Request>(),
                            .response_schema = make_json_schema<Response>(),
                            .request_fields = positional ? positional_fields<Method>() : request_fields<Request>(),
                            .query_objects =
                                positional ? std::vector<openapi_query_object>{} : query_objects<Request>(),
                            .positional_request = positional,
                            .response_body = response_body};
}

[[nodiscard]] forge::variant build_openapi_document(const forge::api::core::descriptor& api,
                                                    std::vector<openapi_operation> operations, openapi_info info);

} // namespace detail

template <typename Interface> [[nodiscard]] forge::variant openapi(openapi_info info = {}) {
   return detail::build_openapi_document(forge::api::core::api_traits<Interface>::describe(),
                                         traits<Interface>::openapi_operations(), std::move(info));
}

class openapi_document_builder {
 public:
   openapi_document_builder();
   ~openapi_document_builder();

   openapi_document_builder(const openapi_document_builder&) = delete;
   openapi_document_builder& operator=(const openapi_document_builder&) = delete;
   openapi_document_builder(openapi_document_builder&&) noexcept;
   openapi_document_builder& operator=(openapi_document_builder&&) noexcept;

   template <typename Interface> openapi_document_builder& add(std::string_view base_path) {
      add(forge::api::core::api_traits<Interface>::describe(), traits<Interface>::openapi_operations(), base_path);
      return *this;
   }

   [[nodiscard]] forge::variant build(openapi_info info) const;

 private:
   void add(forge::api::core::descriptor api, std::vector<openapi_operation> operations, std::string_view base_path);

   struct impl;

   std::unique_ptr<impl> impl_;
};

} // namespace forge::api::http
