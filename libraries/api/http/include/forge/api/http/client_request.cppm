module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <tuple>
#include <utility>
#include <vector>

export module forge.api.http.client_request;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.server_supplied;
import forge.api.core.types;
import forge.net.http.body;
import forge.api.http.parameters;
export import forge.net.http.client;
import forge.net.http.exceptions;
import forge.net.http.file;
export import forge.api.http.mapping;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.upload;
import forge.codec.json;
import forge.reflect.reflect;
import forge.codec.xml;

export namespace forge::api::http {

using namespace forge::net::http;

namespace detail {

[[nodiscard]] inline std::string default_header_name(std::string_view field_name) {
   auto output = std::string{};
   output.reserve(field_name.size());
   for (const auto character : field_name) {
      output.push_back(character == '_' ? '-' : character);
   }
   return output;
}

[[nodiscard]] inline std::string route_header_name(const route& route, std::string_view field_name) {
   const auto matched = std::find_if(route.headers.begin(), route.headers.end(), [&](const field_binding& binding) {
      return binding.field == field_name;
   });
   if (matched != route.headers.end()) {
      return matched->name;
   }
   return default_header_name(field_name);
}

[[nodiscard]] inline std::string route_query_name(const route& route, std::string_view field_name);

[[nodiscard]] inline std::string route_form_name(const route& route, std::string_view field_name) {
   const auto matched = std::find_if(route.forms.begin(), route.forms.end(), [&](const field_binding& binding) {
      return binding.field == field_name;
   });
   if (matched != route.forms.end()) {
      return matched->name;
   }
   return std::string{field_name};
}

template <typename Request>
void apply_route_headers(request& target, const route& route, const Request& value) {
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_header<member_type>::value) {
            const auto& header = value.*member;
            if (header.present) {
               auto encoded = format_http_field(header.value);
               if (!encoded.has_value()) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                      "HTTP API header value cannot be encoded as text");
               }
               target.set(route_header_name(route, field_name), std::move(*encoded));
            }
         }
      });
   }
}

template <typename Request>
void append_route_query_fields(std::string& target, const route& route, const Request& value) {
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      const auto parsed = parse_route_template(route.target);
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_query<member_type>::value) {
            const auto already_rendered =
               std::find_if(parsed.query.begin(), parsed.query.end(), [&](const field_binding& binding) {
                  return binding.field == field_name;
               }) != parsed.query.end();
            const auto& query = value.*member;
            if (!already_rendered && query.present) {
               auto encoded = format_http_field(query.value);
               if (!encoded.has_value()) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                      "HTTP API query value cannot be encoded as text");
               }
               target.push_back(target.find('?') == std::string::npos ? '?' : '&');
               target += encode_query_component(route_query_name(route, field_name));
               target.push_back('=');
               target += encode_query_component(*encoded);
            }
         }
      });
   }
}

template <typename Request>
void apply_route_cookies(request& target, const Request& value) {
   auto cookie = std::string{};
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_cookie<member_type>::value) {
            const auto& field = value.*member;
            if (field.present) {
               auto encoded = format_http_field(field.value);
               if (!encoded.has_value()) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                      "HTTP API cookie value cannot be encoded as text");
               }
               if (!cookie.empty()) {
                  cookie += "; ";
               }
               cookie += field_name;
               cookie += '=';
               cookie += *encoded;
            }
         }
      });
   }
   if (!cookie.empty()) {
      target.set(field::cookie, std::move(cookie));
   }
}

template <typename Tuple>
[[nodiscard]] std::optional<std::string> tuple_argument_text(const Tuple& arguments,
                                                            const std::vector<std::string>& names,
                                                            std::string_view name) {
   auto result = std::optional<std::string>{};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if (result.has_value() || Index >= names.size() || names[Index] != name) {
             return;
          }
          const auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (forge::api::core::server_supplied_value<argument_type>) {
             return;
          } else if constexpr (detail::is_header<argument_type>::value ||
                        detail::is_query<argument_type>::value ||
                        detail::is_cookie<argument_type>::value ||
                        detail::is_body<argument_type>::value ||
                        detail::is_form<argument_type>::value ||
                        detail::is_form_field<argument_type>::value) {
             if (argument.present) {
                result = format_http_field(argument.value);
             }
          } else if constexpr (!detail::is_body_stream_v<argument_type> &&
                               !detail::is_body_bytes_v<argument_type> &&
                               !detail::is_upload_file_v<argument_type>) {
             result = format_http_field(argument);
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return result;
}

template <typename Tuple>
[[nodiscard]] std::string require_tuple_argument_text(const Tuple& arguments,
                                                      const std::vector<std::string>& names,
                                                      std::string_view name) {
   auto value = tuple_argument_text(arguments, names, name);
   if (!value.has_value()) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API positional argument is not available",
                          forge::exceptions::ctx("field", std::string{name}));
   }
   return *value;
}

