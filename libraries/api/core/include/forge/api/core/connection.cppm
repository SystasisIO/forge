module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <concepts>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <typeindex>
#include <tuple>
#include <utility>

export module forge.api.core.connection;

export import forge.api.core.descriptor;
export import forge.api.core.error_projection;
export import forge.api.core.handle;
export import forge.api.core.server_supplied;

export namespace forge::api::core {

template <typename Interface> class proxy;

template <typename Interface, surface Surface = surface::local> class contract {
 public:
   using interface = Interface;
   static constexpr auto api_surface = Surface;

   [[nodiscard]] static descriptor describe() {
      return api_traits<Interface>::describe();
   }

   [[nodiscard]] static api_ref ref(std::uint16_t min_revision = api_traits<Interface>::version().revision) {
      const auto version = api_traits<Interface>::version();
      return api_ref{.id = api_traits<Interface>::id(), .major = version.major, .min_revision = min_revision};
   }
};

template <typename T>
concept interface = requires {
   typename T::interface;
   { T::api_surface } -> std::convertible_to<surface>;
} && std::derived_from<T, contract<typename T::interface, T::api_surface>>;

template <typename T, surface Surface>
concept supports_surface = interface<T> && supports(T::api_surface, Surface);

template <typename T>
concept local_interface = supports_surface<T, surface::local>;

template <typename T>
concept remote_interface = supports_surface<T, surface::remote>;

namespace detail {

template <typename Request>
[[nodiscard]] bytes encode_owned_request(const method_descriptor& descriptor, Request& request) {
   if (auto encoded = detail::encode_owned_request(descriptor, &request)) {
      return std::move(*encoded);
   }
   if (descriptor.request_encoder) {
      return descriptor.request_encoder(&request);
   }
   return pack_body(request);
}

} // namespace detail

class remote_invoker {
 public:
   virtual ~remote_invoker() = default;

   virtual boost::asio::awaitable<response> async_call(request value) = 0;

   virtual boost::asio::awaitable<response> async_stream_call(request value, method_kind kind,
                                                              std::shared_ptr<detail::stream_endpoint> input,
                                                              std::shared_ptr<detail::stream_endpoint> output) {
      static_cast<void>(value);
      static_cast<void>(kind);
      static_cast<void>(input);
      static_cast<void>(output);
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "remote invoker does not support incremental stream calls");
   }

   virtual bool supports_typed_arguments() const noexcept {
      return false;
   }

