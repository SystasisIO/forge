module;

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <forge/exceptions/macros.hpp>

export module forge.api.http.mapping;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.server_supplied;
import forge.api.core.types;
import forge.codec.json;
import forge.api.http.parameters;
import forge.net.http.exceptions;
import forge.net.http.negotiation;
import forge.net.http.types;
import forge.reflect.reflect;
import forge.schema.scalar;

export namespace forge::api::http {

using namespace forge::net::http;

enum class body_codec {
   json,
   xml,
};

enum class error_codec {
   json,
   xml,
};

enum class cache_policy {
   unspecified,
   no_store,
};

struct field_binding {
   std::string field;
   std::string name;
};

struct route {
   method verb = method::get;
   std::string method_name;
   std::string target;
   status success_status = status::ok;
   std::vector<field_binding> headers;
   std::vector<field_binding> forms;
   std::optional<std::string> body_stream_field;
   bool response_file = false;
   bool response_stream = false;
   body_codec request_body_codec = body_codec::json;
   body_codec response_body_codec = body_codec::json;
   error_codec error_body_codec = error_codec::json;
   cache_policy cache = cache_policy::unspecified;
   std::optional<std::string> timeout_field;
};

class route_builder {
 public:
   route_builder(method verb, std::string method_name, std::string target, status success_status)
       : route_{.verb = verb,
                .method_name = std::move(method_name),
                .target = std::move(target),
                .success_status = success_status} {}

   route_builder&& header(std::string field, std::string name) && {
      route_.headers.push_back(field_binding{.field = std::move(field), .name = std::move(name)});
      return std::move(*this);
   }

   route_builder&& form(std::string field, std::string name) && {
      route_.forms.push_back(field_binding{.field = std::move(field), .name = std::move(name)});
      return std::move(*this);
   }

   route_builder&& body_stream(std::string field) && {
      route_.body_stream_field = std::move(field);
      return std::move(*this);
   }

   route_builder&& response_file() && {
      route_.response_file = true;
      return std::move(*this);
   }

   route_builder&& response_stream() && {
      route_.response_stream = true;
      return std::move(*this);
   }

   route_builder&& success_status(status value) && {
      route_.success_status = value;
      return std::move(*this);
   }

   route_builder&& request_body_codec(body_codec value) && {
      route_.request_body_codec = value;
      return std::move(*this);
   }

   route_builder&& response_body_codec(body_codec value) && {
      route_.response_body_codec = value;
      return std::move(*this);
   }

   route_builder&& error_body_codec(error_codec value) && {
      route_.error_body_codec = value;
      return std::move(*this);
   }

   route_builder&& cache(cache_policy value) && {
      route_.cache = value;
      return std::move(*this);
   }

   route_builder&& timeout(std::string field) && {
      route_.timeout_field = std::move(field);
      return std::move(*this);
   }

   [[nodiscard]] route build() && {
      return std::move(route_);
   }

