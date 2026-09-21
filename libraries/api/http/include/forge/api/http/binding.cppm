module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/mp11.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

export module forge.api.http.binding;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.auth.authenticated_caller;
import forge.net.http.exceptions;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.connection;
import forge.api.http.parameters;
import forge.net.http.file;
export import forge.api.http.mapping;
export import forge.api.http.openapi;
import forge.net.http.middleware;
import forge.net.http.router;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.upload;
import forge.codec.json;
import forge.reflect.reflect;
import forge.schema.diagnostic;
import forge.schema.exceptions;
import forge.schema.object;
import forge.schema.scalar;
import forge.codec.xml;

export namespace forge::api::http {

using namespace forge::net::http;

namespace detail {

[[nodiscard]] forge::api::core::trusted_invocation
trusted_invocation_for(const forge::net::http::route_context& context);

void validate_live_stream_headers(const forge::net::http::request& request,
                                  forge::api::core::method_kind kind);

boost::asio::awaitable<forge::net::http::stream_response>
make_live_server_stream_response(forge::api::core::pinned_binding_plan plan, forge::api::core::frame request,
                                 forge::net::http::stream_request& http_request,
                                 forge::net::http::status success_status, forge::api::core::trusted_invocation trusted);

boost::asio::awaitable<forge::api::core::frame>
dispatch_live_client_stream(forge::api::core::pinned_binding_plan plan, forge::api::core::frame request,
                            forge::net::http::body_reader body,
                            std::function<void(const forge::api::core::bytes&, forge::raw::unpack_limits)> decoder,
                            forge::api::core::trusted_invocation trusted);

[[nodiscard]] forge::net::http::response
make_live_terminal_response(const forge::net::http::request& request,
                            forge::net::http::status success_status,
                            const forge::api::core::frame& terminal);

boost::asio::awaitable<forge::api::core::response>
invoke_live_http_stream(forge::net::http::client& client,
                        const forge::api::core::descriptor& descriptor,
                        const route& route,
                        forge::api::core::request request,
                        forge::net::http::request http_request,
                        forge::net::http::request_options request_options,
                        forge::api::core::method_kind kind,
                        std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                        std::shared_ptr<forge::api::core::detail::stream_endpoint> output);

} // namespace detail

template <typename T> struct is_http_body_sequence : std::false_type {};

template <typename T, typename Allocator> struct is_http_body_sequence<std::vector<T, Allocator>> : std::true_type {};

struct route_options {
   std::vector<field_binding> query;
   std::vector<field_binding> headers;
   std::vector<field_binding> forms;
   std::optional<std::string> body_stream_field;
   bool response_file = false;
   bool response_stream = false;
   status success_status = status::ok;
   body_codec request_body_codec = body_codec::json;
   body_codec response_body_codec = body_codec::json;
   error_codec error_body_codec = error_codec::json;
   cache_policy cache = cache_policy::unspecified;
};

class binding_plan {
 public:
   void mount(router& target, std::string_view base_path = {}) const {
      for (const auto& step : steps_) {
         step(target, base_path);
      }
   }

 private:
   friend class binding_builder;

   using mount_action = std::function<void(router&, std::string_view)>;

   explicit binding_plan(std::vector<mount_action> steps) : steps_{std::move(steps)} {}

   std::vector<mount_action> steps_;
};

class binding_builder {
 public:
   binding_builder() = default;
   explicit binding_builder(router& target) : target_{&target} {}

   binding_builder& use(forge::api::core::binding_plan plan) {
      plan_ = std::move(plan);
      return *this;
   }

   template <auto Method, typename Request, typename Response>
   binding_builder& get(std::string path, route_options options = {}) {
      steps_.push_back(make_step<Method, Request, Response>(method::get, std::move(path), std::move(options), {}));
      return *this;
   }

   template <auto Method, typename Request, typename Response>
   binding_builder& post(std::string path, route_options options = {}) {
      steps_.push_back(make_step<Method, Request, Response>(method::post, std::move(path), std::move(options), {}));
      return *this;
   }

   template <auto Method, typename Request, typename Response>
   binding_builder& put(std::string path, route_options options = {}) {
      steps_.push_back(make_step<Method, Request, Response>(method::put, std::move(path), std::move(options), {}));
      return *this;
   }

   template <auto Method, typename Request, typename Response>
   binding_builder& del(std::string path, route_options options = {}) {
      steps_.push_back(make_step<Method, Request, Response>(method::delete_, std::move(path), std::move(options), {}));
      return *this;
   }

   template <auto Method, typename Request, typename Response> binding_builder& route(route value) {
      const auto parsed = detail::parse_route_template(value.target);
      steps_.push_back(make_step<Method, Request, Response>(value.verb, parsed.path,
                                                            route_options{
                                                                .query = parsed.query,
                                                                .headers = value.headers,
                                                                .forms = value.forms,
                                                                .body_stream_field = value.body_stream_field,
                                                                .response_file = value.response_file,
                                                                .response_stream = value.response_stream,
                                                                .success_status = value.success_status,
                                                                .request_body_codec = value.request_body_codec,
                                                                .response_body_codec = value.response_body_codec,
                                                                .error_body_codec = value.error_body_codec,
                                                                .cache = value.cache,
                                                            },
                                                            value.method_name));
      return *this;
   }

   template <typename Interface> binding_builder& bind() {
      return traits<Interface>::bind(*this);
   }

   binding_builder& middleware(middleware_descriptor descriptor) {
      steps_.push_back([descriptor = std::move(descriptor)](router& target, std::string_view base_path) mutable {
         auto mounted = descriptor;
         mounted.path_prefix = join_path(base_path, mounted.path_prefix);
         target.use(std::move(mounted));
      });
      return *this;
   }

   [[nodiscard]] binding_plan build() {
      static_cast<void>(target_);
      return binding_plan{std::move(steps_)};
   }

 private:
   using mount_action = binding_plan::mount_action;

   template <typename T> struct is_optional : std::false_type {};
   template <typename T> struct is_optional<std::optional<T>> : std::true_type {
      using value_type = T;
   };

   template <typename Object> struct multipart_member_predicate {
      template <typename Descriptor>
      using fn = std::bool_constant<
          detail::is_form_field<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>::value ||
          detail::is_form<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>::value ||
          detail::is_upload_file_v<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>>;
   };

   template <typename Object> struct body_bytes_member_predicate {
      template <typename Descriptor>
      using fn = std::bool_constant<
          detail::is_body_bytes_v<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>>;
   };

   template <typename Object> struct body_member_predicate {
      template <typename Descriptor>
      using fn = std::bool_constant<
          detail::is_body<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>::value>;
   };

   template <typename Object> struct body_stream_member_predicate {
      template <typename Descriptor>
      using fn = std::bool_constant<
          detail::is_body_stream_v<std::remove_cvref_t<decltype(std::declval<Object>().*Descriptor::pointer)>>>;
   };

   template <typename T, template <typename> typename Predicate,
             bool Described = forge::reflect::is_described_object_v<T>>
   struct request_has_member : std::false_type {};

   template <typename T, template <typename> typename Predicate> struct request_has_member<T, Predicate, true> {
      using members = boost::describe::describe_members<std::remove_cvref_t<T>, boost::describe::mod_any_access |
                                                                                    boost::describe::mod_inherited>;
      static constexpr auto value = boost::mp11::mp_any_of_q<members, Predicate<T>>::value;
   };

   template <typename T, template <typename> typename Predicate,
             bool Described = forge::reflect::is_described_object_v<T>>
   struct request_member_count : std::integral_constant<std::size_t, 0U> {};

   template <typename T, template <typename> typename Predicate> struct request_member_count<T, Predicate, true> {
      using members = boost::describe::describe_members<std::remove_cvref_t<T>, boost::describe::mod_any_access |
                                                                                    boost::describe::mod_inherited>;
      static constexpr auto value = boost::mp11::mp_count_if_q<members, Predicate<T>>::value;
   };

   template <typename Request>
   static constexpr auto request_has_multipart_v = request_has_member<Request, multipart_member_predicate>::value;

   template <typename Request>
   static constexpr auto request_has_body_bytes_v = request_has_member<Request, body_bytes_member_predicate>::value;

   template <typename Request>
   static constexpr auto request_has_body_v = request_has_member<Request, body_member_predicate>::value;

   template <typename Request>
   static constexpr auto request_has_body_stream_v = request_has_member<Request, body_stream_member_predicate>::value;

   template <typename Request>
   static constexpr auto request_body_source_count_v =
       (request_has_multipart_v<Request> ? 1U : 0U) + request_member_count<Request, body_member_predicate>::value +
       request_member_count<Request, body_bytes_member_predicate>::value +
       request_member_count<Request, body_stream_member_predicate>::value;

   [[nodiscard]] static std::string normalize_base_path(std::string_view base_path) {
      if (base_path.empty() || base_path == "/") {
         return {};
      }
      if (base_path.front() != '/') {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API base path must start with /");
      }
      while (base_path.size() > 1U && base_path.back() == '/') {
         base_path.remove_suffix(1U);
      }
      return std::string{base_path};
   }

   [[nodiscard]] static std::string join_path(std::string_view base_path, std::string_view path) {
      if (path.empty() || path.front() != '/') {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API path must start with /");
      }
      auto base = normalize_base_path(base_path);
      if (base.empty()) {
         return std::string{path};
      }
      if (path == "/") {
         return base;
      }
      auto result = std::move(base);
      result += path;
      return result;
   }

