module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <utility>
#include <vector>

export module forge.api.core.descriptor;

export import forge.api.core.duplex_stream;
export import forge.api.core.exceptions;
export import forge.api.core.context;
export import forge.api.core.server_supplied;
export import forge.api.core.types;
export import forge.exceptions;
export import forge.raw.datastream;
export import forge.raw.raw;

import forge.raw.exceptions;

export namespace forge::api::core {

template <typename Interface> struct api_traits;

template <typename T>
[[nodiscard]] T unpack_body(std::span<const std::uint8_t> body) {
   if constexpr (std::is_void_v<T>) {
      if (!body.empty()) {
         throw exceptions::protocol_error{"void API response contains a body"};
      }
      return;
   } else {
      const auto bounded = static_cast<std::uint32_t>(std::min<std::size_t>(
         body.size(), std::numeric_limits<std::uint32_t>::max()));
      try {
         return forge::raw::unpack_exact<T>(
            body, forge::raw::unpack_limits{
                     .max_container_elements = bounded,
                     .max_total_container_elements = bounded,
                     .max_bytes = bounded,
                     .first_container_elements = bounded,
                  });
      } catch (const forge::raw::exceptions::allocation_limit&) {
         throw exceptions::resource_exhausted{"API body exceeds decode limits"};
      } catch (const std::bad_alloc&) {
         throw exceptions::resource_exhausted{"API body allocation failed"};
      } catch (const forge::raw::exceptions::codec_error&) {
         throw exceptions::protocol_error{"API body is malformed"};
      }
   }
}

template <typename T> [[nodiscard]] T unpack_body(const bytes& body) {
   return unpack_body<T>(std::span<const std::uint8_t>{body.data(), body.size()});
}

template <typename T> [[nodiscard]] bytes pack_body(const T& value) {
   return forge::raw::pack(value);
}

template <typename T> struct method_signature;

template <typename Class, typename Response, typename... Args>
struct method_signature<boost::asio::awaitable<Response> (Class::*)(Args...)> {
   using class_type = Class;
   using argument_tuple = std::tuple<Args...>;
   using response_type = Response;
};

template <typename Class, typename Response, typename... Args>
struct method_signature<boost::asio::awaitable<Response> (Class::*)(Args...) const>
    : method_signature<boost::asio::awaitable<Response> (Class::*)(Args...)> {};

template <typename Tuple> struct method_payload;

template <> struct method_payload<std::tuple<>> {
   using type = std::tuple<>;
};

template <typename T> struct method_payload<std::tuple<T>> {
   using type = T;
};

template <typename First, typename Second, typename... Rest> struct method_payload<std::tuple<First, Second, Rest...>> {
   using type = std::tuple<First, Second, Rest...>;
};

template <auto Method> using method_argument_tuple_t = typename method_signature<decltype(Method)>::argument_tuple;

template <auto Method> using method_response_t = typename method_signature<decltype(Method)>::response_type;

template <auto Method> using method_request_t = typename method_payload<method_argument_tuple_t<Method>>::type;

template <auto Method> using method_class_t = typename method_signature<decltype(Method)>::class_type;

template <auto Method, std::size_t Index>
using method_argument_t = std::tuple_element_t<Index, method_argument_tuple_t<Method>>;

template <auto Method>
inline constexpr auto method_argument_count_v = std::tuple_size_v<method_argument_tuple_t<Method>>;

using canonical_request_encoder = std::function<bytes()>;
using contextual_dispatch_hook = std::function<boost::asio::awaitable<void>(
    frame&, request_view, const canonical_request_encoder&)>;

namespace detail {

class contextual_handler_gate_state;

} // namespace detail

class contextual_handler_gate {
 public:
   contextual_handler_gate(contextual_handler_gate&& other) noexcept;
   contextual_handler_gate& operator=(contextual_handler_gate&& other) noexcept;
   ~contextual_handler_gate();

   contextual_handler_gate(const contextual_handler_gate&) = delete;
   contextual_handler_gate& operator=(const contextual_handler_gate&) = delete;

   boost::asio::awaitable<std::shared_ptr<void>>
   operator()(request_view request, const canonical_request_encoder& encode);

 private:
   explicit contextual_handler_gate(std::weak_ptr<detail::contextual_handler_gate_state> state);

   friend class detail::contextual_handler_gate_state;

   std::weak_ptr<detail::contextual_handler_gate_state> state_;
   bool acquired_ = false;
};

namespace detail {

class contextual_handler_gate_control {
 public:
   contextual_handler_gate_control(frame request, contextual_dispatch_hook before,
                                   std::function<void(const bytes&)> request_validator,
                                   std::function<void(const bytes&, const bytes&)> response_validator,
                                   std::shared_ptr<void> implementation);
   contextual_handler_gate_control(contextual_handler_gate_control&& other) noexcept;
   contextual_handler_gate_control& operator=(contextual_handler_gate_control&& other) noexcept;
   ~contextual_handler_gate_control() noexcept;

   contextual_handler_gate_control(const contextual_handler_gate_control&) = delete;
   contextual_handler_gate_control& operator=(const contextual_handler_gate_control&) = delete;