template <typename Tuple>
struct rendered_route_target {
   std::string target;
   std::array<bool, std::tuple_size_v<Tuple>> consumed{};
};

template <std::size_t Size>
void mark_argument_consumed(std::array<bool, Size>& consumed,
                            const std::vector<std::string>& names,
                            std::string_view name) {
   for (auto index = std::size_t{0}; index != consumed.size() && index != names.size(); ++index) {
      if (names[index] == name) {
         consumed[index] = true;
         return;
      }
   }
}

[[nodiscard]] inline std::string route_query_name(const route& route, std::string_view field_name) {
   const auto parsed = parse_route_template(route.target);
   const auto matched = std::find_if(parsed.query.begin(), parsed.query.end(), [&](const field_binding& binding) {
      return binding.field == field_name;
   });
   if (matched != parsed.query.end()) {
      return matched->name;
   }
   return std::string{field_name};
}

template <typename Tuple>
[[nodiscard]] rendered_route_target<Tuple> render_route_target(const route& route,
                                                              const Tuple& arguments,
                                                              const std::vector<std::string>& names) {
   auto result = rendered_route_target<Tuple>{};
   auto output = std::string{};
   output.reserve(route.target.size() + 32U);

   for (auto index = std::size_t{0}; index != route.target.size();) {
      const auto current = route.target[index];
      if (current == ':') {
         auto end = index + 1U;
         while (end != route.target.size()) {
            const auto value = route.target[end];
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
         const auto field_name = std::string_view{route.target}.substr(index + 1U, end - index - 1U);
         output += encode_path_segment(require_tuple_argument_text(arguments, names, field_name));
         mark_argument_consumed(result.consumed, names, field_name);
         index = end;
         continue;
      }
      if (current == '{') {
         const auto end = route.target.find('}', index + 1U);
         if (end != std::string::npos) {
            const auto field_name = std::string_view{route.target}.substr(index + 1U, end - index - 1U);
            output += encode_query_component(require_tuple_argument_text(arguments, names, field_name));
            mark_argument_consumed(result.consumed, names, field_name);
            index = end + 1U;
            continue;
         }
      }
      output.push_back(current);
      ++index;
   }
   result.target = std::move(output);
   return result;
}

template <typename Tuple>
void append_unconsumed_query_arguments(std::string& target,
                                       const route& route,
                                       const Tuple& arguments,
                                       const std::vector<std::string>& names,
                                       const std::array<bool, std::tuple_size_v<Tuple>>& consumed) {
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if constexpr (Index < std::tuple_size_v<Tuple>) {
             if (Index >= names.size() || consumed[Index]) {
                return;
             }
             const auto& argument = std::get<Index>(arguments);
             using argument_type = std::remove_cvref_t<decltype(argument)>;
             if constexpr (detail::is_query<argument_type>::value) {
                if (!argument.present) {
                   return;
                }
                auto encoded = format_http_field(argument.value);
                if (!encoded.has_value()) {
                   FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                       "HTTP API query value cannot be encoded as text");
                }
                target.push_back(target.find('?') == std::string::npos ? '?' : '&');
                target += encode_query_component(route_query_name(route, names[Index]));
                target.push_back('=');
                target += encode_query_component(*encoded);
             }
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tuple>
void apply_route_headers(request& target,
                         const route& route,
                         const Tuple& arguments,
                         const std::vector<std::string>& names) {
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if constexpr (Index < std::tuple_size_v<Tuple>) {
             const auto& argument = std::get<Index>(arguments);
             using argument_type = std::remove_cvref_t<decltype(argument)>;
             if constexpr (detail::is_header<argument_type>::value) {
                if (Index >= names.size()) {
                   FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                       "HTTP API positional header name is missing");
                }
                if (argument.present) {
                   auto encoded = format_http_field(argument.value);
                   if (!encoded.has_value()) {
                      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                          "HTTP API header value cannot be encoded as text");
                   }
                   target.set(route_header_name(route, names[Index]), std::move(*encoded));
                }
             }
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tuple>
void apply_route_cookies(request& target,
                         const Tuple& arguments,
                         const std::vector<std::string>& names) {
   auto cookie = std::string{};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if constexpr (Index < std::tuple_size_v<Tuple>) {
             const auto& argument = std::get<Index>(arguments);
             using argument_type = std::remove_cvref_t<decltype(argument)>;
             if constexpr (detail::is_cookie<argument_type>::value) {
                if (Index >= names.size()) {
                   FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                       "HTTP API positional cookie name is missing");
                }
                if (argument.present) {
                   auto encoded = format_http_field(argument.value);
                   if (!encoded.has_value()) {
                      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                          "HTTP API cookie value cannot be encoded as text");
                   }
                   if (!cookie.empty()) {
                      cookie += "; ";
                   }
                   cookie += names[Index];
                   cookie += '=';
                   cookie += *encoded;
                }
             }
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});

   if (!cookie.empty()) {
      target.set(field::cookie, std::move(cookie));
   }
}

