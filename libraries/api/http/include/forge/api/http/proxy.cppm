module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <exception>
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

export module forge.api.http.proxy;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.types;
export import forge.api.core.handle;
import forge.api.http.binding;
import forge.api.http.client_request;
export import forge.api.http.client_response;
export import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.types;
export import forge.api.http.mapping;

export namespace forge::api::http {

using namespace forge::net::http;

template <typename Interface> class proxy;

namespace detail {

[[noreturn]] inline void rethrow_invocation_failure(std::exception_ptr failure, std::string_view method) {
   try {
      std::rethrow_exception(std::move(failure));
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const boost::system::system_error& error) {
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "HTTP API request was canceled",
                               forge::exceptions::ctx("method", std::string{method}));
      }
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::remote_internal, "HTTP API invocation failed",
                            forge::exceptions::ctx("method", std::string{method}),
                            forge::exceptions::ctx("cause", error.what()));
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::remote_internal, "HTTP API invocation failed",
                            forge::exceptions::ctx("method", std::string{method}),
                            forge::exceptions::ctx("cause", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::remote_internal, "HTTP API invocation failed",
                            forge::exceptions::ctx("method", std::string{method}));
   }
}

struct route_call {
   std::string method;
   std::function<boost::asio::awaitable<forge::api::core::response>(client&, const forge::api::core::descriptor&,
                                                                    forge::api::core::request)>
       handler;
   std::function<boost::asio::awaitable<void>(client&, const forge::api::core::descriptor&, forge::api::core::request,
                                              std::type_index, void*, std::type_index, void*)>
       typed_handler;
   std::function<boost::asio::awaitable<forge::api::core::response>(
      client&, const forge::api::core::descriptor&, forge::api::core::request,
      forge::api::core::method_kind,
      std::shared_ptr<forge::api::core::detail::stream_endpoint>,
      std::shared_ptr<forge::api::core::detail::stream_endpoint>)>
      stream_handler;
};

class route_invoker final : public forge::api::core::remote_invoker {
 public:
   route_invoker(client& target, forge::api::core::descriptor descriptor, std::vector<route_call> routes)
       : target_{&target}, descriptor_{std::move(descriptor)}, routes_{std::move(routes)} {}

   boost::asio::awaitable<forge::api::core::response> async_call(forge::api::core::request value) override {
      const auto method = value.method;
      try {
         const auto route = std::find_if(routes_.begin(), routes_.end(),
                                         [&](const route_call& candidate) { return candidate.method == method; });
         if (route == routes_.end()) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found, "HTTP API route is not declared",
                                  forge::exceptions::ctx("method", method));
         }
         co_return co_await route->handler(*target_, descriptor_, std::move(value));
      } catch (...) {
         rethrow_invocation_failure(std::current_exception(), method);
      }
   }

   boost::asio::awaitable<forge::api::core::response>
   async_stream_call(forge::api::core::request value, forge::api::core::method_kind kind,
                     std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                     std::shared_ptr<forge::api::core::detail::stream_endpoint> output) override {
      const auto method = value.method;
      try {
         const auto route = std::find_if(routes_.begin(), routes_.end(),
                                         [&](const route_call& candidate) { return candidate.method == method; });
         if (route == routes_.end()) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found, "HTTP API route is not declared",
                                  forge::exceptions::ctx("method", method));
         }
         co_return co_await route->stream_handler(*target_, descriptor_, std::move(value), kind,
                                                  std::move(input), std::move(output));
      } catch (...) {
         rethrow_invocation_failure(std::current_exception(), method);
      }
   }

   bool supports_typed_arguments() const noexcept override {
      return true;
   }

   boost::asio::awaitable<void> async_call_arguments(forge::api::core::request value,
                                                     std::type_index argument_tuple_type, void* argument_tuple,
                                                     std::type_index response_type, void* response_storage) override {
      const auto method = value.method;
      try {
         const auto route = std::find_if(routes_.begin(), routes_.end(),
                                         [&](const route_call& candidate) { return candidate.method == method; });
         if (route == routes_.end()) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found, "HTTP API route is not declared",
                                  forge::exceptions::ctx("method", method));
         }
         co_await route->typed_handler(*target_, descriptor_, std::move(value), argument_tuple_type, argument_tuple,
                                       response_type, response_storage);
      } catch (...) {
         rethrow_invocation_failure(std::current_exception(), method);
      }
   }

 private:
   client* target_;
   forge::api::core::descriptor descriptor_;
   std::vector<route_call> routes_;
};

[[nodiscard]] inline std::vector<std::string> argument_names_for(const forge::api::core::descriptor& descriptor,
                                                                 std::string_view method) {
   const auto* value = forge::api::core::find_method(descriptor, method);
   if (value == nullptr) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found, "HTTP API route method is not declared",
                            forge::exceptions::ctx("method", std::string{method}));
   }
   return value->argument_names;
}