   [[nodiscard]] contextual_handler_gate take_gate();
   void close() noexcept;
   [[nodiscard]] bool completed_successfully_once() const;
   void validate_response(const bytes& response) const;
   void transfer_completion_metadata(frame& response);

 private:
   std::shared_ptr<contextual_handler_gate_state> state_;
   bool gate_taken_ = false;
};

} // namespace detail

namespace detail {

struct missing_proxy_argument final {};

template <auto Method, std::size_t Index,
          bool Present = (Index < method_argument_count_v<Method>)>
struct method_argument_or {
   using type = missing_proxy_argument;
};

template <auto Method, std::size_t Index>
struct method_argument_or<Method, Index, true> {
   using type = method_argument_t<Method, Index>;
};

template <auto Method, std::size_t Index>
using method_argument_or_t = typename method_argument_or<Method, Index>::type;

template <auto Method>
inline constexpr bool has_stream_endpoint_v = [] {
   if constexpr (method_argument_count_v<Method> == 0) {
      return false;
   } else {
      return stream_endpoint_v<method_argument_t<Method, method_argument_count_v<Method> - 1>>;
   }
}();

template <auto Method, std::size_t... Index> consteval bool any_stream_endpoint(std::index_sequence<Index...>) {
   return (stream_endpoint_v<method_argument_t<Method, Index>> || ... || false);
}

template <auto Method>
inline constexpr bool has_any_stream_endpoint_v =
    any_stream_endpoint<Method>(std::make_index_sequence<method_argument_count_v<Method>>{});

template <auto Method>
inline constexpr std::size_t fixed_argument_count_v =
    method_argument_count_v<Method> - (has_stream_endpoint_v<Method> ? 1U : 0U);

template <auto Method, std::size_t... Index>
[[nodiscard]] auto fixed_argument_tuple(std::index_sequence<Index...>)
    -> std::tuple<std::remove_cvref_t<method_argument_t<Method, Index>>...>;

template <auto Method>
using method_fixed_argument_tuple_t =
    decltype(fixed_argument_tuple<Method>(std::make_index_sequence<fixed_argument_count_v<Method>>{}));

template <auto Method>
using method_fixed_request_t = typename method_payload<method_fixed_argument_tuple_t<Method>>::type;

struct server_field_operations {
   std::function<void(void*)> reset_wire;
   std::function<void(void*, const trusted_invocation&)> apply_wire;
   std::function<void(void*)> reset_fixed;
   std::function<void(void*, const trusted_invocation&)> apply_fixed;

   [[nodiscard]] bool empty() const noexcept {
      return !reset_wire && !apply_wire && !reset_fixed && !apply_fixed;
   }