[[nodiscard]] std::string encode_body_value(const auto& value, body_codec codec) {
   switch (codec) {
   case body_codec::json: {
      auto encoded = forge::codec::json::write(value);
      if (!encoded.ok()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API request body cannot be encoded as JSON");
      }
      return std::move(encoded.text);
   }
   case body_codec::xml: {
      auto encoded = forge::codec::xml::write(value);
      if (!encoded.ok()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API request body cannot be encoded as XML");
      }
      return std::move(encoded.text);
   }
   }
   return {};
}

template <typename Tuple>
std::optional<std::string> positional_codec_body(const Tuple& arguments,
                                                 const std::array<bool, std::tuple_size_v<Tuple>>& consumed,
                                                 body_codec codec) {
   auto result = std::optional<std::string>{};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          const auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (forge::api::core::server_supplied_value<argument_type>) {
             return;
          } else if constexpr (detail::is_body<argument_type>::value) {
             if (!argument.present) {
                return;
             }
             if (result.has_value()) {
                FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                    "HTTP API request has multiple body parameters");
             }
             result = encode_body_value(argument.value, codec);
          } else if constexpr (forge::reflect::is_described_object_v<argument_type> &&
                               !detail::request_needs_stream_v<argument_type> &&
                               !is_http_parameter_v<argument_type>) {
             if (consumed[Index]) {
                return;
             }
             if (result.has_value()) {
                FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                    "HTTP API request has multiple positional body candidates");
             }
             result = encode_body_value(argument, codec);
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return result;
}

