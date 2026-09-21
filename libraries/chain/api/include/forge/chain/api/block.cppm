module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>
#include <forge/exceptions/macros.hpp>
#include <type_traits>

export module forge.chain.api.block;

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.dispatcher;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.client_request;
import forge.api.http.mapping;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.chain.api.json_schema;
import forge.crypto.asymmetric;
import forge.crypto.digest.sha256;
import forge.net.http.types;
import forge.variant.variant_dynamic_bitset;

export import forge.chain.api.exceptions;
export import forge.chain.protocol.block_query;

export namespace forge::chain::api {

class block
    : public forge::api::core::contract<block, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~block() = default;

   virtual boost::asio::awaitable<protocol::block_response> get_block(protocol::block_request value) = 0;
   virtual boost::asio::awaitable<protocol::block_header_response> get_header(protocol::block_request value) = 0;
   virtual boost::asio::awaitable<protocol::block_state_response> get_block_state(protocol::block_request value) = 0;
   virtual boost::asio::awaitable<protocol::block_range_response>
   get_canonical_range(protocol::block_range_request value) = 0;
   virtual boost::asio::awaitable<protocol::protocol_features_response>
   get_activated_protocol_features(protocol::protocol_features_request value) = 0;
   virtual boost::asio::awaitable<protocol::consensus_parameters_response>
   get_consensus_parameters(protocol::anchored_request value) = 0;
   virtual boost::asio::awaitable<protocol::producers_response> get_producers(protocol::producers_request value) = 0;
   virtual boost::asio::awaitable<protocol::producer_rewards_response>
   get_producer_rewards(protocol::producer_rewards_request) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "producer rewards are not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::producer_schedule_response>
   get_producer_schedule(protocol::anchored_request value) = 0;
   virtual boost::asio::awaitable<protocol::finalizer_info_response>
   get_finalizer_info(protocol::anchored_request value) = 0;
};

} // namespace forge::chain::api

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::chain::api::block> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::chain::api::block, EnableRaw>& method) {
      ::forge::chain::api::exceptions::descriptor::declare_historical_query<Method>(method);
      if constexpr (std::is_same_v<decltype(Method), decltype(&::forge::chain::api::block::get_block)>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      } else if constexpr (std::is_same_v<decltype(Method), decltype(&::forge::chain::api::block::get_header)>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      } else if constexpr (std::is_same_v<decltype(Method), decltype(&::forge::chain::api::block::get_block_state)>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      } else if constexpr (std::is_same_v<decltype(Method), decltype(&::forge::chain::api::block::get_producers)>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      } else if constexpr (std::is_same_v<decltype(Method),
                                          decltype(&::forge::chain::api::block::get_producer_rewards)>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      }
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(::forge::chain::api::block, FORGE_API_CONTRACT("forge.chain.api.block", 2, 2),
                 FORGE_API_METHOD_TYPED(get_block, ::forge::chain::protocol::block_request,
                                        ::forge::chain::protocol::block_response),
                 FORGE_API_METHOD_TYPED(get_header, ::forge::chain::protocol::block_request,
                                        ::forge::chain::protocol::block_header_response),
                 FORGE_API_METHOD_TYPED(get_block_state, ::forge::chain::protocol::block_request,
                                        ::forge::chain::protocol::block_state_response),
                 FORGE_API_METHOD_TYPED(get_canonical_range, ::forge::chain::protocol::block_range_request,
                                        ::forge::chain::protocol::block_range_response),
                 FORGE_API_METHOD_TYPED(get_activated_protocol_features,
                                        ::forge::chain::protocol::protocol_features_request,
                                        ::forge::chain::protocol::protocol_features_response),
                 FORGE_API_METHOD_TYPED(get_consensus_parameters, ::forge::chain::protocol::anchored_request,
                                        ::forge::chain::protocol::consensus_parameters_response),
                 FORGE_API_METHOD_TYPED(get_producers, ::forge::chain::protocol::producers_request,
                                        ::forge::chain::protocol::producers_response),
                 FORGE_API_METHOD_TYPED_SINCE(get_producer_rewards,
                                               ::forge::chain::protocol::producer_rewards_request,
                                               ::forge::chain::protocol::producer_rewards_response, 2),
                 FORGE_API_METHOD_TYPED(get_producer_schedule, ::forge::chain::protocol::anchored_request,
                                        ::forge::chain::protocol::producer_schedule_response),
                 FORGE_API_METHOD_TYPED(get_finalizer_info, ::forge::chain::protocol::anchored_request,
                                        ::forge::chain::protocol::finalizer_info_response))

FORGE_HTTP_API(
    ::forge::chain::api::block,
    FORGE_HTTP_GET(get_block, "/v1/chain/blocks?id={id}&num={num}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_header, "/v1/chain/blocks/header?id={id}&num={num}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_block_state,
                   "/v1/chain/blocks/state?id={id}&num={num}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_canonical_range,
                   "/v1/chain/blocks/canonical-range?first={first}&limit={limit}&finality_from={finality_from}"
                   "&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_activated_protocol_features,
                   "/v1/chain/blocks/activated-protocol-features?lower_bound={lower_bound}&upper_bound={upper_bound}"
                   "&limit={limit}&search_by_block_num={search_by_block_num}&reverse={reverse}&anchor={anchor}"
                   "&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_consensus_parameters,
                   "/v1/chain/blocks/consensus-parameters?anchor={anchor}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_producers,
                   "/v1/chain/blocks/producers?lower_bound={lower_bound}&limit={limit}&cursor={cursor}&anchor={anchor}"
                   "&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_producer_rewards,
                   "/v1/chain/blocks/producer-rewards?producer={producer}&anchor={anchor}&finality_from={finality_from}"
                   "&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_producer_schedule,
                   "/v1/chain/blocks/producer-schedule?anchor={anchor}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_finalizer_info,
                   "/v1/chain/blocks/finalizers?anchor={anchor}&finality_from={finality_from}&audit={audit}",
                   FORGE_HTTP_CACHE(no_store)))