 private:
   route route_;
};

struct parsed_route {
   std::string path;
   std::vector<field_binding> query;
};

template <typename Interface> struct traits;

namespace detail {

template <auto Method>
using http_method_request_t = forge::api::core::detail::method_fixed_request_t<Method>;

template <auto Method>
using http_method_argument_tuple_t = forge::api::core::detail::method_fixed_argument_tuple_t<Method>;

[[noreturn]] inline void throw_positional_proxy_not_supported() {
   FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                         "HTTP positional proxy requires forge::api remote proxy");
}

using ::forge::api::http::detail::is_body;
using ::forge::api::http::detail::is_body_bytes_v;
using ::forge::api::http::detail::is_body_stream_v;
using ::forge::api::http::detail::is_bytes_response_v;
using ::forge::api::http::detail::is_cookie;
using ::forge::api::http::detail::is_empty_response_v;
using ::forge::api::http::detail::is_form;
using ::forge::api::http::detail::is_form_field;
using ::forge::api::http::detail::is_header;
using ::forge::api::http::detail::is_http_parameter_v;
using ::forge::api::http::detail::is_query;
using ::forge::api::http::detail::is_stream_response_v;
using ::forge::api::http::detail::is_streaming_response_v;
using ::forge::api::http::detail::is_upload_file_v;
using ::forge::api::http::detail::request_has_http_parameter_v;
using ::forge::api::http::detail::request_needs_stream_v;
using ::forge::api::http::detail::response_needs_stream_v;

[[nodiscard]] inline bool uses_request_body(method verb) noexcept {
   return verb == method::post || verb == method::put || verb == method::patch || verb == method::delete_;
}

[[nodiscard]] inline std::span<const forge::net::http::media_type_match> media_types(body_codec codec) noexcept {
   static constexpr auto json = std::array{
       forge::net::http::media_type_match{.type = "application/json", .structured_suffix = {}},
       forge::net::http::media_type_match{.type = {}, .structured_suffix = "+json"},
   };
   static constexpr auto xml = std::array{
       forge::net::http::media_type_match{.type = "application/xml", .structured_suffix = {}},
       forge::net::http::media_type_match{.type = "text/xml", .structured_suffix = {}},
       forge::net::http::media_type_match{.type = {}, .structured_suffix = "+xml"},
   };
   switch (codec) {
   case body_codec::json:
      return json;
   case body_codec::xml:
      return xml;
   }
   return {};
}

[[nodiscard]] inline std::span<const forge::net::http::media_type_match>
response_media_types(body_codec codec) noexcept {
   static constexpr auto json = std::array{
       forge::net::http::media_type_match{.type = "application/json", .structured_suffix = {}},
   };
   static constexpr auto xml = std::array{
       forge::net::http::media_type_match{.type = "application/xml", .structured_suffix = {}},
   };
   switch (codec) {
   case body_codec::json:
      return json;
   case body_codec::xml:
      return xml;
   }
   return {};
}

[[nodiscard]] inline bool media_type_matches(body_codec codec, std::string_view value) {
   return forge::net::http::media_type_matches(value, media_types(codec));
}

[[nodiscard]] inline std::string_view content_type(body_codec codec) noexcept {
   switch (codec) {
   case body_codec::json:
      return "application/json";
   case body_codec::xml:
      return "application/xml";
   }
   return "application/octet-stream";
}

[[nodiscard]] inline std::string_view content_type(error_codec codec) noexcept {
   switch (codec) {
   case error_codec::json:
      return "application/json";
   case error_codec::xml:
      return "application/xml";
   }
   return "application/octet-stream";
}

[[nodiscard]] inline bool accept_allows(body_codec codec, std::string_view value) {
   return forge::net::http::accept_allows(value, response_media_types(codec));
}

template <auto Method, typename Request>
inline constexpr auto is_positional_http_method_v =
    std::tuple_size_v<http_method_argument_tuple_t<Method>> != 1U ||
    !forge::reflect::is_described_object_v<std::remove_cvref_t<Request>> ||
    forge::api::core::server_supplied_value<Request>;

template <typename T> struct optional_field : std::false_type {};
template <typename T> struct optional_field<std::optional<T>> : std::true_type {
   using value_type = T;
};

template <typename T> struct byte_vector_field : std::false_type {};
template <typename Allocator> struct byte_vector_field<std::vector<std::uint8_t, Allocator>> : std::true_type {};

[[nodiscard]] inline bool unreserved(char value) noexcept {
   const auto ch = static_cast<unsigned char>(value);
   return std::isalnum(ch) != 0 || value == '-' || value == '.' || value == '_' || value == '~';
}

[[nodiscard]] inline std::string encode_uri_component(std::string_view value) {
   constexpr auto* hex = "0123456789ABCDEF";
   auto output = std::string{};
   output.reserve(value.size());
   for (const auto character : value) {
      if (unreserved(character)) {
         output.push_back(character);
         continue;
      }
      const auto byte = static_cast<unsigned char>(character);
      output.push_back('%');
      output.push_back(hex[(byte >> 4U) & 0x0FU]);
      output.push_back(hex[byte & 0x0FU]);
   }
   return output;
}

[[nodiscard]] inline std::string encode_path_segment(std::string_view value) {
   return encode_uri_component(value);
}

[[nodiscard]] inline std::string encode_query_component(std::string_view value) {
   return encode_uri_component(value);
}

template <typename T> [[nodiscard]] std::optional<std::string> format_http_field(const T& value) {
   using clean = std::remove_cvref_t<T>;
   if constexpr (optional_field<clean>::value) {
      if (!value.has_value()) {
         return std::nullopt;
      }
      return format_http_field(*value);
   } else if constexpr (detail::is_header<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (detail::is_query<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (detail::is_cookie<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (detail::is_body<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (detail::is_form<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (detail::is_form_field<clean>::value) {
      if (!value.present) {
         return std::nullopt;
      }
      return format_http_field(value.value);
   } else if constexpr (byte_vector_field<clean>::value) {
      auto encoded = forge::codec::json::write(value);
      return encoded.ok() ? std::optional<std::string>{std::move(encoded.text)} : std::nullopt;
   } else {
      return forge::schema::format_scalar_text(value);
   }
}

template <typename Request>
[[nodiscard]] std::optional<std::string> request_field_text(const Request& request, std::string_view name) {
   auto result = std::optional<std::string>{};
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         if (!result.has_value() && std::string_view{field_name} == name) {
            result = format_http_field(request.*member);
         }
      });
   }
   return result;
}

template <typename Request>
[[nodiscard]] std::string require_request_field(const Request& request, std::string_view name) {
   auto value = request_field_text(request, name);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route field is not available",
                            forge::exceptions::ctx("field", std::string{name}));
   }
   return *value;
}

[[nodiscard]] inline parsed_route parse_route_template(std::string_view target) {
   if (target.empty() || target.front() != '/') {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route must start with /");
   }

   auto normalize_path = [](std::string_view path) {
      auto output = std::string{};
      output.reserve(path.size());
      for (auto offset = std::size_t{0}; offset < path.size();) {
         if (path[offset] == '/') {
            output.push_back('/');
            ++offset;
            continue;
         }

         const auto separator = path.find('/', offset);
         const auto end = separator == std::string_view::npos ? path.size() : separator;
         const auto segment = path.substr(offset, end - offset);
         if (segment.find('{') != std::string_view::npos || segment.find('}') != std::string_view::npos) {
            if (segment.size() <= 2U || segment.front() != '{' || segment.back() != '}') {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API path placeholders must occupy a complete segment");
            }
            const auto name = segment.substr(1U, segment.size() - 2U);
            for (const auto value : name) {
               if (std::isalnum(static_cast<unsigned char>(value)) == 0 && value != '_') {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                        "HTTP API path placeholder name is invalid");
               }
            }
            output.push_back(':');
            output += name;
         } else {
            output += segment;
         }
         offset = end;
      }
      return output;
   };