template <typename Tuple>
std::size_t positional_plain_codec_body_candidate_count(const std::array<bool, std::tuple_size_v<Tuple>>& consumed) {
   auto count = std::size_t{0};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          using argument_type = std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>;
          if constexpr (!forge::api::core::server_supplied_value<argument_type> &&
                        forge::reflect::is_described_object_v<argument_type> &&
                        !detail::request_needs_stream_v<argument_type> &&
                        !is_http_parameter_v<argument_type>) {
             if (!consumed[Index]) {
                ++count;
             }
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return count;
}

template <typename Tuple>
consteval std::size_t positional_explicit_body_source_count() {
   auto count = std::size_t{0};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          using argument_type = std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>;
          if constexpr (detail::is_body<argument_type>::value ||
                        detail::is_body_stream_v<argument_type> ||
                        detail::is_body_bytes_v<argument_type>) {
             ++count;
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return count;
}

template <typename Tuple>
std::optional<body_reader> take_positional_body_stream(Tuple& arguments) {
   auto result = std::optional<body_reader>{};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if (result.has_value()) {
             return;
          }
          auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (detail::is_body_stream_v<argument_type>) {
             result = argument.release_reader();
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return result;
}

template <typename Tuple>
std::optional<std::string> positional_body_bytes(Tuple& arguments) {
   auto result = std::optional<std::string>{};
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          if (result.has_value()) {
             return;
          }
          auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (detail::is_body_bytes_v<argument_type>) {
             auto bytes = std::move(argument.bytes);
             result.emplace();
             result->resize(bytes.size());
             if (!bytes.empty()) {
                std::memcpy(result->data(), bytes.data(), bytes.size());
             }
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   return result;
}

template <typename Tuple>
void reject_unsupported_positional_client_body(const Tuple& arguments) {
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          const auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (detail::is_form<argument_type>::value ||
                        detail::is_form_field<argument_type>::value ||
                        detail::is_upload_file_v<argument_type>) {
             static_cast<void>(argument);
             FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                 "HTTP positional form and upload client binding is not implemented");
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tuple>
void reject_http_positional_parameters(const Tuple& arguments) {
   [&]<std::size_t... Index>(std::index_sequence<Index...>) {
      (([&] {
          const auto& argument = std::get<Index>(arguments);
          using argument_type = std::remove_cvref_t<decltype(argument)>;
          if constexpr (detail::is_http_parameter_v<argument_type>) {
             static_cast<void>(argument);
             FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                 "HTTP positional methods cannot use forge::http parameter wrappers");
          }
       }()),
       ...);
   }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tuple>
std::optional<body_reader> bind_positional_request_body(request& target,
                                                        const route& route,
                                                        Tuple& arguments,
                                                        const std::array<bool, std::tuple_size_v<Tuple>>& consumed) {
   if (!uses_request_body(route.verb)) {
      return std::nullopt;
   }
   reject_unsupported_positional_client_body(arguments);
   constexpr auto explicit_body_sources = positional_explicit_body_source_count<Tuple>();
   if constexpr (explicit_body_sources > 1U) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API request has multiple body parameters");
   }
   const auto plain_body_candidates = positional_plain_codec_body_candidate_count<Tuple>(consumed);
   if constexpr (explicit_body_sources > 0U) {
      if (plain_body_candidates > 0U) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                             "HTTP API request has explicit and inferred body parameters");
      }
   }
   auto stream_body = take_positional_body_stream(arguments);
   if (stream_body.has_value()) {
      return stream_body;
   }
   auto bytes_body = positional_body_bytes(arguments);
   if (bytes_body.has_value()) {
      target.body() = std::move(*bytes_body);
      target.prepare_payload();
      return std::nullopt;
   }
   auto codec_body = positional_codec_body(arguments, consumed, route.request_body_codec);
   if (codec_body.has_value()) {
      target.body() = std::move(*codec_body);
      target.set(field::content_type, std::string{detail::content_type(route.request_body_codec)});
      target.prepare_payload();
   }
   return std::nullopt;
}

template <typename Request>
std::optional<body_reader> take_body_stream(Request& value, const route& route) {
   auto result = std::optional<body_reader>{};
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_body_stream_v<member_type>) {
            if (!result.has_value() &&
                (!route.body_stream_field.has_value() || *route.body_stream_field == field_name)) {
               result = (value.*member).release_reader();
            }
         }
      });
   }
   return result;
}

template <typename Request>
std::optional<std::string> dto_body_bytes(Request& value) {
   auto result = std::optional<std::string>{};
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char*, auto member) {
         if (result.has_value()) {
            return;
         }
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_body_bytes_v<member_type>) {
            const auto& bytes = (value.*member).bytes;
            result.emplace();
            result->resize(bytes.size());
            if (!bytes.empty()) {
               std::memcpy(result->data(), bytes.data(), bytes.size());
            }
         }
      });
   }
   return result;
}

template <typename Request>
std::optional<std::string> dto_codec_body(Request& value, bool allow_whole_request_body, body_codec codec) {
   auto result = std::optional<std::string>{};
   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char*, auto member) {
         if (result.has_value()) {
            return;
         }
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_body<member_type>::value) {
            const auto& body = value.*member;
            if (body.present) {
               result = encode_body_value(body.value, codec);
            }
         }
      });
   }
   if constexpr (!detail::request_has_http_parameter_v<Request>) {
      if (!result.has_value() && allow_whole_request_body) {
         result = encode_body_value(value, codec);
      }
   }
   return result;
}