   // A void response has response_type == typeid(void) and no response storage.
   virtual boost::asio::awaitable<void> async_call_arguments(request value, std::type_index argument_tuple_type,
                                                             void* argument_tuple, std::type_index response_type,
                                                             void* response_storage) {
      static_cast<void>(value);
      static_cast<void>(argument_tuple_type);
      static_cast<void>(argument_tuple);
      static_cast<void>(response_type);
      static_cast<void>(response_storage);
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "remote invoker does not support typed argument calls");
   }

   template <typename Request, typename Response>
   boost::asio::awaitable<Response> call(const descriptor& contract, api_ref api, std::string method, Request value) {
      const auto* method_value = find_method(contract, method);
      if (method_value == nullptr) {
         throw exceptions::protocol_error{"API method is not available"};
      }
      if (method_value->request_type != typeid(void) &&
          method_value->request_type != typeid(std::remove_cvref_t<Request>)) {
         throw exceptions::protocol_error{"API request type does not match its method descriptor"};
      }
      auto outbound = request{
          .api = std::move(api),
          .method = std::move(method),
          .codec = {.value = "forge.raw"},
          .body = detail::encode_owned_request(*method_value, value),
      };
      auto inbound = co_await async_call(std::move(outbound));
      if (inbound.error) {
         raise_remote_error(*inbound.error, find_method(contract, inbound.method));
      }
      co_return unpack_body<Response>(inbound.body);
   }

   template <typename Response, typename... Args>
   boost::asio::awaitable<Response> call_arguments(const descriptor& contract, api_ref api, std::string method,
                                                   Args&&... args) {
      using argument_tuple = std::tuple<std::remove_cvref_t<Args>...>;
      using wire_request = typename method_payload<argument_tuple>::type;
      const auto* method_value = find_method(contract, method);
      if (method_value == nullptr) {
         throw exceptions::protocol_error{"API method is not available"};
      }
      const auto request_matches =
         method_value->request_type == typeid(void) || method_value->request_type == typeid(wire_request);
      const auto fixed_matches = method_value->fixed_arguments_type == typeid(void) ||
                                 method_value->fixed_arguments_type == typeid(argument_tuple);
      if (!request_matches || !fixed_matches) {
         throw exceptions::protocol_error{"API argument tuple does not match its method descriptor"};
      }
      auto arguments = argument_tuple{std::forward<Args>(args)...};
      if (supports_typed_arguments()) {
         detail::reset_fixed_request(*method_value, &arguments);
         auto outbound = request{
             .api = std::move(api),
             .method = std::move(method),
             .codec = {.value = "forge.typed"},
         };
         if constexpr (std::is_void_v<Response>) {
            co_await async_call_arguments(std::move(outbound), typeid(argument_tuple), &arguments, typeid(void),
                                          nullptr);
            co_return;
         } else {
            auto output = std::optional<Response>{};
            co_await async_call_arguments(std::move(outbound), typeid(argument_tuple), &arguments, typeid(Response),
                                          &output);
            if (!output.has_value()) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                                     "typed remote invoker returned no response value");
            }
            co_return std::move(*output);
         }
      }

      auto outbound = request{
          .api = std::move(api),
          .method = std::move(method),
          .codec = {.value = "forge.raw"},
      };
      if constexpr (sizeof...(Args) == 0U) {
         outbound.body = {};
      } else if constexpr (sizeof...(Args) == 1U) {
         outbound.body = detail::encode_owned_request(*method_value, std::get<0>(arguments));
      } else {
         outbound.body = detail::encode_owned_request(*method_value, arguments);
      }
      auto inbound = co_await async_call(std::move(outbound));
      if (inbound.error) {
         raise_remote_error(*inbound.error, find_method(contract, inbound.method));
      }
      co_return unpack_body<Response>(inbound.body);
   }
};

namespace detail {

template <auto Method, typename Tuple, std::size_t... Index>
[[nodiscard]] bytes encode_fixed_proxy_arguments(const method_descriptor& descriptor,
                                                  Tuple& arguments,
                                                  std::index_sequence<Index...>) {
   const auto request_matches = descriptor.request_type == typeid(void) ||
                                descriptor.request_type == typeid(method_fixed_request_t<Method>);
   const auto fixed_matches =
      descriptor.fixed_arguments_type == typeid(void) ||
      descriptor.fixed_arguments_type == typeid(method_fixed_argument_tuple_t<Method>);
   if (!request_matches || !fixed_matches) {
      throw exceptions::protocol_error{"API stream arguments do not match its method descriptor"};
   }

   constexpr auto count = sizeof...(Index);
   if constexpr (count == 0) {
      return {};
   } else if constexpr (count == 1) {
      return encode_owned_request(descriptor, std::get<0>(arguments));
   } else {
      auto fixed = std::tuple{std::move(std::get<Index>(arguments))...};
      return encode_owned_request(descriptor, fixed);
   }
}

template <typename Interface, typename Request, typename Response>
boost::asio::awaitable<Response> proxy_call(std::shared_ptr<remote_invoker> invoker, api_ref selected_api,
                                            std::string method, Request request) {
   if constexpr (remote_interface<Interface>) {
      co_return co_await invoker->template call<Request, Response>(
          api_traits<Interface>::describe(), std::move(selected_api), std::move(method), std::move(request));
   } else {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "local-only API does not support remote invocation");
   }
}

template <typename Interface, typename Response, typename... Args>
boost::asio::awaitable<Response> proxy_call_arguments(std::shared_ptr<remote_invoker> invoker, api_ref selected_api,
                                                      std::string method, Args&&... args) {
   if constexpr (remote_interface<Interface>) {
      co_return co_await invoker->template call_arguments<Response>(
         api_traits<Interface>::describe(), std::move(selected_api), std::move(method),
         std::forward<Args>(args)...);
   } else {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "local-only API does not support remote invocation");
   }
}