   const auto query_start = target.find('?');
   auto parsed = parsed_route{
       .path = normalize_path(target.substr(0, query_start)),
   };
   if (query_start == std::string_view::npos) {
      return parsed;
   }

   auto query = target.substr(query_start + 1U);
   auto offset = std::size_t{0};
   while (offset <= query.size()) {
      const auto separator = query.find('&', offset);
      const auto end = separator == std::string_view::npos ? query.size() : separator;
      const auto part = query.substr(offset, end - offset);
      const auto equals = part.find('=');
      if (equals != std::string_view::npos && equals + 2U <= part.size() && part[equals + 1U] == '{' &&
          part.back() == '}') {
         parsed.query.push_back(field_binding{
             .field = std::string{part.substr(equals + 2U, part.size() - equals - 3U)},
             .name = std::string{part.substr(0, equals)},
         });
      }
      if (separator == std::string_view::npos) {
         break;
      }
      offset = separator + 1U;
   }
   return parsed;
}

template <typename Request> [[nodiscard]] std::string render_route_target(const route& route, const Request& request) {
   const auto query_start = route.target.find('?');
   const auto path = std::string_view{route.target}.substr(0, query_start);
   auto output = std::string{};
   output.reserve(route.target.size() + 32U);

   for (auto index = std::size_t{0}; index != path.size();) {
      const auto current = path[index];
      if (current == ':') {
         auto end = index + 1U;
         while (end != path.size()) {
            const auto value = path[end];
            if (std::isalnum(static_cast<unsigned char>(value)) == 0 && value != '_') {
               break;
            }
            ++end;
         }
         if (end == index + 1U) {
            output.push_back(current);
            ++index;
            continue;
         }
         output += encode_path_segment(require_request_field(request, path.substr(index + 1U, end - index - 1U)));
         index = end;
         continue;
      }
      if (current == '{') {
         const auto end = path.find('}', index + 1U);
         if (end != std::string_view::npos) {
            output += encode_path_segment(require_request_field(request, path.substr(index + 1U, end - index - 1U)));
            index = end + 1U;
            continue;
         }
      }
      output.push_back(current);
      ++index;
   }

   if (query_start == std::string::npos) {
      return output;
   }

   auto query = std::string_view{route.target}.substr(query_start + 1U);
   auto separator = '?';
   for (auto offset = std::size_t{0}; offset <= query.size();) {
      const auto next = query.find('&', offset);
      const auto end = next == std::string_view::npos ? query.size() : next;
      const auto part = query.substr(offset, end - offset);
      const auto equals = part.find('=');
      if (equals != std::string_view::npos && equals + 2U <= part.size() && part[equals + 1U] == '{' &&
          part.back() == '}') {
         const auto field = part.substr(equals + 2U, part.size() - equals - 3U);
         if (const auto value = request_field_text(request, field); value.has_value()) {
            output.push_back(separator);
            output.append(part.substr(0, equals + 1U));
            output += encode_query_component(*value);
            separator = '&';
         }
      } else if (!part.empty()) {
         output.push_back(separator);
         output.append(part);
         separator = '&';
      }
      if (next == std::string_view::npos) {
         break;
      }
      offset = next + 1U;
   }
   return output;
}