   template <typename Interface, typename Request, typename Response> [[nodiscard]] static std::string method_name() {
      const auto descriptor = Interface::describe();
      auto result = std::optional<std::string>{};
      for (const auto& method_value : descriptor.methods) {
         if (method_value.request_type == typeid(Request) && method_value.response_type == typeid(Response)) {
            if (result.has_value()) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                                     "HTTP API route method is ambiguous for request/response types");
            }
            result = method_value.name;
         }
      }
      if (!result.has_value()) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                               "HTTP API route method is not declared by descriptor");
      }
      return *result;
   }

   [[nodiscard]] static const forge::api::core::method_descriptor&
   require_route_method_descriptor(const forge::api::core::descriptor* descriptor, std::string_view name) {
      if (descriptor != nullptr) {
         if (const auto* method = forge::api::core::find_method(*descriptor, name); method != nullptr) {
            return *method;
         }
      }
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                            "HTTP API route method is not declared by descriptor",
                            forge::exceptions::ctx("method", std::string{name}));
   }

   [[nodiscard]] static const forge::api::core::detail::server_field_operations*
   server_field_operations_for(const forge::api::core::method_descriptor& descriptor) {
      const auto* fields = forge::api::core::detail::server_fields_for(descriptor);
      return fields != nullptr && fields->active() ? fields : nullptr;
   }

   [[nodiscard]] static bool has_contextual_unary_hook(const forge::api::core::method_descriptor& descriptor) {
      const auto* contextual = forge::api::core::detail::contextual_unary_for(descriptor);
      return contextual != nullptr && static_cast<bool>(*contextual);
   }

   [[nodiscard]] static bool has_contextual_stream_hook(const forge::api::core::method_descriptor& descriptor) {
      const auto* contextual = forge::api::core::detail::contextual_stream_for(descriptor);
      return contextual != nullptr && static_cast<bool>(*contextual);
   }

   [[nodiscard]] static bool has_contextual_method_hook(const forge::api::core::method_descriptor& descriptor) {
      return has_contextual_unary_hook(descriptor) || has_contextual_stream_hook(descriptor);
   }

   [[nodiscard]] static bool has_wire_request_object_hook(const forge::api::core::method_descriptor& descriptor) {
      const auto* fields = server_field_operations_for(descriptor);
      return static_cast<bool>(descriptor.request_encoder) || has_contextual_method_hook(descriptor) ||
             (fields != nullptr && (fields->reset_wire || fields->apply_wire));
   }

   [[nodiscard]] static bool has_fixed_arguments_object_hook(const forge::api::core::method_descriptor& descriptor) {
      const auto* fields = server_field_operations_for(descriptor);
      return has_contextual_method_hook(descriptor) ||
             (fields != nullptr && (fields->reset_fixed || fields->apply_fixed));
   }

   [[nodiscard]] static bool has_response_object_hook(const forge::api::core::method_descriptor& descriptor) {
      return static_cast<bool>(descriptor.response_encoder) || has_contextual_method_hook(descriptor);
   }

   [[nodiscard]] static bool matches_typed_or_byte_only_legacy(std::type_index actual, std::type_index expected,
                                                                 bool byte_only_legacy) {
      return actual == expected || (actual == typeid(void) && byte_only_legacy);
   }

   [[nodiscard]] static bool matches_absent_stream_endpoint(std::type_index actual) {
      return actual == typeid(void);
   }

   [[nodiscard]] static bool matches_stream_endpoint(std::type_index actual, std::type_index expected,
                                                      bool byte_only_legacy) {
      return matches_typed_or_byte_only_legacy(actual, expected, byte_only_legacy);
   }

   template <auto Method, typename Request, typename Response>
   static void validate_route_method_descriptor(const forge::api::core::method_descriptor& descriptor,
                                                std::string_view name) {
      using request_type = std::remove_cvref_t<Request>;
      using wire_request_type = std::remove_cvref_t<detail::http_method_request_t<Method>>;
      using argument_tuple = detail::http_method_argument_tuple_t<Method>;
      using response_type = std::remove_cvref_t<Response>;

      constexpr auto positional = detail::is_positional_http_method_v<Method, request_type>;
      const auto request_matches =
          matches_typed_or_byte_only_legacy(descriptor.request_type, typeid(request_type),
                                            !has_wire_request_object_hook(descriptor)) &&
          matches_typed_or_byte_only_legacy(descriptor.request_type, typeid(wire_request_type),
                                            !has_wire_request_object_hook(descriptor));
      const auto response_matches =
          matches_typed_or_byte_only_legacy(descriptor.response_type, typeid(response_type),
                                            !has_response_object_hook(descriptor)) &&
          matches_typed_or_byte_only_legacy(descriptor.result_type, typeid(response_type),
                                            !has_response_object_hook(descriptor));
      const auto fixed_arguments_match =
          !positional || matches_typed_or_byte_only_legacy(descriptor.fixed_arguments_type, typeid(argument_tuple),
                                                            !has_fixed_arguments_object_hook(descriptor));
      const auto stream_items_match = [&] {
         constexpr auto kind = forge::api::core::method_kind_v<Method>;
         if constexpr (kind == forge::api::core::method_kind::unary) {
            return matches_absent_stream_endpoint(descriptor.input_type) &&
                   matches_absent_stream_endpoint(descriptor.output_type);
         } else {
            const auto byte_only_legacy = !has_contextual_stream_hook(descriptor);
            using endpoint_type = std::remove_cvref_t<forge::api::core::method_argument_t<
                Method, forge::api::core::method_argument_count_v<Method> - 1U>>;
            if constexpr (kind == forge::api::core::method_kind::server_stream) {
               return matches_absent_stream_endpoint(descriptor.input_type) &&
                      matches_stream_endpoint(
                         descriptor.output_type,
                         typeid(typename forge::api::core::stream_writer_traits<endpoint_type>::item_type),
                         byte_only_legacy);
            } else if constexpr (kind == forge::api::core::method_kind::client_stream) {
               return matches_stream_endpoint(
                         descriptor.input_type,
                         typeid(typename forge::api::core::stream_reader_traits<endpoint_type>::item_type),
                         byte_only_legacy) &&
                      matches_absent_stream_endpoint(descriptor.output_type);
            } else {
               return matches_stream_endpoint(
                         descriptor.input_type,
                         typeid(typename forge::api::core::duplex_stream_traits<endpoint_type>::input_type),
                         byte_only_legacy) &&
                      matches_stream_endpoint(
                         descriptor.output_type,
                         typeid(typename forge::api::core::duplex_stream_traits<endpoint_type>::output_type),
                         byte_only_legacy);
            }
         }
      }();
      if (descriptor.kind != forge::api::core::method_kind_v<Method> || !request_matches || !response_matches ||
          !fixed_arguments_match || !stream_items_match) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "HTTP route method descriptor is incompatible with its method binding",
                               forge::exceptions::ctx("method", std::string{name}));
      }
   }

   [[nodiscard]] static std::string encode_error_payload(const forge::api::core::error_payload& error,
                                                         error_codec codec) {
      switch (codec) {
      case error_codec::json: {
         auto encoded = forge::codec::json::write(error);
         if (encoded.ok()) {
            return std::move(encoded.text);
         }
         return forge::codec::json::write(forge::api::core::make_internal_error_payload()).text;
      }
      case error_codec::xml: {
         auto doc = forge::codec::xml::document{
            .root =
               forge::codec::xml::element{
                  .name = "ErrorPayload",
                  .children =
                     {
                        forge::codec::xml::element{.name = "error", .text = error.error},
                        forge::codec::xml::element{.name = "message", .text = error.message},
                        forge::codec::xml::element{.name = "retryable", .text = error.retryable ? "true" : "false"},
                        forge::codec::xml::element{
                           .name = "status_code",
                           .text = std::to_string(static_cast<unsigned>(error.status_code)),
                        },
                        forge::codec::xml::element{
                           .name = "identity",
                           .children =
                              {
                                 forge::codec::xml::element{.name = "category", .text = error.identity.category},
                                 forge::codec::xml::element{.name = "code", .text = std::to_string(error.identity.code)},
                              },
                        },
                     },
               },
         };
         auto encoded = forge::codec::xml::write_value(doc);
         if (encoded.ok()) {
            return std::move(encoded.text);
         }
         return "<ErrorPayload><error>internal</error><message>internal</message><retryable>false</retryable>"
                "<status_code>500</status_code><identity><category>forge.api</category><code>500</code></identity>"
                "</ErrorPayload>";
      }
      }
      return {};
   }

   [[nodiscard]] static status http_status(forge::api::core::status value) noexcept {
      return static_cast<status>(static_cast<unsigned>(value));
   }

   [[nodiscard]] static bool uses_request_body(method verb) noexcept {
      return verb == method::post || verb == method::put || verb == method::patch || verb == method::delete_;
   }

   [[nodiscard]] static bool request_content_type_matches(const request& request_value, body_codec codec) {
      const auto content_type_header = request_value.find(field::content_type);
      if (content_type_header == request_value.end() || content_type_header->value().empty()) {
         return true;
      }
      return detail::media_type_matches(codec, std::string_view{content_type_header->value()});
   }

   static void require_request_content_type(const request& request_value, body_codec codec) {
      if (!request_content_type_matches(request_value, codec)) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::unsupported_media_type,
                               std::string{"HTTP API request body must be "} +
                                   std::string{detail::content_type(codec)});
      }
   }

   [[nodiscard]] static std::optional<std::string> combined_accept_header(const request& request_value) {
      auto combined = std::string{};
      for (const auto& header : request_value.headers()) {
         if (!header_name_equal(header.name, field_name(field::accept))) {
            continue;
         }
         if (!combined.empty()) {
            combined += ", ";
         }
         combined += header.text;
      }
      if (combined.empty()) {
         return std::nullopt;
      }
      return combined;
   }

   static void require_response_accept(const request& request_value, body_codec codec) {
      const auto accept = combined_accept_header(request_value);
      if (accept.has_value() && !detail::accept_allows(codec, *accept)) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::not_acceptable,
                               std::string{"HTTP API response body cannot satisfy Accept"});
      }
   }

   template <typename Response>
   static void require_response_accept_before_handler(const request& request_value, const route_options& options) {
      if constexpr (!detail::is_bytes_response_v<Response> && !detail::is_empty_response_v<Response> &&
                    !std::is_same_v<std::remove_cvref_t<Response>, file_response> &&
                    !detail::is_streaming_response_v<Response> && !detail::is_stream_response_v<Response>) {
         require_response_accept(request_value, options.response_body_codec);
      }
   }

   [[nodiscard]] static std::optional<std::string_view>
   route_value(const route_context& context, const route_options& options, std::string_view name) {
      if (const auto route = context.route_params.find(std::string{name}); route != context.route_params.end()) {
         return route->second;
      }

      const auto configured =
          std::find_if(options.query.begin(), options.query.end(),
                       [&](const field_binding& binding) { return std::string_view{binding.field} == name; });
      if (configured == options.query.end()) {
         return std::nullopt;
      }

      for (const auto& query : context.parsed_target.query_params) {
         if (query.key == configured->name) {
            return query.has_value ? std::string_view{query.value} : std::string_view{"true"};
         }
      }
      return std::nullopt;
   }

   [[nodiscard]] static std::optional<std::string_view>
   query_value(const route_context& context, const route_options& options, std::string_view name) {
      auto wire_name = std::string{name};
      const auto configured =
          std::find_if(options.query.begin(), options.query.end(),
                       [&](const field_binding& binding) { return std::string_view{binding.field} == name; });
      if (configured != options.query.end()) {
         wire_name = configured->name;
      }

      for (const auto& query : context.parsed_target.query_params) {
         if (query.key == wire_name) {
            return query.has_value ? std::string_view{query.value} : std::string_view{"true"};
         }
      }
      return std::nullopt;
   }

   template <typename T> static void parse_http_field(T& target, std::string_view value, std::string_view field) {
      using clean = std::remove_cvref_t<T>;
      if constexpr (detail::is_header<clean>::value) {
         typename detail::is_header<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (detail::is_query<clean>::value) {
         typename detail::is_query<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (detail::is_cookie<clean>::value) {
         typename detail::is_cookie<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (detail::is_body<clean>::value) {
         typename detail::is_body<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (detail::is_form<clean>::value) {
         typename detail::is_form<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (detail::is_form_field<clean>::value) {
         typename detail::is_form_field<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target.value = std::move(parsed);
         target.present = true;
      } else if constexpr (is_optional<clean>::value) {
         typename is_optional<clean>::value_type parsed{};
         parse_http_field(parsed, value, field);
         target = std::move(parsed);
      } else if constexpr (detail::byte_vector_field<clean>::value) {
         auto parsed = forge::codec::json::read<clean>(
             value, {.described_records = forge::codec::json::described_record_policy::exact});
         if (!parsed.ok()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API field value is invalid",
                                  forge::exceptions::ctx("field", std::string{field}));
         }
         target = std::move(parsed.value);
      } else if constexpr (std::is_same_v<clean, std::string> || std::is_same_v<clean, bool> ||
                           (std::is_integral_v<clean> && !std::is_same_v<clean, bool>) ||
                           std::is_floating_point_v<clean> || std::is_enum_v<clean> ||
                           forge::schema::canonical_string_scalar<clean>) {
         try {
            target = forge::schema::parse_scalar_text<clean>(value);
         } catch (const forge::schema::exceptions::invalid_value&) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API field value is invalid",
                                  forge::exceptions::ctx("field", std::string{field}));
         }
      } else {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route field type is not supported",
                               forge::exceptions::ctx("field", std::string{field}));
      }
   }

   template <typename Request>
   [[nodiscard]] static Request make_request_from_route(const route_context& context, const route_options& options,
                                                        bool body_decoded = false) {
      auto request = Request{};
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            if (auto value = route_value(context, options, name); value.has_value()) {
               parse_http_field(request.*member, *value, name);
            }
         });
      }
      return request;
   }

   [[nodiscard]] static std::string header_name_from_field(std::string_view name) {
      auto output = std::string{};
      output.reserve(name.size());
      for (const auto character : name) {
         output.push_back(character == '_' ? '-' : character);
      }
      return output;
   }

   [[nodiscard]] static std::optional<std::string_view> header_value(const request& request_value,
                                                                     std::string_view name) {
      return request_value.header(name);
   }

   [[nodiscard]] static std::optional<std::string_view> cookie_value(const request& request_value,
                                                                     std::string_view name) {
      const auto header = header_value(request_value, "Cookie");
      if (!header.has_value()) {
         return std::nullopt;
      }

      auto text = *header;
      while (!text.empty()) {
         while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == ';')) {
            text.remove_prefix(1U);
         }
         const auto separator = text.find(';');
         const auto pair = text.substr(0, separator);
         const auto equals = pair.find('=');
         if (equals != std::string_view::npos) {
            auto key = pair.substr(0, equals);
            auto value = pair.substr(equals + 1U);
            while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
               key.remove_suffix(1U);
            }
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
               value.remove_prefix(1U);
            }
            if (key == name) {
               return value;
            }
         }
         if (separator == std::string_view::npos) {
            break;
         }
         text.remove_prefix(separator + 1U);
      }
      return std::nullopt;
   }

   [[nodiscard]] static std::string mapped_name_or_default(const std::vector<field_binding>& bindings,
                                                           std::string_view field,
                                                           std::string (*default_name)(std::string_view)) {
      const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const field_binding& binding) {
         return std::string_view{binding.field} == field;
      });
      if (found != bindings.end()) {
         return found->name;
      }
      return default_name(field);
   }

   [[nodiscard]] static std::string identity_name(std::string_view value) {
      return std::string{value};
   }

   template <typename Request>
   static void bind_headers(Request& request, const route_context& context, const route_options& options) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_header<member_type>::value) {
               const auto header_name = mapped_name_or_default(options.headers, name, header_name_from_field);
               if (auto value = header_value(context.request, header_name); value.has_value()) {
                  parse_http_field(request.*member, *value, name);
               }
            }
         });
      }
   }

   template <typename Request> static void bind_cookies(Request& request, const route_context& context) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_cookie<member_type>::value) {
               if (auto value = cookie_value(context.request, name); value.has_value()) {
                  parse_http_field(request.*member, *value, name);
               }
            }
         });
      }
   }

   template <typename Request> static void bind_body_value(Request& request, std::string_view body, body_codec codec) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_body<member_type>::value) {
               using value_type = typename detail::is_body<member_type>::value_type;
               auto value = decode_value<value_type>(body, "http.request.body", codec);
               validate_bound_value_schema(value, "http.request.body");
               (request.*member).value = std::move(value);
               (request.*member).present = true;
               static_cast<void>(name);
            }
         });
      }
   }

   template <typename Request> static void bind_body_bytes(Request& request, std::vector<std::byte> bytes) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char*, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_body_bytes_v<member_type>) {
               request.*member = body_bytes{.bytes = bytes};
            }
         });
      }
   }

   template <typename Request>
   static void bind_body_stream(Request& request, body_reader reader, const route_options& options) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         auto bound = false;
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_body_stream_v<member_type>) {
               if (!options.body_stream_field.has_value() || *options.body_stream_field == name) {
                  request.*member = body_stream{reader};
                  bound = true;
               }
            }
         });
         if (options.body_stream_field.has_value() && !bound) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API body stream field is not described",
                                  forge::exceptions::ctx("field", *options.body_stream_field));
         }
      }
   }

   template <typename Request>
   static void bind_multipart(Request& request, const multipart_form& form, const route_options& options) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_form<member_type>::value) {
               const auto form_name = mapped_name_or_default(options.forms, name, identity_name);
               if (auto value = form.field(form_name); value.has_value()) {
                  parse_http_field(request.*member, *value, name);
               }
            } else if constexpr (detail::is_form_field<member_type>::value) {
               const auto form_name = mapped_name_or_default(options.forms, name, identity_name);
               if (auto value = form.field(form_name); value.has_value()) {
                  parse_http_field(request.*member, *value, name);
               }
            } else if constexpr (detail::is_upload_file_v<member_type>) {
               const auto form_name = mapped_name_or_default(options.forms, name, identity_name);
               for (const auto& file : form.files) {
                  if (file.name == form_name) {
                     request.*member = upload_file{file};
                     break;
                  }
               }
            }
         });
      }
   }

   template <typename Request>
   static void validate_request_schema(const Request& request, std::string_view base_path = "http.request") {
      const auto rules = forge::schema::rules<Request>::define();
      auto diagnostics = rules.validate(request, base_path);
      const auto error =
          std::find_if(diagnostics.begin(), diagnostics.end(), [](const forge::schema::diagnostic& entry) {
             return entry.level == forge::schema::severity::error;
          });
      if (error != diagnostics.end()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               std::string{error->path} + ": " + error->code + ": " + error->message);
      }
   }

   template <typename Value> static void validate_bound_value_schema(const Value& value, std::string_view base_path) {
      if constexpr (forge::reflect::is_described_object_v<Value>) {
         validate_request_schema(value, base_path);
      } else {
         static_cast<void>(value);
         static_cast<void>(base_path);
      }
   }

   template <typename T>
   static constexpr auto positional_argument_needs_stream_v =
       detail::is_body_stream_v<std::remove_cvref_t<T>> || detail::is_body_bytes_v<std::remove_cvref_t<T>> ||
       detail::is_upload_file_v<std::remove_cvref_t<T>> || detail::is_form_field<std::remove_cvref_t<T>>::value ||
       detail::is_form<std::remove_cvref_t<T>>::value;

   template <typename Tuple, std::size_t... Index>
   static consteval bool tuple_needs_stream(std::index_sequence<Index...>) {
      return (positional_argument_needs_stream_v<std::tuple_element_t<Index, Tuple>> || ...);
   }

   template <typename Tuple>
   static constexpr auto tuple_needs_stream_v =
       tuple_needs_stream<Tuple>(std::make_index_sequence<std::tuple_size_v<Tuple>>{});

   template <typename T>
   static constexpr auto is_plain_codec_body_argument_v =
       (forge::reflect::is_described_object_v<std::remove_cvref_t<T>> ||
        is_http_body_sequence<std::remove_cvref_t<T>>::value) &&
       !detail::request_needs_stream_v<std::remove_cvref_t<T>> && !detail::is_header<std::remove_cvref_t<T>>::value &&
       !detail::is_query<std::remove_cvref_t<T>>::value && !detail::is_cookie<std::remove_cvref_t<T>>::value &&
       !detail::is_body<std::remove_cvref_t<T>>::value && !detail::is_form<std::remove_cvref_t<T>>::value &&
       !detail::is_form_field<std::remove_cvref_t<T>>::value && !detail::is_body_stream_v<std::remove_cvref_t<T>> &&
       !detail::is_body_bytes_v<std::remove_cvref_t<T>> && !detail::is_upload_file_v<std::remove_cvref_t<T>>;

   template <auto Method, typename Request>
   static constexpr auto is_positional_http_method_v = detail::is_positional_http_method_v<Method, Request>;

   [[nodiscard]] static bool path_uses_field(std::string_view path, std::string_view name) {
      for (auto index = std::size_t{0}; index != path.size();) {
         if (path[index] != ':') {
            ++index;
            continue;
         }
         auto end = index + 1U;
         while (end != path.size()) {
            const auto value = path[end];
            if (std::isalnum(static_cast<unsigned char>(value)) == 0 && value != '_') {
               break;
            }
            ++end;
         }
         if (std::string_view{path}.substr(index + 1U, end - index - 1U) == name) {
            return true;
         }
         index = end;
      }
      return false;
   }

   [[nodiscard]] static bool query_uses_field(const route_options& options, std::string_view name) {
      return std::find_if(options.query.begin(), options.query.end(), [&](const field_binding& binding) {
                return std::string_view{binding.field} == name;
             }) != options.query.end();
   }

   template <typename Argument>
   static constexpr auto is_allowed_positional_body_argument_v = is_plain_codec_body_argument_v<Argument>;

   template <typename Tuple, std::size_t... Index>
   static void validate_positional_http_arguments(method verb, std::string_view path, const route_options& options,
                                                  const forge::api::core::method_descriptor& method_descriptor,
                                                  bool allow_body,
                                                  bool allow_parameter_wrappers,
                                                  std::index_sequence<Index...>) {
      auto body_candidates = std::size_t{0};
      (([&] {
          using argument_type = std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>;
          const auto name = argument_name(method_descriptor, Index);
          if constexpr (detail::is_header<argument_type>::value ||
                        detail::is_query<argument_type>::value ||
                        detail::is_cookie<argument_type>::value) {
             if (!allow_parameter_wrappers) {
                FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                      "HTTP positional methods cannot use forge::http parameter wrappers",
                                      forge::exceptions::ctx("field", name));
             }
             if (path_uses_field(path, name) ||
                 query_uses_field(options, name)) {
                FORGE_THROW_EXCEPTION(
                   forge::net::http::exceptions::bad_request,
                   "HTTP parameter wrapper conflicts with route binding",
                   forge::exceptions::ctx("field", name));
             }
             return;
          } else if constexpr (detail::is_http_parameter_v<argument_type> ||
                               positional_argument_needs_stream_v<argument_type>) {
             if (allow_parameter_wrappers && allow_body && uses_request_body(verb)) {
                ++body_candidates;
                return;
             }
             FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                   "HTTP positional argument requires request-body mapping",
                                   forge::exceptions::ctx("field", name));
          } else if (path_uses_field(path, name) || query_uses_field(options, name)) {
             return;
          } else if constexpr (is_allowed_positional_body_argument_v<argument_type>) {
             if (uses_request_body(verb)) {
                ++body_candidates;
                return;
             }
             FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                   "HTTP positional body argument requires a body-capable method",
                                   forge::exceptions::ctx("field", name));
          } else {
             FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                   "HTTP positional argument is not bound by route path/query",
                                   forge::exceptions::ctx("field", name));
          }
       }()),
       ...);
      if (body_candidates > 1U) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API request has multiple positional body candidates");
      }
   }

   template <typename Tuple>
   static void validate_positional_http_arguments(method verb, std::string_view path, const route_options& options,
                                                  const forge::api::core::method_descriptor& method_descriptor,
                                                  bool allow_body = true,
                                                  bool allow_parameter_wrappers = false) {
      validate_positional_http_arguments<Tuple>(verb, path, options, method_descriptor, allow_body,
                                                allow_parameter_wrappers,
                                                std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   }

   template <typename Request>
   static void validate_client_stream_request(std::string_view path, const route_options& options) {
      if (!options.forms.empty() || options.body_stream_field.has_value()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API client stream reserves the request body for stream items");
      }
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(std::declval<Request>().*member)>;
            if constexpr (detail::is_header<member_type>::value ||
                          detail::is_query<member_type>::value ||
                          detail::is_cookie<member_type>::value) {
               if (path_uses_field(path, name) ||
                   query_uses_field(options, name)) {
                  FORGE_THROW_EXCEPTION(
                     forge::net::http::exceptions::bad_request,
                     "HTTP parameter wrapper conflicts with route binding",
                     forge::exceptions::ctx("field", name));
               }
               return;
            } else if constexpr (detail::is_http_parameter_v<member_type> ||
                                 detail::is_body_stream_v<member_type> ||
                                 detail::is_body_bytes_v<member_type> ||
                                 detail::is_upload_file_v<member_type>) {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API client stream fixed field requires request-body mapping",
                                     forge::exceptions::ctx("field", name));
            } else if (!path_uses_field(path, name) && !query_uses_field(options, name)) {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API client stream fixed field is not bound by path or query",
                                     forge::exceptions::ctx("field", name));
            }
         });
      }
   }

   [[nodiscard]] static std::string argument_name(const forge::api::core::method_descriptor& method_descriptor,
                                                  std::size_t index) {
      if (index >= method_descriptor.argument_names.size()) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API positional method argument name is missing",
                               forge::exceptions::ctx("method", method_descriptor.name));
      }
      return method_descriptor.argument_names[index];
   }

   [[nodiscard]] static std::string diagnostic_message(const std::vector<forge::schema::diagnostic>& diagnostics,
                                                       std::string_view fallback) {
      const auto error =
          std::find_if(diagnostics.begin(), diagnostics.end(), [](const forge::schema::diagnostic& entry) {
             return entry.level == forge::schema::severity::error;
          });
      if (error == diagnostics.end()) {
         return std::string{fallback};
      }
      return std::string{error->path} + ": " + error->code + ": " + error->message;
   }

   template <typename T>
   [[nodiscard]] static T decode_value(std::string_view body, std::string_view source_name, body_codec codec) {
      switch (codec) {
      case body_codec::json: {
         auto decoded = forge::codec::json::read<T>(
             body, forge::codec::json::read_options{.source_name = std::string{source_name},
                                                    .unknown_fields = forge::codec::json::unknown_field_policy::error});
         if (!decoded.ok()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  diagnostic_message(decoded.diagnostics, "HTTP API JSON request body is invalid"));
         }
         return std::move(decoded.value);
      }
      case body_codec::xml: {
         auto decoded = forge::codec::xml::read<T>(
             body, forge::codec::xml::read_options{.source_name = std::string{source_name},
                                                   .unknown_fields = forge::codec::xml::unknown_field_policy::error});
         if (!decoded.ok()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  diagnostic_message(decoded.diagnostics, "HTTP API XML request body is invalid"));
         }
         return std::move(decoded.value);
      }
      }
      return T{};
   }

   template <typename Argument>
   [[nodiscard]] static bool positional_plain_codec_body_candidate(const route_context& context,
                                                                   const route_options& options,
                                                                   std::string_view name) {
      if constexpr (is_plain_codec_body_argument_v<Argument>) {
         return uses_request_body(context.request.method()) && !context.request.body().empty() &&
                !route_value(context, options, name).has_value();
      } else {
         static_cast<void>(context);
         static_cast<void>(options);
         static_cast<void>(name);
         return false;
      }
   }

   template <typename Argument>
   [[nodiscard]] static bool positional_stream_plain_codec_body_candidate(const route_context& context,
                                                                          const route_options& options,
                                                                          std::string_view name) {
      if constexpr (is_plain_codec_body_argument_v<Argument>) {
         return uses_request_body(context.request.method()) && !route_value(context, options, name).has_value();
      } else {
         static_cast<void>(context);
         static_cast<void>(options);
         static_cast<void>(name);
         return false;
      }
   }

   template <typename Tuple, std::size_t... Index>
   [[nodiscard]] static std::size_t
   positional_plain_codec_body_candidate_count(const route_context& context, const route_options& options,
                                               const forge::api::core::method_descriptor& method_descriptor,
                                               std::index_sequence<Index...>) {
      auto count = std::size_t{0};
      ((count += positional_plain_codec_body_candidate<std::tuple_element_t<Index, Tuple>>(
                     context, options, argument_name(method_descriptor, Index))
                     ? 1U
                     : 0U),
       ...);
      return count;
   }

   template <typename Tuple>
   [[nodiscard]] static std::size_t
   positional_plain_codec_body_candidate_count(const route_context& context, const route_options& options,
                                               const forge::api::core::method_descriptor& method_descriptor) {
      return positional_plain_codec_body_candidate_count<Tuple>(context, options, method_descriptor,
                                                                std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   }

   template <typename Tuple, std::size_t... Index>
   [[nodiscard]] static std::size_t
   positional_stream_plain_codec_body_candidate_count(const route_context& context, const route_options& options,
                                                      const forge::api::core::method_descriptor& method_descriptor,
                                                      std::index_sequence<Index...>) {
      auto count = std::size_t{0};
      ((count += positional_stream_plain_codec_body_candidate<std::tuple_element_t<Index, Tuple>>(
                     context, options, argument_name(method_descriptor, Index))
                     ? 1U
                     : 0U),
       ...);
      return count;
   }

   template <typename Tuple>
   [[nodiscard]] static std::size_t
   positional_stream_plain_codec_body_candidate_count(const route_context& context, const route_options& options,
                                                      const forge::api::core::method_descriptor& method_descriptor) {
      return positional_stream_plain_codec_body_candidate_count<Tuple>(
          context, options, method_descriptor, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   }

   template <typename Value>
   static void require_body_route_consistency(const Value& value, const route_context& context,
                                              const route_options& options) {
      if constexpr (forge::reflect::is_described_object_v<Value>) {
         forge::reflect::for_each_member<Value>([&](const char* name, auto member) {
            if (auto route = route_value(context, options, name); route.has_value()) {
               auto expected = std::remove_cvref_t<decltype(value.*member)>{};
               parse_http_field(expected, *route, name);
               require_equal(value.*member, expected, name);
            }
         });
      } else {
         static_cast<void>(value);
         static_cast<void>(context);
         static_cast<void>(options);
      }
   }

   template <typename Argument>
   [[nodiscard]] static Argument make_positional_argument_from_http(const route_context& context,
                                                                    const route_options& options, std::string_view name,
                                                                    bool decode_plain_codec_body = false) {
      using clean = std::remove_cvref_t<Argument>;
      auto result = clean{};
      if constexpr (detail::is_header<clean>::value) {
         const auto header_name = mapped_name_or_default(options.headers, name, header_name_from_field);
         if (auto value = header_value(context.request, header_name); value.has_value()) {
            parse_http_field(result, *value, name);
         }
      } else if constexpr (detail::is_query<clean>::value) {
         if (auto value = query_value(context, options, name); value.has_value()) {
            parse_http_field(result, *value, name);
         }
      } else if constexpr (detail::is_cookie<clean>::value) {
         if (auto value = cookie_value(context.request, name); value.has_value()) {
            parse_http_field(result, *value, name);
         }
      } else if constexpr (detail::is_body<clean>::value) {
         if (!context.request.body().empty()) {
            require_request_content_type(context.request, options.request_body_codec);
            using value_type = typename detail::is_body<clean>::value_type;
            result.value =
                decode_value<value_type>(context.request.body(), "http.request.body", options.request_body_codec);
            validate_bound_value_schema(result.value, "http.request.body");
            result.present = true;
         }
      } else if constexpr (detail::is_form<clean>::value || detail::is_form_field<clean>::value ||
                           detail::is_upload_file_v<clean> || detail::is_body_bytes_v<clean> ||
                           detail::is_body_stream_v<clean>) {
         static_cast<void>(context);
         static_cast<void>(options);
      } else if (auto value = route_value(context, options, name); value.has_value()) {
         parse_http_field(result, *value, name);
      } else if constexpr (is_plain_codec_body_argument_v<clean>) {
         if (decode_plain_codec_body) {
            require_request_content_type(context.request, options.request_body_codec);
            result = decode_value<clean>(context.request.body(), "http.request.body", options.request_body_codec);
            validate_bound_value_schema(result, "http.request.body");
            require_body_route_consistency(result, context, options);
         }
      }
      return result;
   }

   template <typename Tuple, std::size_t... Index>
   [[nodiscard]] static Tuple
   make_positional_arguments_from_http(const route_context& context, const route_options& options,
                                       const forge::api::core::method_descriptor& method_descriptor,
                                       std::index_sequence<Index...>) {
      const auto body_candidates =
          positional_plain_codec_body_candidate_count<Tuple>(context, options, method_descriptor);
      if (body_candidates > 1U) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API request has multiple positional body candidates");
      }
      return Tuple{make_positional_argument_from_http<std::tuple_element_t<Index, Tuple>>(
          context, options, argument_name(method_descriptor, Index),
          positional_plain_codec_body_candidate<std::tuple_element_t<Index, Tuple>>(
              context, options, argument_name(method_descriptor, Index)))...};
   }

   template <typename Tuple>
   [[nodiscard]] static Tuple
   make_positional_arguments_from_http(const route_context& context, const route_options& options,
                                       const forge::api::core::method_descriptor& method_descriptor) {
      return make_positional_arguments_from_http<Tuple>(context, options, method_descriptor,
                                                        std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   }

   template <typename Argument>
   static boost::asio::awaitable<Argument>
   make_positional_argument_from_stream(stream_request& stream, const route_options& options,
                                        const multipart_form* form, const std::optional<std::string>* plain_codec_body,
                                        std::string_view name, bool decode_plain_codec_body = false) {
      using clean = std::remove_cvref_t<Argument>;
      if constexpr (detail::is_body_stream_v<clean>) {
         co_return body_stream{std::move(stream.body)};
      } else if constexpr (detail::is_body_bytes_v<clean>) {
         auto text = co_await stream.body.async_read_all();
         auto bytes = std::vector<std::byte>(text.size());
         std::memcpy(bytes.data(), text.data(), text.size());
         co_return body_bytes{.bytes = std::move(bytes)};
      } else if constexpr (detail::is_form<clean>::value || detail::is_form_field<clean>::value) {
         auto result = clean{};
         if (form != nullptr) {
            const auto form_name = mapped_name_or_default(options.forms, name, identity_name);
            if (auto value = form->field(form_name); value.has_value()) {
               parse_http_field(result, *value, name);
            }
         }
         co_return result;
      } else if constexpr (detail::is_upload_file_v<clean>) {
         if (form != nullptr) {
            const auto form_name = mapped_name_or_default(options.forms, name, identity_name);
            for (const auto& file : form->files) {
               if (file.name == form_name) {
                  co_return upload_file{file};
               }
            }
         }
         co_return upload_file{};
      } else if constexpr (detail::is_body<clean>::value) {
         auto result = clean{};
         auto text = co_await stream.body.async_read_all();
         if (!text.empty()) {
            require_request_content_type(stream.context.request, options.request_body_codec);
            using value_type = typename detail::is_body<clean>::value_type;
            result.value = decode_value<value_type>(text, "http.request.body", options.request_body_codec);
            validate_bound_value_schema(result.value, "http.request.body");
            result.present = true;
         }
         co_return result;
      } else {
         if constexpr (is_plain_codec_body_argument_v<clean>) {
            if (decode_plain_codec_body && plain_codec_body != nullptr && plain_codec_body->has_value() &&
                !plain_codec_body->value().empty()) {
               require_request_content_type(stream.context.request, options.request_body_codec);
               auto result =
                   decode_value<clean>(plain_codec_body->value(), "http.request.body", options.request_body_codec);
               validate_bound_value_schema(result, "http.request.body");
               require_body_route_consistency(result, stream.context, options);
               co_return result;
            }
         }
         co_return make_positional_argument_from_http<Argument>(stream.context, options, name);
      }
   }

   template <typename Tuple, std::size_t... Index>
   static boost::asio::awaitable<Tuple> make_positional_arguments_from_stream_impl(
       stream_request& stream, const route_options& options,
       const forge::api::core::method_descriptor& method_descriptor, const multipart_form* form,
       const std::optional<std::string>* plain_codec_body, std::index_sequence<Index...>) {
      co_return Tuple{co_await make_positional_argument_from_stream<std::tuple_element_t<Index, Tuple>>(
          stream, options, form, plain_codec_body, argument_name(method_descriptor, Index),
          positional_stream_plain_codec_body_candidate<std::tuple_element_t<Index, Tuple>>(
              stream.context, options, argument_name(method_descriptor, Index)))...};
   }

   template <typename Tuple>
   static boost::asio::awaitable<Tuple>
   make_positional_arguments_from_stream(stream_request& stream, const route_options& options,
                                         const forge::api::core::method_descriptor& method_descriptor) {
      auto form = std::optional<multipart_form>{};
      auto plain_codec_body = std::optional<std::string>{};
      const auto body_candidates =
          positional_stream_plain_codec_body_candidate_count<Tuple>(stream.context, options, method_descriptor);
      if (body_candidates > 1U) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API request has multiple positional body candidates");
      }
      if constexpr (tuple_needs_stream_v<Tuple>) {
         constexpr auto multipart_needed = []<std::size_t... Index>(std::index_sequence<Index...>) consteval {
            return ((detail::is_form<std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>>::value ||
                     detail::is_form_field<std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>>::value ||
                     detail::is_upload_file_v<std::remove_cvref_t<std::tuple_element_t<Index, Tuple>>>) ||
                    ...);
         }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
         if constexpr (multipart_needed) {
            const auto content_type = stream.context.request.find(field::content_type);
            auto reader = upload_reader{std::move(stream.body)};
            form = co_await reader.async_read_multipart(content_type == stream.context.request.end()
                                                            ? std::string_view{}
                                                            : std::string_view{content_type->value()});
         }
      }
      if (body_candidates == 1U) {
         plain_codec_body = co_await stream.body.async_read_all();
      }
      co_return co_await make_positional_arguments_from_stream_impl<Tuple>(
          stream, options, method_descriptor, form.has_value() ? &*form : nullptr, &plain_codec_body,
          std::make_index_sequence<std::tuple_size_v<Tuple>>{});
   }

   template <typename Request>
   [[nodiscard]] static Request decode_request_body(std::string_view body, body_codec codec) {
      return decode_value<Request>(body, "http.request", codec);
   }

   template <typename Request>
   static void bind_route_query_headers(Request& request, const route_context& context, const route_options& options,
                                        bool body_was_decoded) {
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         forge::reflect::for_each_member<Request>([&](const char* name, auto member) {
            using member_type = std::remove_cvref_t<decltype(request.*member)>;
            if constexpr (detail::is_query<member_type>::value) {
               if (auto value = query_value(context, options, name); value.has_value()) {
                  parse_http_field(request.*member, *value, name);
               }
               return;
            }
            if constexpr (detail::is_header<member_type>::value || detail::is_cookie<member_type>::value ||
                          detail::is_body<member_type>::value || detail::is_form<member_type>::value ||
                          detail::is_form_field<member_type>::value || detail::is_body_stream_v<member_type> ||
                          detail::is_body_bytes_v<member_type> || detail::is_upload_file_v<member_type>) {
               return;
            }
            if (auto value = route_value(context, options, name); value.has_value()) {
               auto expected = std::remove_cvref_t<decltype(request.*member)>{};
               parse_http_field(expected, *value, name);
               if (body_was_decoded) {
                  require_equal(request.*member, expected, name);
               } else {
                  request.*member = std::move(expected);
               }
            }
         });
      }
      bind_headers(request, context, options);
      bind_cookies(request, context);
   }

   template <typename Request>
   [[nodiscard]] static Request make_request_from_http(const route_context& context, const route_options& options) {
      return make_request_from_http<Request>(context, options, true);
   }

   template <typename Request>
   [[nodiscard]] static Request make_request_from_http(const route_context& context, const route_options& options,
                                                       bool validate_schema) {
      auto request = Request{};
      const auto has_body = uses_request_body(context.request.method()) && !context.request.body().empty();
      auto whole_request_body_was_decoded = false;
      if constexpr (request_body_source_count_v<Request> > 1U) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                               "HTTP API request DTO has multiple body sources");
      } else if constexpr (request_has_body_v<Request>) {
         if (has_body) {
            require_request_content_type(context.request, options.request_body_codec);
            bind_body_value(request, context.request.body(), options.request_body_codec);
         }
      } else if constexpr (!detail::request_needs_stream_v<Request>) {
         if (has_body) {
            require_request_content_type(context.request, options.request_body_codec);
            request = decode_request_body<Request>(context.request.body(), options.request_body_codec);
            whole_request_body_was_decoded = true;
         }
      }
      bind_route_query_headers(request, context, options, whole_request_body_was_decoded);
      if (validate_schema) {
         validate_request_schema(request);
      }
      return request;
   }

   template <typename Request>
   static boost::asio::awaitable<Request> make_request_from_stream(stream_request& stream,
                                                                   const route_options& options) {
      auto request = make_request_from_http<Request>(stream.context, options, false);
      if constexpr (forge::reflect::is_described_object_v<Request>) {
         if constexpr (request_body_source_count_v<Request> > 1U) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API request DTO has multiple body sources");
         } else if constexpr (request_has_multipart_v<Request>) {
            const auto content_type = stream.context.request.find(field::content_type);
            auto reader = upload_reader{std::move(stream.body)};
            auto form = co_await reader.async_read_multipart(content_type == stream.context.request.end()
                                                                 ? std::string_view{}
                                                                 : std::string_view{content_type->value()});
            bind_multipart(request, form, options);
         } else if constexpr (request_has_body_bytes_v<Request>) {
            auto text = co_await stream.body.async_read_all();
            auto bytes = std::vector<std::byte>(text.size());
            std::memcpy(bytes.data(), text.data(), text.size());
            bind_body_bytes(request, std::move(bytes));
         } else if constexpr (request_has_body_stream_v<Request>) {
            bind_body_stream(request, std::move(stream.body), options);
         } else if constexpr (request_has_body_v<Request>) {
            auto text = co_await stream.body.async_read_all();
            if (!text.empty()) {
               require_request_content_type(stream.context.request, options.request_body_codec);
               bind_body_value(request, text, options.request_body_codec);
            }
         } else if (uses_request_body(stream.context.request.method())) {
            auto text = co_await stream.body.async_read_all();
            if (!text.empty()) {
               require_request_content_type(stream.context.request, options.request_body_codec);
               request = decode_request_body<Request>(text, options.request_body_codec);
               bind_route_query_headers(request, stream.context, options, true);
            }
         }
      }
      validate_request_schema(request);
      co_return request;
   }

   template <typename T> static void require_equal(const T& actual, const T& expected, std::string_view field) {
      using clean = std::remove_cvref_t<T>;
      if constexpr (is_optional<clean>::value) {
         if (actual.has_value() != expected.has_value()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route field disagrees with body",
                                  forge::exceptions::ctx("field", std::string{field}));
         }
         if (actual) {
            require_equal(*actual, *expected, field);
         }
      } else if constexpr (std::same_as<clean, std::string> || std::same_as<clean, bool> ||
                           (std::integral<clean> && !std::same_as<clean, bool>) || std::floating_point<clean> ||
                           std::is_enum_v<clean>) {
         if (!(actual == expected)) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route field disagrees with body",
                                  forge::exceptions::ctx("field", std::string{field}));
         }
      } else {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request, "HTTP API route field type is not comparable",
                               forge::exceptions::ctx("field", std::string{field}));
      }
   }

   [[nodiscard]] static forge::api::core::error_payload make_http_error_payload(std::string error, std::string message,
                                                                                status status_code) {
      return forge::api::core::error_payload{
          .error = std::move(error),
          .message = std::move(message),
          .retryable = false,
          .status_code = forge::api::core::status::invalid_argument,
          .identity =
              {
                  .category = "forge.api.http",
                  .code = static_cast<std::uint32_t>(status_code),
              },
      };
   }

   [[nodiscard]] static response make_http_error_response(const request& request_value, std::string error,
                                                          std::string message, status status_code,
                                                          const route_options& options,
                                                          const std::shared_ptr<endpoint_state>& endpoint = {}) {
      const auto payload = make_http_error_payload(std::move(error), std::move(message), status_code);
      auto output =
          make_text_response(request_value, status_code, encode_error_payload(payload, options.error_body_codec),
                             std::string{detail::content_type(options.error_body_codec)});
      merge_endpoint_headers(output, endpoint);
      apply_cache_policy(output, options);
      return output;
   }

   [[nodiscard]] static response make_error_response(const request& request_value,
                                                     const forge::api::core::error_payload& payload,
                                                     const route_options& options,
                                                     const std::shared_ptr<endpoint_state>& endpoint = {}) {
      auto output = make_text_response(request_value, http_status(payload.status_code),
                                       encode_error_payload(payload, options.error_body_codec),
                                       std::string{detail::content_type(options.error_body_codec)});
      merge_endpoint_headers(output, endpoint);
      apply_cache_policy(output, options);
      return output;
   }

   [[nodiscard]] static response make_validation_response(const request& request_value, std::string_view message,
                                                          const route_options& options,
                                                          const std::shared_ptr<endpoint_state>& endpoint = {}) {
      auto output = make_text_response(request_value, static_cast<status>(422),
                                       encode_error_payload(
                                           forge::api::core::error_payload{
                                               .error = "validation_error",
                                               .message = std::string{message},
                                               .retryable = false,
                                               .status_code = forge::api::core::status::invalid_argument,
                                               .identity =
                                                   {
                                                       .category = "forge.api.http",
                                                       .code = 422,
                                                   },
                                           },
                                           options.error_body_codec),
                                       std::string{detail::content_type(options.error_body_codec)});
      merge_endpoint_headers(output, endpoint);
      apply_cache_policy(output, options);
      return output;
   }

   [[nodiscard]] static bool same_header_name(std::string_view left, std::string_view right) noexcept {
      if (left.size() != right.size()) {
         return false;
      }
      for (auto index = std::size_t{0}; index != left.size(); ++index) {
         if (std::tolower(static_cast<unsigned char>(left[index])) !=
             std::tolower(static_cast<unsigned char>(right[index]))) {
            return false;
         }
      }
      return true;
   }

   [[nodiscard]] static bool endpoint_header_is_protocol_framing(std::string_view name) noexcept {
      return same_header_name(name, field_name(field::content_length)) ||
             same_header_name(name, field_name(field::transfer_encoding)) ||
             same_header_name(name, field_name(field::content_range));
   }

   static void merge_endpoint_headers(response& target, const std::shared_ptr<endpoint_state>& state) {
      if (!state) {
         return;
      }
      const auto& endpoint_response = endpoint_state_access::response(state);
      for (const auto& header : endpoint_response.headers()) {
         if (endpoint_header_is_protocol_framing(header.name)) {
            continue;
         }
         if (same_header_name(header.name, "Set-Cookie")) {
            target.insert(header.name, header.text);
            continue;
         }
         target.set(header.name, header.text);
      }
   }

   static void apply_cache_policy(response& target, const route_options& options) {
      switch (options.cache) {
      case cache_policy::unspecified:
         break;
      case cache_policy::no_store:
         target.set("Cache-Control", "no-store");
         break;
      }
   }

   template <typename Request>
   [[nodiscard]] static std::shared_ptr<endpoint_state> make_endpoint_state(const request& request_value,
                                                                            status success_status) {
      if constexpr (std::is_base_of_v<endpoint_request, std::remove_cvref_t<Request>>) {
         auto response_value = response{success_status, request_value.version()};
         response_value.keep_alive(request_value.keep_alive());
         return endpoint_state_access::make(request_value, std::move(response_value));
      } else {
         return {};
      }
   }

   template <typename Request>
   static void attach_endpoint_state(Request& request_value, const std::shared_ptr<endpoint_state>& state) {
      if constexpr (std::is_base_of_v<endpoint_request, std::remove_cvref_t<Request>>) {
         endpoint_state_access::attach(request_value, state);
      }
   }

   template <typename Response>
   [[nodiscard]] static std::string encode_response_body(const Response& value, body_codec codec) {
      switch (codec) {
      case body_codec::json: {
         auto encoded = forge::codec::json::write(value);
         if (!encoded.ok()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API response cannot be encoded as JSON");
         }
         return std::move(encoded.text);
      }
      case body_codec::xml: {
         auto encoded = forge::codec::xml::write(value);
         if (!encoded.ok()) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API response cannot be encoded as XML");
         }
         return std::move(encoded.text);
      }
      }
      return {};
   }

   template <typename Response>
   [[nodiscard]] static response make_success_response(const request& request_value, status success_status,
                                                       const Response& value, const route_options& options,
                                                       const std::shared_ptr<endpoint_state>& endpoint = {}) {
      require_endpoint_success_status(success_status, endpoint);
      auto output = response{};
      if constexpr (detail::is_bytes_response_v<Response>) {
         require_route_success_status(success_status, value.status_code);
         auto body = std::string{reinterpret_cast<const char*>(value.bytes.data()), value.bytes.size()};
         output = make_text_response(request_value, success_status, std::move(body), value.content_type);
      } else if constexpr (detail::is_empty_response_v<Response>) {
         require_route_success_status(success_status, value.status_code);
         output = response{success_status, request_value.version()};
         output.prepare_payload();
         output.keep_alive(request_value.keep_alive());
      } else {
         require_response_accept(request_value, options.response_body_codec);
         auto encoded = encode_response_body(value, options.response_body_codec);
         output = make_text_response(request_value, success_status, std::move(encoded),
                                     std::string{detail::content_type(options.response_body_codec)});
      }
      if (request_value.method() == method::head) {
         output.body().clear();
      }
      merge_endpoint_headers(output, endpoint);
      apply_cache_policy(output, options);
      return output;
   }

   static void require_route_success_status(status declared, status actual) {
      if (actual != declared) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::internal,
                               "HTTP response status does not match declared route",
                               forge::exceptions::ctx("declared", static_cast<std::uint16_t>(declared)),
                               forge::exceptions::ctx("actual", static_cast<std::uint16_t>(actual)));
      }
   }

   static void require_endpoint_success_status(status declared, const std::shared_ptr<endpoint_state>& endpoint) {
      if (endpoint) {
         require_route_success_status(declared, endpoint_state_access::response(endpoint).result());
      }
   }

   static void require_file_response_status(status declared, status actual, bool path_backed) {
      if (!path_backed) {
         require_route_success_status(declared, actual);
         return;
      }

      require_route_success_status(declared, status::ok);
      if (actual != status::ok && actual != status::partial_content && actual != status::not_modified &&
          actual != status::not_found && actual != status::range_not_satisfiable) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::internal,
                               "HTTP path-backed file response produced an unsupported status",
                               forge::exceptions::ctx("actual", static_cast<std::uint16_t>(actual)));
      }
   }

   template <typename Response>
   static boost::asio::awaitable<stream_response>
   make_success_stream_response(const request& request_value, status success_status, Response value,
                                const route_options& options, const std::shared_ptr<endpoint_state>& endpoint = {},
                                const stream_request* stream_request_value = nullptr) {
      require_endpoint_success_status(success_status, endpoint);
      auto output = stream_response{};
      auto endpoint_headers_merged = false;
      if constexpr (std::is_same_v<std::remove_cvref_t<Response>, file_response>) {
         const auto path_backed = value.path_backed();
         output = co_await std::move(value).materialize(request_value);
         require_file_response_status(success_status, output.head.result(), path_backed);
      } else if constexpr (detail::is_streaming_response_v<Response>) {
         output = stream_request_value != nullptr ? std::move(value).materialize(*stream_request_value, success_status)
                                                  : std::move(value).materialize(request_value, success_status);
      } else if constexpr (detail::is_stream_response_v<Response>) {
         output = std::move(value);
      } else {
         output =
             stream_response::buffered(make_success_response(request_value, success_status, value, options, endpoint));
         endpoint_headers_merged = true;
      }
      if constexpr (!std::is_same_v<std::remove_cvref_t<Response>, file_response>) {
         require_route_success_status(success_status, output.head.result());
      }
      if (!endpoint_headers_merged) {
         merge_endpoint_headers(output.head, endpoint);
      }
      apply_cache_policy(output.head, options);
      if (request_value.method() == forge::net::http::method::head) {
         output.body = {};
      }
      co_return output;
   }

   template <typename Interface, typename Request>
   [[nodiscard]] static forge::api::core::bytes validate_local_request(const forge::api::core::pinned_binding_plan& plan,
                                                                       std::string_view name, const Request& request) {
      const auto* descriptor = plan.describe(Interface::ref());
      const auto* method = descriptor == nullptr ? nullptr : forge::api::core::find_method(*descriptor, name);
      if (method == nullptr) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                               "HTTP API method is not installed in the local registry");
      }
      if (!method->request_validator && !method->response_validator) {
         return {};
      }
      if (method->request_type != typeid(Request) || !method->request_encoder) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "HTTP API method request validator has no compatible encoder");
      }
      auto payload = method->request_encoder(&request);
      if (method->request_validator) {
         method->request_validator(payload);
      }
      return payload;
   }

   template <typename Interface, typename Response>
   static void validate_local_response(const forge::api::core::pinned_binding_plan& plan, std::string_view name,
                                       const forge::api::core::bytes& request, const Response& response) {
      const auto* descriptor = plan.describe(Interface::ref());
      const auto* method = descriptor == nullptr ? nullptr : forge::api::core::find_method(*descriptor, name);
      if (method == nullptr || !method->response_validator) {
         return;
      }
      if (method->response_type != typeid(Response) || !method->response_encoder) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "HTTP API method response validator has no compatible encoder");
      }
      method->response_validator(request, method->response_encoder(&response));
   }

   template <auto Method, typename Interface, typename Request, typename Response>
   static boost::asio::awaitable<Response>
   invoke_local(const forge::api::core::pinned_binding_plan& plan, std::string_view name,
                const forge::api::core::method_descriptor& canonical_method, Request request,
                const forge::api::core::trusted_invocation& trusted = {}) {
      const auto* descriptor = plan.describe(Interface::ref());
      const auto* method = descriptor == nullptr ? nullptr : forge::api::core::find_method(*descriptor, name);
      if (method == nullptr) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                               "HTTP API method is not installed in the local registry");
      }
      validate_route_method_descriptor<Method, Request, Response>(*method, name);
      forge::api::core::detail::apply_wire_request(canonical_method, &request, trusted);
      auto request_payload = validate_local_request<Interface>(plan, name, request);
      auto implementation = plan.get<Interface>(Interface::ref());
      auto response = co_await std::invoke(Method, *implementation.shared(), std::move(request));
      validate_local_response<Interface>(plan, name, request_payload, response);
      co_return response;
   }

   template <auto Method, typename Interface, typename Request, typename Tuple, typename Response>
   static boost::asio::awaitable<Response>
   invoke_local_arguments(const forge::api::core::pinned_binding_plan& plan, std::string_view name,
                          const forge::api::core::method_descriptor& canonical_method, Tuple arguments,
                          const forge::api::core::trusted_invocation& trusted = {}) {
      const auto* descriptor = plan.describe(Interface::ref());
      const auto* method = descriptor == nullptr ? nullptr : forge::api::core::find_method(*descriptor, name);
      if (method == nullptr) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                               "HTTP API method is not installed in the local registry");
      }
      validate_route_method_descriptor<Method, Request, Response>(*method, name);
      forge::api::core::detail::apply_fixed_request(canonical_method, &arguments, trusted);
      auto request_payload = [&] {
         if constexpr (std::tuple_size_v<Tuple> == 1U) {
            return validate_local_request<Interface>(plan, name, std::get<0>(arguments));
         } else {
            return validate_local_request<Interface>(plan, name, arguments);
         }
      }();
      auto implementation = plan.get<Interface>(Interface::ref());
      auto response = co_await std::apply(
          [&](auto&&... args) {
             return std::invoke(Method, *implementation.shared(), std::forward<decltype(args)>(args)...);
          },
          std::move(arguments));
      validate_local_response<Interface>(plan, name, request_payload, response);
      co_return response;
   }

   static stream_response buffered(response value) {
      return stream_response::buffered(std::move(value));
   }

   template <typename Interface>
   [[nodiscard]] static forge::api::core::frame
   make_live_request(std::string name, forge::api::core::bytes payload) {
      return forge::api::core::frame{
         .kind = forge::api::core::frame_kind::request,
         .id = {.value = 1},
         .api = Interface::ref(),
         .method = std::move(name),
         .codec = {.value = "forge.raw"},
         .payload = std::move(payload),
      };
   }

   template <typename Response> static void validate_response_file_option(const route_options& options) {
      constexpr auto response_is_file = std::is_same_v<std::remove_cvref_t<Response>, file_response>;
      constexpr auto response_is_stream = detail::is_streaming_response_v<Response>;
      if constexpr (response_is_file) {
         if (!options.response_file) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API file response route requires FORGE_HTTP_RESPONSE_FILE");
         }
         if (options.success_status != status::ok) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API file response route requires a 200 success status");
         }
      } else {
         if (options.response_file) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "FORGE_HTTP_RESPONSE_FILE requires forge::net::http::file_response");
         }
      }
      if constexpr (response_is_stream) {
         if (!options.response_stream) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "HTTP API streaming response route requires FORGE_HTTP_RESPONSE_STREAM");
         }
      } else {
         if (options.response_stream) {
            FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                  "FORGE_HTTP_RESPONSE_STREAM requires forge::net::http::streaming_response");
         }
      }
   }

   template <auto Method, typename Request, typename Response>
   [[nodiscard]] mount_action make_step(method verb, std::string path, route_options options,
                                        std::string explicit_name) {
      using interface_type = typename method_class<decltype(Method)>::type;
      using argument_tuple = detail::http_method_argument_tuple_t<Method>;
      auto plan = plan_;
      auto name = explicit_name.empty() ? method_name<interface_type, Request, Response>() : std::move(explicit_name);
      return [plan = std::move(plan), verb, path = std::move(path), options = std::move(options),
              name = std::move(name)](router& target, std::string_view base_path) {
         validate_response_file_option<Response>(options);
         const auto api_descriptor = interface_type::describe();
         const auto& mount_method_descriptor = require_route_method_descriptor(&api_descriptor, name);
         validate_route_method_descriptor<Method, Request, Response>(mount_method_descriptor, name);
         if (plan.local != nullptr) {
            const auto& installed_method_descriptor =
                require_route_method_descriptor(plan.local->describe(interface_type::ref()), name);
            validate_route_method_descriptor<Method, Request, Response>(installed_method_descriptor, name);
         }
         if constexpr (forge::api::core::method_kind_v<Method> ==
                       forge::api::core::method_kind::bidirectional_stream) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                                  "HTTP/1.1 does not support bidirectional API streams",
                                  forge::exceptions::ctx("method", name));
         }
         if constexpr (is_positional_http_method_v<Method, Request>) {
            if (std::tuple_size_v<argument_tuple> != 0U && mount_method_descriptor.argument_names.empty()) {
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                     "HTTP API positional method is missing argument metadata");
            }
            validate_positional_http_arguments<argument_tuple>(
               verb, path, options, mount_method_descriptor,
               forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::client_stream,
               forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::unary);
         } else if constexpr (forge::api::core::method_kind_v<Method> ==
                              forge::api::core::method_kind::client_stream) {
            validate_client_stream_request<Request>(path, options);
         }
         auto canonical_method_descriptor = mount_method_descriptor;
         auto mounted_path = join_path(base_path, path);
         if constexpr (forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::unary) {
            auto stream_handler = [plan, options, name,
                                   canonical_method_descriptor](stream_request& request_value)
                -> boost::asio::awaitable<stream_response> {
               if (plan.local == nullptr) {
                  FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                                        "HTTP API binding has no local registry");
               }
               auto pinned = plan.pin(interface_type::ref());
               const auto& method_descriptor = canonical_method_descriptor;
               auto trusted = detail::trusted_invocation_for(request_value.context);
               try {
                  const auto& installed_method_descriptor = require_route_method_descriptor(
                     pinned.describe(interface_type::ref()), name);
                  validate_route_method_descriptor<Method, Request, Response>(installed_method_descriptor, name);
                  detail::validate_live_stream_headers(
                     request_value.context.request, forge::api::core::method_kind_v<Method>);

                  auto payload = forge::api::core::bytes{};
                  if constexpr (is_positional_http_method_v<Method, Request>) {
                     if constexpr (std::tuple_size_v<argument_tuple> != 0U) {
                        if (method_descriptor.argument_names.empty()) {
                           FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                                 "HTTP API positional method is missing argument metadata");
                        }
                     }
                     if constexpr (forge::api::core::method_kind_v<Method> ==
                                   forge::api::core::method_kind::server_stream) {
                        auto arguments = co_await make_positional_arguments_from_stream<argument_tuple>(
                           request_value, options, method_descriptor);
                        forge::api::core::detail::apply_fixed_request(method_descriptor, &arguments, trusted);
                        payload = forge::api::core::detail::encode_fixed_proxy_arguments<Method>(
                           method_descriptor, arguments,
                           std::make_index_sequence<std::tuple_size_v<argument_tuple>>{});
                     } else {
                        auto arguments = make_positional_arguments_from_http<argument_tuple>(
                           request_value.context, options, method_descriptor);
                        forge::api::core::detail::apply_fixed_request(method_descriptor, &arguments, trusted);
                        payload = forge::api::core::detail::encode_fixed_proxy_arguments<Method>(
                           method_descriptor, arguments,
                           std::make_index_sequence<std::tuple_size_v<argument_tuple>>{});
                     }
                  } else {
                     if constexpr (forge::api::core::method_kind_v<Method> ==
                                   forge::api::core::method_kind::server_stream) {
                        auto request = co_await make_request_from_stream<Request>(request_value, options);
                        forge::api::core::detail::apply_wire_request(method_descriptor, &request, trusted);
                        payload = forge::api::core::detail::encode_owned_request(method_descriptor, request);
                     } else {
                        auto request = make_request_from_http<Request>(request_value.context, options);
                        forge::api::core::detail::apply_wire_request(method_descriptor, &request, trusted);
                        payload = forge::api::core::detail::encode_owned_request(method_descriptor, request);
                     }
                  }

                  auto request = make_live_request<interface_type>(name, std::move(payload));
                  if constexpr (forge::api::core::method_kind_v<Method> ==
                                forge::api::core::method_kind::server_stream) {
                     auto output = co_await detail::make_live_server_stream_response(
                         std::move(pinned), std::move(request), request_value, options.success_status,
                         std::move(trusted));
                     apply_cache_policy(output.head, options);
                     co_return output;
                  } else {
                     auto terminal = co_await detail::dispatch_live_client_stream(
                         std::move(pinned), std::move(request), std::move(request_value.body),
                         method_descriptor.input_decoder, std::move(trusted));
                     if (terminal.kind == forge::api::core::frame_kind::error) {
                        auto payload = forge::raw::unpack_exact<forge::api::core::error_payload>(terminal.payload);
                        co_return buffered(make_error_response(request_value.context.request, payload, options));
                     }
                     auto output = detail::make_live_terminal_response(
                        request_value.context.request, options.success_status, terminal);
                     apply_cache_policy(output, options);
                     co_return buffered(std::move(output));
                  }
               } catch (const forge::net::http::exceptions::unsupported_media_type& error) {
                  co_return buffered(make_http_error_response(request_value.context.request, "unsupported_media_type",
                                                              error.message(), status::unsupported_media_type,
                                                              options));
               } catch (const forge::net::http::exceptions::not_acceptable& error) {
                  co_return buffered(make_http_error_response(request_value.context.request, "not_acceptable",
                                                              error.message(), status::not_acceptable, options));
               } catch (const forge::net::http::exceptions::bad_request& error) {
                  co_return buffered(make_validation_response(request_value.context.request, error.message(), options));
               } catch (const forge::exceptions::base& error) {
                  const auto payload = forge::api::core::project_error(method_descriptor, error);
                  co_return buffered(make_error_response(request_value.context.request, payload, options));
               }
            };
            switch (verb) {
            case method::get:
               target.get_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::head:
               target.head_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::post:
               target.post_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::put:
               target.put_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::patch:
               target.patch_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::delete_:
               target.del_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            default:
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::method_not_allowed,
                                     "unsupported HTTP API live stream route verb");
            }
         } else if constexpr (detail::request_needs_stream_v<Request> || tuple_needs_stream_v<argument_tuple> ||
                       detail::response_needs_stream_v<Response>) {
            auto stream_handler = [plan, options, name,
                                   canonical_method_descriptor](stream_request& request_value)
                -> boost::asio::awaitable<stream_response> {
               if (plan.local == nullptr) {
                  FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                                        "HTTP API binding has no local registry");
               }
               auto pinned = plan.pin(interface_type::ref());
               const auto& method_descriptor = canonical_method_descriptor;
               auto trusted = detail::trusted_invocation_for(request_value.context);
               auto endpoint = std::shared_ptr<endpoint_state>{};
               try {
                  const auto& installed_method_descriptor = require_route_method_descriptor(
                     pinned.describe(interface_type::ref()), name);
                  validate_route_method_descriptor<Method, Request, Response>(installed_method_descriptor, name);
                  require_response_accept_before_handler<Response>(request_value.context.request, options);
                  if constexpr (is_positional_http_method_v<Method, Request>) {
                     if (method_descriptor.argument_names.empty()) {
                        FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                              "HTTP API positional method is missing argument metadata");
                     }
                     auto arguments = co_await make_positional_arguments_from_stream<argument_tuple>(
                         request_value, options, method_descriptor);
                     auto value =
                         co_await invoke_local_arguments<Method, interface_type, Request, argument_tuple, Response>(
                             pinned, name, canonical_method_descriptor, std::move(arguments), trusted);
                     co_return co_await make_success_stream_response(request_value.context.request,
                                                                     options.success_status, std::move(value), options,
                                                                     {}, &request_value);
                  } else {
                     auto request = co_await make_request_from_stream<Request>(request_value, options);
                     endpoint = make_endpoint_state<Request>(request_value.context.request, options.success_status);
                     attach_endpoint_state(request, endpoint);
                     auto value = co_await invoke_local<Method, interface_type, Request, Response>(
                         pinned, name, canonical_method_descriptor, std::move(request), trusted);
                     co_return co_await make_success_stream_response(request_value.context.request,
                                                                     options.success_status, std::move(value), options,
                                                                     endpoint, &request_value);
                  }
               } catch (const forge::net::http::exceptions::unsupported_media_type& error) {
                  co_return buffered(make_http_error_response(request_value.context.request, "unsupported_media_type",
                                                              error.message(), status::unsupported_media_type,
                                                              options, endpoint));
               } catch (const forge::net::http::exceptions::not_acceptable& error) {
                  co_return buffered(make_http_error_response(request_value.context.request, "not_acceptable",
                                                              error.message(), status::not_acceptable, options,
                                                              endpoint));
               } catch (const forge::net::http::exceptions::bad_request& error) {
                  co_return buffered(
                      make_validation_response(request_value.context.request, error.message(), options, endpoint));
               } catch (const forge::exceptions::base& error) {
                  const auto payload = forge::api::core::project_error(method_descriptor, error);
                  co_return buffered(make_error_response(request_value.context.request, payload, options, endpoint));
               }
            };
            switch (verb) {
            case method::get:
               target.get_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::head:
               target.head_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::post:
               target.post_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::put:
               target.put_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::patch:
               target.patch_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            case method::delete_:
               target.del_stream(std::move(mounted_path), std::move(stream_handler));
               break;
            default:
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::method_not_allowed,
                                     "unsupported HTTP API stream route verb");
            }
         } else {
            auto handler = [plan, options, name,
                            canonical_method_descriptor](route_context& context) -> boost::asio::awaitable<response> {
               if (plan.local == nullptr) {
                  FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                                        "HTTP API binding has no local registry");
               }
               auto pinned = plan.pin(interface_type::ref());
               const auto& method_descriptor = canonical_method_descriptor;
               auto endpoint = std::shared_ptr<endpoint_state>{};
               try {
                  const auto& installed_method_descriptor = require_route_method_descriptor(
                     pinned.describe(interface_type::ref()), name);
                  validate_route_method_descriptor<Method, Request, Response>(installed_method_descriptor, name);
                  require_response_accept_before_handler<Response>(context.request, options);
                  auto trusted = detail::trusted_invocation_for(context);
                  if constexpr (is_positional_http_method_v<Method, Request>) {
                     if (method_descriptor.argument_names.empty()) {
                        FORGE_THROW_EXCEPTION(forge::net::http::exceptions::bad_request,
                                              "HTTP API positional method is missing argument metadata");
                     }
                     auto arguments =
                         make_positional_arguments_from_http<argument_tuple>(context, options, method_descriptor);
                     auto value =
                         co_await invoke_local_arguments<Method, interface_type, Request, argument_tuple, Response>(
                             pinned, name, canonical_method_descriptor, std::move(arguments), trusted);
                     co_return make_success_response(context.request, options.success_status, value, options);
                  } else {
                     auto request = make_request_from_http<Request>(context, options);
                     endpoint = make_endpoint_state<Request>(context.request, options.success_status);
                     attach_endpoint_state(request, endpoint);
                     auto value = co_await invoke_local<Method, interface_type, Request, Response>(
                         pinned, name, canonical_method_descriptor, std::move(request), trusted);
                     co_return make_success_response(context.request, options.success_status, value, options, endpoint);
                  }
               } catch (const forge::net::http::exceptions::unsupported_media_type& error) {
                  co_return make_http_error_response(context.request, "unsupported_media_type", error.message(),
                                                     status::unsupported_media_type, options, endpoint);
               } catch (const forge::net::http::exceptions::not_acceptable& error) {
                  co_return make_http_error_response(context.request, "not_acceptable", error.message(),
                                                     status::not_acceptable, options, endpoint);
               } catch (const forge::net::http::exceptions::bad_request& error) {
                  co_return make_validation_response(context.request, error.message(), options, endpoint);
               } catch (const forge::exceptions::base& error) {
                  const auto payload = forge::api::core::project_error(method_descriptor, error);
                  co_return make_error_response(context.request, payload, options, endpoint);
               }
            };
            switch (verb) {
            case method::get:
               target.get(std::move(mounted_path), std::move(handler));
               break;
            case method::head:
               target.head(std::move(mounted_path), std::move(handler));
               break;
            case method::post:
               target.post(std::move(mounted_path), std::move(handler));
               break;
            case method::put:
               target.put(std::move(mounted_path), std::move(handler));
               break;
            case method::patch:
               target.patch(std::move(mounted_path), std::move(handler));
               break;
            case method::delete_:
               target.del(std::move(mounted_path), std::move(handler));
               break;
            default:
               FORGE_THROW_EXCEPTION(forge::net::http::exceptions::method_not_allowed,
                                     "unsupported HTTP API route verb");
            }
         }
      };
   }

   template <typename> struct method_class;

   template <typename Class, typename Return, typename... Args> struct method_class<Return (Class::*)(Args...)> {
      using type = Class;
   };

   template <typename Class, typename Return, typename... Args> struct method_class<Return (Class::*)(Args...) const> {
      using type = Class;
   };

   router* target_ = nullptr;
   forge::api::core::binding_plan plan_;
   std::vector<mount_action> steps_;
};

[[nodiscard]] inline binding_builder binding(router& target) {
   return binding_builder{target};
}

[[nodiscard]] inline binding_builder binding() {
   return binding_builder{};
}

} // namespace forge::api::http