template <typename Interface, auto Method, typename... Args>
boost::asio::awaitable<method_response_t<Method>>
proxy_method(std::shared_ptr<remote_invoker> invoker, api_ref selected_api, std::string method, Args&&... args) {
   validate_method_signature<Method>();
   static_assert(sizeof...(Args) == method_argument_count_v<Method>,
                 "generated API proxy argument count does not match method signature");
   if constexpr (!remote_interface<Interface>) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "local-only API does not support remote invocation");
   } else if constexpr (method_kind_v<Method> == method_kind::unary) {
      co_return co_await proxy_call_arguments<Interface, method_response_t<Method>>(
          std::move(invoker), std::move(selected_api), std::move(method), std::forward<Args>(args)...);
   } else {
      auto arguments = std::tuple<std::remove_cvref_t<Args>...>{std::forward<Args>(args)...};
      auto contract = api_traits<Interface>::describe();
      const auto* method_value = find_method(contract, method);
      if (method_value == nullptr) {
         throw exceptions::protocol_error{"API method is not available"};
      }
      auto outbound = request{
          .api = std::move(selected_api),
          .method = std::move(method),
          .codec = {.value = "forge.raw"},
          .body = encode_fixed_proxy_arguments<Method>(
             *method_value, arguments,
             std::make_index_sequence<fixed_argument_count_v<Method>>{}),
      };
      auto& endpoint = std::get<method_argument_count_v<Method> - 1>(arguments);
      auto input = std::shared_ptr<stream_endpoint>{};
      auto output = std::shared_ptr<stream_endpoint>{};
      if constexpr (method_kind_v<Method> == method_kind::server_stream) {
         output = writer_access::take_endpoint(endpoint);
      } else if constexpr (method_kind_v<Method> == method_kind::client_stream) {
         input = reader_access::endpoint(endpoint);
      } else {
         input = reader_access::endpoint(endpoint.input());
         output = writer_access::take_endpoint(endpoint.output());
      }
      auto input_owner = input;
      auto output_owner = output;
      try {
         auto inbound = co_await invoker->async_stream_call(
            std::move(outbound), method_kind_v<Method>, std::move(input),
            std::move(output));
         if (inbound.error) {
            raise_remote_error(*inbound.error,
                               find_method(contract, inbound.method));
         }
         if constexpr (std::same_as<method_response_t<Method>, void>) {
            co_return;
         } else {
            co_return unpack_body<method_response_t<Method>>(inbound.body);
         }
      } catch (...) {
         auto error = std::current_exception();
         if (input_owner) {
            input_owner->fail(error);
         }
         if (output_owner) {
            output_owner->fail(error);
         }
         throw;
      }
   }
}

template <typename Interface, bool Remote> class proxy_impl;

template <typename Interface> class proxy_impl<Interface, false> {
 public:
   explicit proxy_impl(std::shared_ptr<remote_invoker>) {}
   explicit proxy_impl(std::shared_ptr<remote_invoker> invoker, api_ref) : proxy_impl(std::move(invoker)) {}
};

} // namespace detail

class service_mount {
 public:
   virtual ~service_mount() = default;

   template <typename Interface> void register_api(std::shared_ptr<Interface> implementation) {
      static_assert(local_interface<Interface>, "Interface must opt in to forge::api::core::surface::local");
      register_api(Interface::describe(), std::move(implementation), typeid(Interface));
   }

 protected:
   virtual void register_api(descriptor value, std::shared_ptr<void> implementation, std::type_index type) = 0;
};

class remote_mount {
 public:
   virtual ~remote_mount() = default;

   boost::asio::awaitable<std::shared_ptr<remote_invoker>> get_remote_invoker(api_ref requested,
                                                                              descriptor remote_descriptor) {
      return open_remote_invoker(std::move(requested), std::move(remote_descriptor));
   }

   template <typename Interface>
   boost::asio::awaitable<handle<Interface>> get_remote_api(api_ref requested = Interface::ref()) {
      static_assert(remote_interface<Interface>, "Interface must opt in to forge::api::core::surface::remote");
      auto remote_descriptor = Interface::describe();
      auto invoker = co_await get_remote_invoker(requested, remote_descriptor);
      co_return handle<Interface>{std::make_shared<proxy<Interface>>(std::move(invoker), std::move(requested))};
   }

 protected:
   virtual boost::asio::awaitable<std::shared_ptr<remote_invoker>>
   open_remote_invoker(api_ref requested, descriptor remote_descriptor) = 0;
};

class connection : public remote_mount {
 public:
   ~connection() override = default;

   virtual void cancel() noexcept = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;
};

} // namespace forge::api::core