   [[nodiscard]] bool active() const noexcept {
      return !empty();
   }
};

template <typename Wire, typename Fixed>
server_field_operations make_server_field_operations() {
   return {
      .reset_wire = [](void* value) { reset_server_supplied(*static_cast<Wire*>(value)); },
      .apply_wire = [](void* value, const trusted_invocation& trusted) {
         apply_server_supplied(*static_cast<Wire*>(value), trusted);
      },
      .reset_fixed = [](void* value) { reset_server_supplied(*static_cast<Fixed*>(value)); },
      .apply_fixed = [](void* value, const trusted_invocation& trusted) {
         apply_server_supplied(*static_cast<Fixed*>(value), trusted);
      },
   };
}

template <auto Method>
inline constexpr method_kind inferred_method_kind_v = [] {
   if constexpr (!has_stream_endpoint_v<Method>) {
      return method_kind::unary;
   } else {
      using endpoint_type = std::remove_cvref_t<method_argument_t<Method, method_argument_count_v<Method> - 1>>;
      if constexpr (stream_writer_traits<endpoint_type>::value) {
         return method_kind::server_stream;
      } else if constexpr (stream_reader_traits<endpoint_type>::value) {
         return method_kind::client_stream;
      } else {
         return method_kind::bidirectional_stream;
      }
   }
}();

template <auto Method> void validate_method_signature() {
   static_assert(!has_any_stream_endpoint_v<Method> || has_stream_endpoint_v<Method>,
                 "API stream endpoint must be the final method argument");
   if constexpr (has_stream_endpoint_v<Method>) {
      static_assert(std::same_as<method_argument_t<Method, method_argument_count_v<Method> - 1>,
                                 std::remove_cvref_t<method_argument_t<Method, method_argument_count_v<Method> - 1>>>,
                    "API stream endpoint must be passed by value");
      if constexpr (inferred_method_kind_v<Method> == method_kind::server_stream ||
                    inferred_method_kind_v<Method> == method_kind::bidirectional_stream) {
         static_assert(std::same_as<method_response_t<Method>, void>,
                       "server and bidirectional stream handlers must return awaitable<void>");
      }
   }
}

template <auto Method, typename Interface, typename Tuple, std::size_t... Index>
boost::asio::awaitable<method_response_t<Method>> invoke_fixed(Interface& implementation, Tuple arguments,
                                                               std::index_sequence<Index...>) {
   if constexpr (std::same_as<method_response_t<Method>, void>) {
      co_await std::invoke(Method, implementation, std::move(std::get<Index>(arguments))...);
      co_return;
   } else {
      co_return co_await std::invoke(Method, implementation, std::move(std::get<Index>(arguments))...);
   }
}

template <auto Method, typename Interface, typename Tuple, typename Endpoint, std::size_t... Index>
boost::asio::awaitable<method_response_t<Method>> invoke_stream(Interface& implementation, Tuple arguments,
                                                                Endpoint endpoint, std::index_sequence<Index...>) {
   if constexpr (std::same_as<method_response_t<Method>, void>) {
      co_await std::invoke(Method, implementation, std::move(std::get<Index>(arguments))..., std::move(endpoint));
      co_return;
   } else {
      co_return co_await std::invoke(Method, implementation, std::move(std::get<Index>(arguments))...,
                                     std::move(endpoint));
   }
}

[[nodiscard]] inline std::string_view trim_argument_name(std::string_view value) noexcept {
   while (!value.empty() &&
          (value.front() == ' ' || value.front() == '\t' || value.front() == '\n' || value.front() == '\r')) {
      value.remove_prefix(1);
   }
   while (!value.empty() &&
          (value.back() == ' ' || value.back() == '\t' || value.back() == '\n' || value.back() == '\r')) {
      value.remove_suffix(1);
   }
   return value;
}

[[nodiscard]] inline std::vector<std::string> argument_names_from_macro(std::string_view value) {
   value = trim_argument_name(value);
   if (value.size() >= 2U && value.front() == '(' && value.back() == ')') {
      value.remove_prefix(1);
      value.remove_suffix(1);
   }
   value = trim_argument_name(value);
   if (value.empty()) {
      return {};
   }

   auto names = std::vector<std::string>{};
   while (true) {
      const auto comma = value.find(',');
      const auto token = trim_argument_name(value.substr(0, comma));
      if (token.empty()) {
         throw exceptions::protocol_error{"API method argument name is empty"};
      }
      names.emplace_back(token);
      if (comma == std::string_view::npos) {
         break;
      }
      value.remove_prefix(comma + 1U);
   }
   return names;
}

template <auto Method, std::size_t... Index>
[[nodiscard]] method_fixed_argument_tuple_t<Method> unpack_fixed_arguments(const bytes& payload,
                                                                           std::index_sequence<Index...>) {
   constexpr auto count = sizeof...(Index);
   if constexpr (count == 0) {
      if (!payload.empty()) {
         static_cast<void>(unpack_body<std::tuple<>>(payload));
      }
      return {};
   } else if constexpr (count == 1) {
      using value_type = std::tuple_element_t<0, method_fixed_argument_tuple_t<Method>>;
      return method_fixed_argument_tuple_t<Method>{unpack_body<value_type>(payload)};
   } else {
      return unpack_body<method_fixed_argument_tuple_t<Method>>(payload);
   }
}

template <auto Method>
[[nodiscard]] method_fixed_argument_tuple_t<Method> unpack_fixed_arguments(const bytes& payload) {
   return unpack_fixed_arguments<Method>(payload, std::make_index_sequence<detail::fixed_argument_count_v<Method>>{});
}

template <auto Method>
[[nodiscard]] bytes pack_fixed_arguments(const method_fixed_argument_tuple_t<Method>& arguments) {
   constexpr auto count = fixed_argument_count_v<Method>;
   if constexpr (count == 0) {
      return pack_body(std::tuple<>{});
   } else if constexpr (count == 1) {
      return pack_body(std::get<0>(arguments));
   } else {
      return pack_body(arguments);
   }
}

template <auto Method>
[[nodiscard]] request_view fixed_request_view(const method_fixed_argument_tuple_t<Method>& arguments) {
   if constexpr (fixed_argument_count_v<Method> == 1) {
      return request_view::borrow(std::get<0>(arguments));
   } else {
      return request_view::borrow(arguments);
   }
}

template <auto Method> [[nodiscard]] bytes pack_terminal_result(const method_response_t<Method>& value) {
   return pack_body(value);
}

template <auto Method, typename Interface>
boost::asio::awaitable<bytes> invoke_raw_stream(std::shared_ptr<void> implementation, bytes fixed_payload,
                                                std::shared_ptr<stream_endpoint> input,
                                                std::shared_ptr<stream_endpoint> output) {
   auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
   auto arguments = unpack_fixed_arguments<Method>(fixed_payload);
   try {
      if constexpr (inferred_method_kind_v<Method> == method_kind::server_stream) {
         if (!output) {
            throw exceptions::protocol_error{"server stream requires an output endpoint"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto writer =
             writer_access::make<typename stream_writer_traits<std::remove_cvref_t<endpoint_type>>::item_type>(output);
         co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(writer),
                                        std::make_index_sequence<fixed_argument_count_v<Method>>{});
         output->close();
         co_return bytes{};
      } else if constexpr (inferred_method_kind_v<Method> == method_kind::client_stream) {
         if (!input) {
            throw exceptions::protocol_error{"client stream requires an input endpoint"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto reader =
             reader_access::make<typename stream_reader_traits<std::remove_cvref_t<endpoint_type>>::item_type>(input);
         if constexpr (std::same_as<method_response_t<Method>, void>) {
            co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(reader),
                                           std::make_index_sequence<fixed_argument_count_v<Method>>{});
            co_return bytes{};
         } else {
            auto result = co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(reader),
                                                         std::make_index_sequence<fixed_argument_count_v<Method>>{});
            co_return pack_terminal_result<Method>(result);
         }
      } else {
         if (!input || !output) {
            throw exceptions::protocol_error{"bidirectional stream requires input and output endpoints"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto stream = duplex_stream<typename duplex_stream_traits<endpoint_type>::input_type,
                                     typename duplex_stream_traits<endpoint_type>::output_type>{
             reader_access::make<typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::input_type>(input),
             writer_access::make<typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::output_type>(
                 output)};
         co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(stream),
                                        std::make_index_sequence<fixed_argument_count_v<Method>>{});
         output->close();
         co_return bytes{};
      }
   } catch (...) {
      auto error = std::current_exception();
      if (input) {
         input->fail(error);
      }
      if (output) {
         output->fail(error);
      }
      throw;
   }
}

template <auto Method, typename Interface>
boost::asio::awaitable<bytes>
invoke_contextual_raw(bytes payload, trusted_invocation trusted, const server_field_operations& fields,
                      contextual_handler_gate gate) {
   auto arguments = unpack_fixed_arguments<Method>(payload);
   fields.reset_fixed(&arguments);
   fields.apply_fixed(&arguments, trusted);
   const auto canonical_payload = [&arguments] { return pack_fixed_arguments<Method>(arguments); };
   auto implementation = co_await gate(fixed_request_view<Method>(arguments), canonical_payload);
   auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
   if constexpr (std::same_as<method_response_t<Method>, void>) {
      co_await invoke_fixed<Method>(*typed, std::move(arguments),
                                    std::make_index_sequence<method_argument_count_v<Method>>{});
      co_return bytes{};
   } else {
      auto response = co_await invoke_fixed<Method>(*typed, std::move(arguments),
                                                     std::make_index_sequence<method_argument_count_v<Method>>{});
      co_return pack_body(response);
   }
}

template <auto Method, typename Interface>
boost::asio::awaitable<bytes>
invoke_contextual_stream(bytes fixed_payload,
                         std::shared_ptr<stream_endpoint> input,
                         std::shared_ptr<stream_endpoint> output,
                         trusted_invocation trusted,
                         const server_field_operations& fields,
                         contextual_handler_gate gate) {
   auto arguments = unpack_fixed_arguments<Method>(fixed_payload);
   fields.reset_fixed(&arguments);
   fields.apply_fixed(&arguments, trusted);
   const auto canonical_payload = [&arguments] { return pack_fixed_arguments<Method>(arguments); };
   auto implementation = co_await gate(fixed_request_view<Method>(arguments), canonical_payload);
   auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
   try {
      if constexpr (inferred_method_kind_v<Method> == method_kind::server_stream) {
         if (!output) {
            throw exceptions::protocol_error{"server stream requires an output endpoint"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto writer =
             writer_access::make<typename stream_writer_traits<std::remove_cvref_t<endpoint_type>>::item_type>(output);
         co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(writer),
                                        std::make_index_sequence<fixed_argument_count_v<Method>>{});
         output->close();
         co_return bytes{};
      } else if constexpr (inferred_method_kind_v<Method> == method_kind::client_stream) {
         if (!input) {
            throw exceptions::protocol_error{"client stream requires an input endpoint"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto reader =
             reader_access::make<typename stream_reader_traits<std::remove_cvref_t<endpoint_type>>::item_type>(input);
         if constexpr (std::same_as<method_response_t<Method>, void>) {
            co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(reader),
                                           std::make_index_sequence<fixed_argument_count_v<Method>>{});
            co_return bytes{};
         } else {
            auto result = co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(reader),
                                                         std::make_index_sequence<fixed_argument_count_v<Method>>{});
            co_return pack_terminal_result<Method>(result);
         }
      } else {
         if (!input || !output) {
            throw exceptions::protocol_error{"bidirectional stream requires input and output endpoints"};
         }
         using endpoint_type = method_argument_t<Method, method_argument_count_v<Method> - 1>;
         auto stream = duplex_stream<typename duplex_stream_traits<endpoint_type>::input_type,
                                     typename duplex_stream_traits<endpoint_type>::output_type>{
             reader_access::make<typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::input_type>(input),
             writer_access::make<typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::output_type>(
                 output)};
         co_await invoke_stream<Method>(*typed, std::move(arguments), std::move(stream),
                                        std::make_index_sequence<fixed_argument_count_v<Method>>{});
         output->close();
         co_return bytes{};
      }
   } catch (...) {
      auto error = std::current_exception();
      if (input) {
         input->fail(error);
      }
      if (output) {
         output->fail(error);
      }
      throw;
   }
}

} // namespace detail

template <auto Method> inline constexpr method_kind method_kind_v = detail::inferred_method_kind_v<Method>;

struct error_options {
   status status_code = status::internal;
   bool retryable = false;
};

struct error_descriptor {
   std::string name;
   error_identity identity;
   status status_code = status::internal;
   bool retryable = false;
   std::type_index exception_type = typeid(void);
   std::type_index details_type = typeid(void);
   std::function<void(const error_payload&)> thrower;
};

using raw_stream_invoker = std::function<boost::asio::awaitable<bytes>(
    std::shared_ptr<void>, bytes, std::shared_ptr<detail::stream_endpoint>, std::shared_ptr<detail::stream_endpoint>)>;
using contextual_raw_invoker = std::function<boost::asio::awaitable<bytes>(
    bytes, trusted_invocation, contextual_handler_gate)>;
using contextual_raw_stream_invoker = std::function<boost::asio::awaitable<bytes>(
    bytes, std::shared_ptr<detail::stream_endpoint>, std::shared_ptr<detail::stream_endpoint>, trusted_invocation,
    contextual_handler_gate)>;

namespace detail {

struct contextual_request_encoder {
   std::function<bytes(const void*)> encode;
   std::function<bytes(void*)> encode_owned;
   server_field_operations fields;

   [[nodiscard]] bytes operator()(const void* request) const;
};

struct contextual_unary_invoker {
   std::function<boost::asio::awaitable<bytes>(std::shared_ptr<void>, bytes)> invoke;
   contextual_raw_invoker invoke_contextual;
   server_field_operations fields;

   boost::asio::awaitable<bytes> operator()(std::shared_ptr<void> implementation, bytes payload) const;
};

struct contextual_stream_invoker {
   raw_stream_invoker invoke;
   contextual_raw_stream_invoker invoke_contextual;
   server_field_operations fields;

   boost::asio::awaitable<bytes>
   operator()(std::shared_ptr<void> implementation, bytes payload,
              std::shared_ptr<stream_endpoint> input,
              std::shared_ptr<stream_endpoint> output) const;
};

} // namespace detail

struct method_descriptor {
   std::string name;
   method_kind kind = method_kind::unary;
   std::uint16_t since_revision = 0;
   bool deprecated = false;
   std::string deprecation_reason;
   std::type_index request_type = typeid(void);
   std::type_index response_type = typeid(void);
   std::type_index fixed_arguments_type = typeid(void);
   std::type_index input_type = typeid(void);
   std::type_index output_type = typeid(void);
   std::type_index result_type = typeid(void);
   std::vector<std::string> argument_names;
   std::vector<std::type_index> response_traits;
   std::vector<error_descriptor> errors;
   std::function<bytes(const void*)> request_encoder;
   std::function<bytes(const void*)> response_encoder;
   std::function<void(const bytes&, forge::raw::unpack_limits)> request_decoder;
   std::function<void(const bytes&, forge::raw::unpack_limits)> response_decoder;
   std::function<void(const bytes&, forge::raw::unpack_limits)> input_decoder;
   std::function<void(const bytes&, forge::raw::unpack_limits)> output_decoder;
   std::function<void(const bytes&)> request_validator;
   std::function<void(const bytes&, const bytes&)> response_validator;
   std::function<boost::asio::awaitable<bytes>(std::shared_ptr<void>, bytes)> raw_invoker;
   raw_stream_invoker stream_invoker;

   template <typename Trait> [[nodiscard]] bool has_response_trait() const noexcept {
      for (const auto& value : response_traits) {
         if (value == typeid(Trait)) {
            return true;
         }
      }
      return false;
   }
};

namespace detail {

void install_request_context(method_descriptor& method,
                             server_field_operations fields,
                             std::function<bytes(void*)> encode_owned);
void install_unary_context(method_descriptor& method,
                           server_field_operations fields,
                           contextual_raw_invoker invoke);
void install_stream_context(method_descriptor& method,
                            server_field_operations fields,
                            contextual_raw_stream_invoker invoke);
void remove_request_context(method_descriptor& method);
void remove_unary_context(method_descriptor& method);
void remove_stream_context(method_descriptor& method);

[[nodiscard]] const server_field_operations*
server_fields_for(const method_descriptor& method) noexcept;
[[nodiscard]] const contextual_raw_invoker*
contextual_unary_for(const method_descriptor& method) noexcept;
[[nodiscard]] const contextual_raw_stream_invoker*
contextual_stream_for(const method_descriptor& method) noexcept;
[[nodiscard]] std::optional<bytes>
encode_owned_request(const method_descriptor& method, void* request);
void reset_fixed_request(const method_descriptor& method, void* request);
void apply_wire_request(const method_descriptor& method, void* request,
                        const trusted_invocation& trusted);
void apply_fixed_request(const method_descriptor& method, void* request,
                         const trusted_invocation& trusted);

} // namespace detail

struct descriptor {
   api_id id;
   api_version version;
   surface supported_surfaces = surface::local;
   std::type_index interface_type = typeid(void);
   std::vector<method_descriptor> methods;
};

[[nodiscard]] bool compatible(const descriptor& available, const api_ref& requested) noexcept;
[[nodiscard]] bool compatible(const method_descriptor& available, const method_descriptor& requested) noexcept;
[[nodiscard]] const method_descriptor* find_method(const descriptor& api, std::string_view name) noexcept;

template <typename Exception> error_identity exception_identity() {
   static_assert(std::is_base_of_v<forge::exceptions::base, Exception>,
                 "API errors must derive from forge::exceptions::base");
   const auto code = forge::exceptions::make_error_code(Exception::value);
   return error_identity{.category = code.category().name(), .code = static_cast<std::uint32_t>(code.value())};
}

template <typename Interface, bool EnableRaw> class method_builder;

template <typename Interface> struct method_descriptor_customization {
   template <auto Method, bool EnableRaw> static void apply(method_builder<Interface, EnableRaw>&) {}
};

template <typename Interface, auto Method, bool EnableRaw>
void customize_method_descriptor(method_builder<Interface, EnableRaw>& method) {
   method_descriptor_customization<Interface>::template apply<Method>(method);
}

template <typename Interface, bool EnableRaw> class contract_builder {
 public:
   explicit contract_builder(descriptor value) : descriptor_(std::move(value)) {}

   template <auto Method> method_builder<Interface, EnableRaw> method(std::string name) {
      using wire = detail::method_fixed_request_t<Method>;
      using fixed = detail::method_fixed_argument_tuple_t<Method>;
      return add_deduced_method<Method>(std::move(name), {},
                                        detail::make_server_field_operations<wire, fixed>());
   }

   template <auto Method>
   method_builder<Interface, EnableRaw> method(std::string name, std::vector<std::string> argument_names) {
      using wire = detail::method_fixed_request_t<Method>;
      using fixed = detail::method_fixed_argument_tuple_t<Method>;
      return add_deduced_method<Method>(std::move(name), std::move(argument_names),
                                        detail::make_server_field_operations<wire, fixed>());
   }

   template <auto Method>
   method_builder<Interface, EnableRaw> method(std::string name,
                                                detail::server_field_operations fields) {
      return add_deduced_method<Method>(std::move(name), {}, std::move(fields));
   }

   template <auto Method>
   method_builder<Interface, EnableRaw> method(std::string name,
                                                std::vector<std::string> argument_names,
                                                detail::server_field_operations fields) {
      return add_deduced_method<Method>(std::move(name), std::move(argument_names),
                                        std::move(fields));
   }

   template <auto Method, typename Request, typename Response>
   method_builder<Interface, EnableRaw> method(std::string name) {
      static_assert(method_kind_v<Method> == method_kind::unary,
                    "explicit request/response API registration is unary only");
      using fixed = detail::method_fixed_argument_tuple_t<Method>;
      return add_explicit_unary_method<Method, Request, Response>(
         std::move(name), detail::make_server_field_operations<Request, fixed>());
   }

   template <auto Method, typename Request, typename Response>
   method_builder<Interface, EnableRaw> method(std::string name,
                                                detail::server_field_operations fields) {
      static_assert(method_kind_v<Method> == method_kind::unary,
                    "explicit request/response API registration is unary only");
      return add_explicit_unary_method<Method, Request, Response>(std::move(name),
                                                                  std::move(fields));
   }

   [[nodiscard]] descriptor build() {
      if (descriptor_.id.value.empty()) {
         throw exceptions::protocol_error{"API id must not be empty"};
      }
      if (descriptor_.version.major == 0) {
         throw exceptions::protocol_error{"API major version must not be zero"};
      }
      return std::move(descriptor_);
   }

   operator descriptor() {
      return build();
   }

 private:
   template <auto Method>
   method_builder<Interface, EnableRaw>
   add_deduced_method(std::string name, std::vector<std::string> argument_names,
                      detail::server_field_operations fields) {
      detail::validate_method_signature<Method>();
      constexpr auto kind = method_kind_v<Method>;
      constexpr auto argument_count = method_argument_count_v<Method>;
      constexpr auto fixed_count = detail::fixed_argument_count_v<Method>;

      if constexpr (kind == method_kind::unary) {
         if (!argument_names.empty() && argument_names.size() != argument_count) {
            throw exceptions::protocol_error{"API method argument name count does not match method signature: " + name};
         }
         if (argument_names.empty() && argument_count > 1U) {
            throw exceptions::protocol_error{"API positional method requires argument names: " + name};
         }
      } else {
         if (!argument_names.empty() && argument_names.size() != fixed_count) {
            throw exceptions::protocol_error{"API stream fixed argument name count does not match method signature: " +
                                             name};
         }
      }

      reject_duplicate(name);
      auto value = method_descriptor{
          .name = std::move(name),
          .kind = kind,
          .request_type = typeid(detail::method_fixed_request_t<Method>),
          .response_type = typeid(method_response_t<Method>),
          .fixed_arguments_type = typeid(detail::method_fixed_argument_tuple_t<Method>),
          .result_type = typeid(method_response_t<Method>),
          .argument_names = std::move(argument_names),
      };

      if constexpr (kind == method_kind::server_stream) {
         using endpoint_type = method_argument_t<Method, argument_count - 1>;
         value.output_type = typeid(typename stream_writer_traits<std::remove_cvref_t<endpoint_type>>::item_type);
      } else if constexpr (kind == method_kind::client_stream) {
         using endpoint_type = method_argument_t<Method, argument_count - 1>;
         value.input_type = typeid(typename stream_reader_traits<std::remove_cvref_t<endpoint_type>>::item_type);
      } else if constexpr (kind == method_kind::bidirectional_stream) {
         using endpoint_type = method_argument_t<Method, argument_count - 1>;
         value.input_type = typeid(typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::input_type);
         value.output_type = typeid(typename duplex_stream_traits<std::remove_cvref_t<endpoint_type>>::output_type);
      }

      if constexpr (EnableRaw) {
         using wire_request = detail::method_fixed_request_t<Method>;
         value.request_encoder = [](const void* request) {
            return pack_body(*static_cast<const wire_request*>(request));
         };
         detail::install_request_context(
            value, fields, [reset = fields.reset_wire](void* request) {
               if (reset) {
                  reset(request);
               }
               return pack_body(*static_cast<wire_request*>(request));
            });
         value.request_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
            static_cast<void>(forge::raw::unpack_exact<wire_request>(payload, limits));
         };

         if constexpr (!std::same_as<method_response_t<Method>, void>) {
            using wire_response = method_response_t<Method>;
            value.response_encoder = [](const void* response) {
               return pack_body(*static_cast<const wire_response*>(response));
            };
            value.response_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
               static_cast<void>(forge::raw::unpack_exact<wire_response>(payload, limits));
            };
         }

         if constexpr (kind == method_kind::client_stream) {
            using input_item = typename stream_reader_traits<
                std::remove_cvref_t<method_argument_t<Method, argument_count - 1>>>::item_type;
            value.input_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
               static_cast<void>(forge::raw::unpack_exact<input_item>(payload, limits));
            };
         } else if constexpr (kind == method_kind::bidirectional_stream) {
            using input_item = typename duplex_stream_traits<
                std::remove_cvref_t<method_argument_t<Method, argument_count - 1>>>::input_type;
            value.input_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
               static_cast<void>(forge::raw::unpack_exact<input_item>(payload, limits));
            };
         }

         if constexpr (kind == method_kind::server_stream) {
            using output_item = typename stream_writer_traits<
                std::remove_cvref_t<method_argument_t<Method, argument_count - 1>>>::item_type;
            value.output_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
               static_cast<void>(forge::raw::unpack_exact<output_item>(payload, limits));
            };
         } else if constexpr (kind == method_kind::bidirectional_stream) {
            using output_item = typename duplex_stream_traits<
                std::remove_cvref_t<method_argument_t<Method, argument_count - 1>>>::output_type;
            value.output_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
               static_cast<void>(forge::raw::unpack_exact<output_item>(payload, limits));
            };
         }