[[nodiscard]] inline std::vector<route> validate_routes(const forge::api::core::descriptor& descriptor,
                                                        std::vector<route> routes) {
   for (const auto& route : routes) {
      if (route.method_name.empty()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found, "HTTP API route method is empty");
      }
      if (forge::api::core::find_method(descriptor, route.method_name) == nullptr) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                               "HTTP API route method is not in descriptor",
                               forge::exceptions::ctx("method", route.method_name));
      }
      static_cast<void>(parse_route_template(route.target));
   }
   return routes;
}

[[nodiscard]] inline bool route_uses_fixed_field(const route& value, std::string_view name) {
   const auto parsed = parse_route_template(value.target);
   for (auto offset = std::size_t{0}; offset != parsed.path.size();) {
      if (parsed.path[offset] != ':') {
         ++offset;
         continue;
      }
      auto end = offset + 1U;
      while (end != parsed.path.size()) {
         const auto character = parsed.path[end];
         if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_') {
            break;
         }
         ++end;
      }
      if (std::string_view{parsed.path}.substr(offset + 1U, end - offset - 1U) == name) {
         return true;
      }
      offset = end;
   }
   return std::ranges::any_of(parsed.query, [&](const field_binding& binding) {
      return binding.field == name;
   });
}

template <typename T>
inline constexpr auto client_stream_transport_parameter_v =
   is_header<std::remove_cvref_t<T>>::value ||
   is_query<std::remove_cvref_t<T>>::value ||
   is_cookie<std::remove_cvref_t<T>>::value;