template <typename Request>
std::optional<std::string> dto_multipart_body(request& target, const route& route, const Request& value) {
   auto parts = std::vector<multipart_writer_part>{};

   auto append_field = [&](std::string_view name, std::string_view text) {
      parts.push_back(multipart_writer_part{
         .name = std::string{name},
         .body = std::string{text},
      });
   };

   auto append_file = [&](std::string_view name, const upload_part& part) {
      auto body = std::string{};
      if (!part.memory.empty()) {
         body.assign(reinterpret_cast<const char*>(part.memory.data()), part.memory.size());
      } else {
         body = part.text();
      }
      parts.push_back(multipart_writer_part{
         .name = std::string{name},
         .filename = part.filename,
         .content_type = part.content_type,
         .body = std::move(body),
      });
   };

   if constexpr (forge::reflect::is_described_object_v<Request>) {
      forge::reflect::for_each_member<Request>([&](const char* field_name, auto member) {
         using member_type = std::remove_cvref_t<decltype(value.*member)>;
         if constexpr (detail::is_form<member_type>::value || detail::is_form_field<member_type>::value) {
            const auto& field = value.*member;
            if (field.present) {
               auto encoded = format_http_field(field.value);
               if (!encoded.has_value()) {
                  FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                      "HTTP API form value cannot be encoded as text");
               }
               append_field(route_form_name(route, field_name), *encoded);
            }
         } else if constexpr (detail::is_upload_file_v<member_type>) {
            const auto& file = value.*member;
            if (file.present()) {
               append_file(route_form_name(route, field_name), file.part());
            }
         }
      });
   }

   if (parts.empty()) {
      return std::nullopt;
   }
   auto multipart = write_multipart_form(std::move(parts));
   target.set(field::content_type, multipart.content_type);
   return std::move(multipart.body);
}

template <typename Request>
std::optional<body_reader> bind_dto_request_body(request& target, const route& route, Request& value) {
   if (!uses_request_body(route.verb)) {
      return std::nullopt;
   }

   auto body_count = std::size_t{0};
   auto stream_body = take_body_stream(value, route);
   if (stream_body.has_value()) {
      ++body_count;
   }
   auto bytes_body = dto_body_bytes(value);
   if (bytes_body.has_value()) {
      ++body_count;
   }
   auto multipart_body = dto_multipart_body(target, route, value);
   if (multipart_body.has_value()) {
      ++body_count;
   }
   const auto allow_whole_request_body = route.verb != method::delete_;
   auto codec_body = dto_codec_body(value, allow_whole_request_body, route.request_body_codec);
   if (codec_body.has_value()) {
      ++body_count;
   }
   if (body_count > 1U) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API request has multiple body sources");
   }
   if (stream_body.has_value()) {
      return stream_body;
   }
   if (bytes_body.has_value()) {
      target.body() = std::move(*bytes_body);
      target.prepare_payload();
   } else if (multipart_body.has_value()) {
      target.body() = std::move(*multipart_body);
      target.prepare_payload();
   } else if (codec_body.has_value()) {
      target.body() = std::move(*codec_body);
      target.set(field::content_type, std::string{detail::content_type(route.request_body_codec)});
      target.prepare_payload();
   }
   return std::nullopt;
}

template <typename Tuple>
struct positional_client_request {
   request value;
   std::array<bool, std::tuple_size_v<Tuple>> consumed{};
};

template <typename Request>
request make_client_request(client& target, const route& route, Request& value) {
   auto request_value = request{};
   request_value.method(route.verb);
   auto rendered_target = render_route_target(route, value);
   append_route_query_fields(rendered_target, route, value);
   request_value.target(target.make_target(rendered_target));
   request_value.version(11);
   apply_route_headers(request_value, route, value);
   apply_route_cookies(request_value, value);
   return request_value;
}

template <typename Tuple>
positional_client_request<Tuple> make_client_request(client& target,
                                                     const route& route,
                                                     const Tuple& arguments,
                                                     const std::vector<std::string>& names) {
   auto output = positional_client_request<Tuple>{};
   auto rendered = render_route_target(route, arguments, names);
   append_unconsumed_query_arguments(rendered.target, route, arguments, names, rendered.consumed);

   auto request_value = request{};
   request_value.method(route.verb);
   request_value.target(target.make_target(rendered.target));
   request_value.version(11);
   apply_route_headers(request_value, route, arguments, names);
   apply_route_cookies(request_value, arguments, names);
   output.value = std::move(request_value);
   output.consumed = rendered.consumed;
   return output;
}

} // namespace detail

} // namespace forge::api::http