         if constexpr (kind == method_kind::unary) {
            value.raw_invoker = [](std::shared_ptr<void> implementation,
                                   bytes payload) -> boost::asio::awaitable<bytes> {
               auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
               auto arguments = detail::unpack_fixed_arguments<Method>(payload);
               if constexpr (std::same_as<method_response_t<Method>, void>) {
                  co_await detail::invoke_fixed<Method>(*typed, std::move(arguments),
                                                        std::make_index_sequence<argument_count>{});
                  co_return bytes{};
               } else {
                  auto response = co_await detail::invoke_fixed<Method>(*typed, std::move(arguments),
                                                                        std::make_index_sequence<argument_count>{});
                  co_return pack_body(response);
               }
            };
            detail::install_unary_context(
               value, fields,
               [fields](bytes payload, trusted_invocation trusted, contextual_handler_gate gate) {
                  return detail::invoke_contextual_raw<Method, Interface>(
                     std::move(payload), std::move(trusted), fields, std::move(gate));
               });
         } else {
            value.stream_invoker = detail::invoke_raw_stream<Method, Interface>;
            detail::install_stream_context(
               value, fields,
               [fields](bytes payload,
                        std::shared_ptr<detail::stream_endpoint> input,
                        std::shared_ptr<detail::stream_endpoint> output,
                        trusted_invocation trusted, contextual_handler_gate gate) {
                  return detail::invoke_contextual_stream<Method, Interface>(
                     std::move(payload), std::move(input), std::move(output), std::move(trusted), fields,
                     std::move(gate));
               });
         }
      }

      descriptor_.methods.push_back(std::move(value));
      return method_builder<Interface, EnableRaw>{*this, descriptor_.methods.back()};
   }

   template <auto Method, typename Request, typename Response>
   method_builder<Interface, EnableRaw>
   add_explicit_unary_method(std::string name, detail::server_field_operations fields) {
      reject_duplicate(name);
      auto value = method_descriptor{
          .name = std::move(name),
          .kind = method_kind::unary,
          .request_type = typeid(Request),
          .response_type = typeid(Response),
          .fixed_arguments_type = typeid(detail::method_fixed_argument_tuple_t<Method>),
          .result_type = typeid(Response),
      };
      if constexpr (EnableRaw) {
         value.request_encoder = [](const void* request) {
            return pack_body(*static_cast<const Request*>(request));
         };
         detail::install_request_context(
            value, fields, [reset = fields.reset_wire](void* request) {
               if (reset) {
                  reset(request);
               }
               return pack_body(*static_cast<Request*>(request));
            });
         value.response_encoder = [](const void* response) {
            return pack_body(*static_cast<const Response*>(response));
         };
         value.request_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
            static_cast<void>(forge::raw::unpack_exact<Request>(payload, limits));
         };
         value.response_decoder = [](const bytes& payload, forge::raw::unpack_limits limits) {
            static_cast<void>(forge::raw::unpack_exact<Response>(payload, limits));
         };
         value.raw_invoker = [](std::shared_ptr<void> implementation, bytes payload) -> boost::asio::awaitable<bytes> {
            auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
            auto request = unpack_body<Request>(payload);
            auto response = co_await std::invoke(Method, *typed, std::move(request));
            co_return pack_body(response);
         };
         detail::install_unary_context(
            value, fields,
            [fields](bytes payload, trusted_invocation trusted, contextual_handler_gate gate)
               -> boost::asio::awaitable<bytes> {
               auto request = unpack_body<Request>(payload);
               fields.reset_wire(&request);
               fields.apply_wire(&request, trusted);
               const auto canonical_payload = [&request] { return pack_body(request); };
               auto implementation = co_await gate(request_view::borrow(request), canonical_payload);
               auto typed = std::static_pointer_cast<Interface>(std::move(implementation));
               auto response = co_await std::invoke(Method, *typed, std::move(request));
               co_return pack_body(response);
            });
      }
      descriptor_.methods.push_back(std::move(value));
      return method_builder<Interface, EnableRaw>{*this, descriptor_.methods.back()};
   }

   void reject_duplicate(const std::string& name) const {
      for (const auto& existing : descriptor_.methods) {
         if (existing.name == name) {
            throw exceptions::protocol_error{"duplicate API method: " + name};
         }
      }
   }

   descriptor descriptor_;

   friend class method_builder<Interface, EnableRaw>;
};