template <auto Method, typename Request, typename Response> route_call make_route_call(route route) {
   validate_server_supplied_route<Method, Request>(route);
   validate_live_stream_route<Method, Request>(route);
   return route_call{
       .method = route.method_name,
       .handler = [route](client& target, const forge::api::core::descriptor& descriptor,
                          forge::api::core::request value) -> boost::asio::awaitable<forge::api::core::response> {
          if constexpr (forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::unary) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                   "HTTP API stream method cannot use unary invocation");
          } else {
             auto output = forge::api::core::response{
                .api = value.api,
                .method = value.method,
                .codec = value.codec,
             };
             if constexpr (!is_positional_http_method_v<Method, Request>) {
                if constexpr (detail::request_has_http_parameter_v<Request> || detail::request_needs_stream_v<Request> ||
                              detail::response_needs_stream_v<Response> || detail::is_bytes_response_v<Response> ||
                              detail::is_empty_response_v<Response>) {
                   FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                         "HTTP parameter methods require typed HTTP invocation");
                } else {
                   auto request_value = forge::api::core::unpack_body<Request>(value.body);
                   auto response_value =
                       co_await call<Request, Response>(target, descriptor, route, std::move(request_value));
                   output.body = forge::api::core::pack_body(response_value);
                }
             } else {
                using argument_tuple = forge::api::core::method_argument_tuple_t<Method>;
                auto arguments = forge::api::core::unpack_body<argument_tuple>(value.body);
                auto response_value = co_await call_arguments<argument_tuple, Response>(
                   target, descriptor, route, std::move(arguments), argument_names_for(descriptor, route.method_name));
                output.body = forge::api::core::pack_body(response_value);
             }
             co_return output;
          }
       },
       .typed_handler = [route](client& target, const forge::api::core::descriptor& descriptor,
                                                   forge::api::core::request value, std::type_index argument_tuple_type,
                                                   void* argument_tuple, std::type_index response_type,
                                                   void* response_storage) -> boost::asio::awaitable<void> {
          if constexpr (forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::unary) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                   "HTTP API stream method cannot use typed unary invocation");
          } else {
             using argument_tuple_t = forge::api::core::method_argument_tuple_t<Method>;
             if (argument_tuple_type != typeid(argument_tuple_t) || response_type != typeid(Response)) {
                FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                      "HTTP API typed argument invocation has incompatible storage");
             }
             auto& arguments = *static_cast<argument_tuple_t*>(argument_tuple);
             auto& output = *static_cast<std::optional<Response>*>(response_storage);
             if constexpr (is_positional_http_method_v<Method, Request>) {
                output.emplace(co_await call_arguments<argument_tuple_t, Response>(
                   target, descriptor, route, std::move(arguments), argument_names_for(descriptor, value.method)));
             } else {
                output.emplace(
                   co_await call<Request, Response>(target, descriptor, route, std::move(std::get<0>(arguments))));
             }
          }
       },
       .stream_handler = [route](
                            client& target, const forge::api::core::descriptor& descriptor,
                            forge::api::core::request value, forge::api::core::method_kind kind,
                            std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                            std::shared_ptr<forge::api::core::detail::stream_endpoint> output)
                            -> boost::asio::awaitable<forge::api::core::response> {
          if constexpr (forge::api::core::method_kind_v<Method> == forge::api::core::method_kind::unary) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                   "HTTP API unary method cannot use stream invocation");
          } else if constexpr (forge::api::core::method_kind_v<Method> ==
                               forge::api::core::method_kind::bidirectional_stream) {
             FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                                   "HTTP/1.1 does not support bidirectional API streams");
          } else {
             if (kind != forge::api::core::method_kind_v<Method>) {
                FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                      "HTTP API stream method kind does not match the route descriptor");
             }
             auto fixed = forge::api::core::detail::unpack_fixed_arguments<Method>(value.body);
             auto request_options = forge::net::http::request_options{};
             auto http_request = forge::net::http::request{};
             if constexpr (is_positional_http_method_v<Method, Request>) {
                auto request_parts = make_client_request(
                   target, route, fixed, argument_names_for(descriptor, route.method_name));
                if constexpr (forge::api::core::method_kind_v<Method> ==
                              forge::api::core::method_kind::server_stream) {
                   if (bind_positional_request_body(request_parts.value, route, fixed,
                                                    request_parts.consumed).has_value()) {
                      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                            "HTTP API server stream cannot combine two streamed bodies");
                   }
                }
                http_request = std::move(request_parts.value);
             } else {
                auto& request = std::get<0>(fixed);
                request_options = request_options_for(route, request);
                http_request = make_client_request(target, route, request);
                if constexpr (forge::api::core::method_kind_v<Method> ==
                              forge::api::core::method_kind::server_stream) {
                   if (bind_dto_request_body(http_request, route, request).has_value()) {
                      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                            "HTTP API server stream cannot combine two streamed bodies");
                   }
                }
             }
             co_return co_await invoke_live_http_stream(
                target, descriptor, route, std::move(value), std::move(http_request), request_options,
                kind, std::move(input), std::move(output));
          }
       },
   };
}

inline std::shared_ptr<forge::api::core::remote_invoker>
make_route_invoker(client& target, forge::api::core::descriptor descriptor, std::vector<route_call> routes) {
   return std::make_shared<route_invoker>(target, std::move(descriptor), std::move(routes));
}

template <auto Method, typename Request, typename Response>
inline constexpr auto route_can_use_api_proxy_v =
    forge::api::core::method_kind_v<Method> != forge::api::core::method_kind::unary ||
    is_positional_http_method_v<Method, Request> ||
    (!detail::request_has_http_parameter_v<Request> && !detail::request_needs_stream_v<Request> &&
     !detail::response_needs_stream_v<Response> && !detail::is_bytes_response_v<Response> &&
     !detail::is_empty_response_v<Response>);

} // namespace detail

template <typename Interface> boost::asio::awaitable<forge::api::core::handle<Interface>> remote(client& value) {
   if constexpr (traits<Interface>::use_api_proxy) {
      co_return forge::api::core::handle<Interface>{std::make_shared<forge::api::core::proxy<Interface>>(
          traits<Interface>::make_invoker(value), Interface::ref())};
   } else {
      co_return forge::api::core::handle<Interface>{std::make_shared<proxy<Interface>>(value)};
   }
}

} // namespace forge::api::http