template <auto Method, typename Request>
void validate_server_supplied_route(const route& value) {
   if constexpr (is_positional_http_method_v<Method, Request>) {
      using interface_type = forge::api::core::method_class_t<Method>;
      using argument_tuple = http_method_argument_tuple_t<Method>;
      const auto descriptor = forge::api::core::api_traits<interface_type>::describe();
      const auto* method = forge::api::core::find_method(descriptor, value.method_name);
      if (method == nullptr || method->argument_names.size() != std::tuple_size_v<argument_tuple>) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API positional method is missing argument metadata");
      }
      [&]<std::size_t... Index>(std::index_sequence<Index...>) {
         (([&] {
            using argument = std::tuple_element_t<Index, argument_tuple>;
            if constexpr (forge::api::core::server_supplied_value<argument>) {
               const auto& name = method->argument_names[Index];
               if (route_uses_fixed_field(value, name) ||
                   std::ranges::any_of(value.headers, [&](const auto& field) { return field.field == name; }) ||
                   std::ranges::any_of(value.forms, [&](const auto& field) { return field.field == name; }) ||
                   (value.body_stream_field && *value.body_stream_field == name)) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                        "HTTP server-supplied argument cannot be bound to a request field",
                                        forge::exceptions::ctx("field", name));
               }
            }
         }()), ...);
      }(std::make_index_sequence<std::tuple_size_v<argument_tuple>>{});
   }
}

template <auto Method, typename Request>
void validate_live_stream_route(const route& value) {
   constexpr auto kind = forge::api::core::method_kind_v<Method>;
   if constexpr (kind == forge::api::core::method_kind::bidirectional_stream) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "HTTP/1.1 does not support bidirectional API streams");
   } else if constexpr (kind == forge::api::core::method_kind::client_stream) {
      if (!value.forms.empty() || value.body_stream_field.has_value()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API client stream reserves the request body for stream items");
      }

      using argument_tuple = http_method_argument_tuple_t<Method>;
      if constexpr (is_positional_http_method_v<Method, Request>) {
         using interface_type = forge::api::core::method_class_t<Method>;
         const auto descriptor = forge::api::core::api_traits<interface_type>::describe();
         const auto* method = forge::api::core::find_method(descriptor, value.method_name);
         if (method == nullptr ||
             (std::tuple_size_v<argument_tuple> != 0U && method->argument_names.size() !=
                                                        std::tuple_size_v<argument_tuple>)) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API positional stream method is missing argument metadata");
         }
         [&]<std::size_t... Index>(std::index_sequence<Index...>) {
            (([&] {
               using argument = std::tuple_element_t<Index, argument_tuple>;
               const auto& name = method->argument_names[Index];
               if constexpr (forge::api::core::server_supplied_value<argument>) {
                  return;
               } else if constexpr (client_stream_transport_parameter_v<argument>) {
                  if (route_uses_fixed_field(value, name)) {
                     FORGE_THROW_EXCEPTION(
                        forge::net::http::exceptions::bad_request,
                        "HTTP parameter wrapper conflicts with route binding",
                        forge::exceptions::ctx("field", name));
                  }
                  return;
               } else if constexpr (is_http_parameter_v<std::remove_cvref_t<argument>>) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                        "HTTP API client stream fixed argument requires request-body mapping",
                                        forge::exceptions::ctx("field", name));
               } else if (!route_uses_fixed_field(value, name)) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                        "HTTP API client stream fixed argument is not bound by path or query",
                                        forge::exceptions::ctx("field", name));
               }
            }()), ...);
         }(std::make_index_sequence<std::tuple_size_v<argument_tuple>>{});
      } else {
         using request_type = std::remove_cvref_t<Request>;
         forge::reflect::for_each_member<request_type>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(std::declval<request_type>().*member)>;
            if constexpr (client_stream_transport_parameter_v<member_type>) {
               if (route_uses_fixed_field(value, name)) {
                  FORGE_THROW_EXCEPTION(
                     forge::net::http::exceptions::bad_request,
                     "HTTP parameter wrapper conflicts with route binding",
                     forge::exceptions::ctx("field", name));
               }
               return;
            } else if constexpr (is_http_parameter_v<member_type> || is_body_stream_v<member_type> ||
                                 is_body_bytes_v<member_type> || is_upload_file_v<member_type>) {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API client stream fixed field requires request-body mapping",
                                     forge::exceptions::ctx("field", name));
            } else if (!route_uses_fixed_field(value, name)) {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API client stream fixed field is not bound by path or query",
                                     forge::exceptions::ctx("field", name));
            }
         });
      }
   }
}

} // namespace detail

} // namespace forge::api::http