template <typename Interface, bool EnableRaw> class method_builder {
 public:
   method_builder(contract_builder<Interface, EnableRaw>& owner, method_descriptor& method)
       : owner_(&owner), method_(&method) {}

   template <typename Exception, typename Details = void>
   method_builder& error(std::string name, error_options options = {}) {
      method_->errors.push_back(error_descriptor{
          .name = std::move(name),
          .identity = exception_identity<Exception>(),
          .status_code = options.status_code,
          .retryable = options.retryable,
          .exception_type = typeid(Exception),
          .details_type = typeid(Details),
          .thrower = [](const error_payload& payload) -> void {
             throw Exception{payload.message, forge::exceptions::make_fields(
                                                  forge::exceptions::ctx("remote.category", payload.identity.category),
                                                  forge::exceptions::ctx("remote.code", payload.identity.code))};
          },
      });
      return *this;
   }

   method_builder& since_revision(std::uint16_t value) noexcept {
      method_->since_revision = value;
      return *this;
   }

   method_builder& deprecated(std::string reason) {
      method_->deprecated = true;
      method_->deprecation_reason = std::move(reason);
      return *this;
   }

   template <typename Trait> method_builder& response_trait() {
      if (!method_->template has_response_trait<Trait>()) {
         method_->response_traits.emplace_back(typeid(Trait));
      }
      return *this;
   }

   method_builder& input_decoder(std::function<void(const bytes&, forge::raw::unpack_limits)> decoder) {
      if (!EnableRaw || method_->input_type == typeid(void) || !decoder) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error,
                               "input decoder requires a remote streaming input and a callable");
      }
      method_->input_decoder = std::move(decoder);
      return *this;
   }

   method_builder& output_decoder(std::function<void(const bytes&, forge::raw::unpack_limits)> decoder) {
      if (!EnableRaw || method_->output_type == typeid(void) || !decoder) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error,
                               "output decoder requires a remote streaming output and a callable");
      }
      method_->output_decoder = std::move(decoder);
      return *this;
   }

   template <auto Method, typename Request, typename Response> method_builder method(std::string name) {
      return owner_->template method<Method, Request, Response>(std::move(name));
   }

   [[nodiscard]] descriptor build() {
      return owner_->build();
   }

   operator descriptor() {
      return build();
   }

 private:
   contract_builder<Interface, EnableRaw>* owner_ = nullptr;
   method_descriptor* method_ = nullptr;
};

template <typename Interface, bool EnableRaw = supports(Interface::api_surface, surface::remote)>
contract_builder<Interface, EnableRaw> define(descriptor value) {
   value.interface_type = typeid(Interface);
   value.supported_surfaces = Interface::api_surface;
   return contract_builder<Interface, EnableRaw>{std::move(value)};
}

} // namespace forge::api::core
