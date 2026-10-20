module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.swarm.api.peer;

export import forge.api.auth.authenticated_caller;
export import forge.api.core.connection;
export import forge.api.core.duplex_stream;
export import forge.api.core.stream_writer;
export import forge.swarm.protocol.metadata;
export import forge.swarm.protocol.peer_message;

export namespace forge::swarm::api {

class peer
    : public forge::api::core::contract<peer, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~peer();

   virtual boost::asio::awaitable<void> metadata(protocol::metadata_request request,
                                                 forge::api::auth::authenticated_caller caller,
                                                 forge::api::core::stream_writer<protocol::metadata_chunk> output) = 0;

   virtual boost::asio::awaitable<void>
   exchange(protocol::exchange_request request, forge::api::auth::authenticated_caller caller,
            forge::api::core::duplex_stream<protocol::peer_message, protocol::peer_message> stream) = 0;
};

} // namespace forge::swarm::api

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::swarm::api::peer> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::swarm::api::peer, EnableRaw>& method) {
      method.template error<exceptions::server_supplied_unavailable>(
          "caller_unavailable", {.status_code = status::unauthenticated, .retryable = false});
      method.template error<exceptions::invocation_denied>(
          "admission_denied", {.status_code = status::permission_denied, .retryable = false});
      method.template error<::forge::swarm::protocol::exceptions::invalid_metadata>(
          "invalid_metadata", {.status_code = status::invalid_argument, .retryable = false});
      method.template error<::forge::swarm::protocol::exceptions::invalid_message>(
          "invalid_message", {.status_code = status::invalid_argument, .retryable = false});
      method.template error<::forge::swarm::protocol::exceptions::invalid_encoding>(
          "invalid_encoding", {.status_code = status::invalid_argument, .retryable = false});
      method.template error<::forge::swarm::protocol::exceptions::unsupported_version>(
          "unsupported_version", {.status_code = status::failed_precondition, .retryable = false});
      if constexpr (EnableRaw) {
         if constexpr (method_kind_v<Method> == method_kind::server_stream) {
            method.output_decoder([](const bytes& value, forge::raw::unpack_limits limits) {
               static_cast<void>(::forge::swarm::protocol::decode_metadata_chunk(value, limits));
            });
         } else {
            method.input_decoder([](const bytes& value, forge::raw::unpack_limits limits) {
               static_cast<void>(::forge::swarm::protocol::decode_message(value, limits));
            });
            method.output_decoder([](const bytes& value, forge::raw::unpack_limits limits) {
               static_cast<void>(::forge::swarm::protocol::decode_message(value, limits));
            });
         }
      }
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(::forge::swarm::api::peer, FORGE_API_CONTRACT("forge.swarm.peer", 1, 0),
                 FORGE_API_METHOD(metadata, request, caller), FORGE_API_METHOD(exchange, request, caller))
