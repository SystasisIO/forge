module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <string>
#include <type_traits>
#include <vector>

export module forge.chain.api.admin;

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
export import forge.chain.protocol.admin;

export namespace forge::chain::api {

class admin
    : public forge::api::core::contract<admin, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~admin() = default;

   virtual boost::asio::awaitable<protocol::push_block_response> push_block(protocol::signed_block value) = 0;
   virtual boost::asio::awaitable<protocol::snapshot_response> create_snapshot(std::string name) = 0;
   virtual boost::asio::awaitable<protocol::snapshot_lifecycle_response> request_snapshot(protocol::snapshot_request) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "idempotent snapshot requests are not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::snapshot_lifecycle_response>
   snapshot_status(protocol::snapshot_status_request) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "snapshot status is not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::prune_response> prune(protocol::prune_request value) = 0;
   virtual boost::asio::awaitable<protocol::producer_status_response> producer_status(protocol::admin_query value) = 0;
   virtual boost::asio::awaitable<protocol::finalizer_status> get_finalizer_status(protocol::admin_query) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "finalizer status is not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::operator_identity> get_operator_identity(protocol::admin_query) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "operator identity is not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::node_status> get_node_status(protocol::admin_query) {
      FORGE_THROW_EXCEPTION(exceptions::unavailable, "node status is not implemented by this node");
   }
   virtual boost::asio::awaitable<protocol::supported_protocol_features_response>
   supported_protocol_features(protocol::supported_protocol_features_request value) = 0;
   virtual boost::asio::awaitable<protocol::ram_corrections_response>
   account_ram_corrections(protocol::ram_corrections_request value) = 0;
   virtual boost::asio::awaitable<protocol::unapplied_transactions_response>
   unapplied_transactions(protocol::unapplied_transactions_request value) = 0;
   virtual boost::asio::awaitable<protocol::snapshot_requests_response>
   snapshot_requests(protocol::admin_query value) = 0;
   virtual boost::asio::awaitable<bool> configure_pause(protocol::producer_pause_request value) = 0;
   virtual boost::asio::awaitable<bool> update_runtime_options(protocol::producer_runtime_options value) = 0;
   virtual boost::asio::awaitable<bool> update_greylist(protocol::greylist_update_request value) = 0;
   virtual boost::asio::awaitable<bool> set_access_policy(protocol::producer_access_policy value) = 0;
   virtual boost::asio::awaitable<protocol::snapshot_schedule>
   schedule_snapshot(protocol::snapshot_schedule_request value) = 0;
   virtual boost::asio::awaitable<protocol::snapshot_schedule>
   unschedule_snapshot(protocol::snapshot_schedule_id value) = 0;
   virtual boost::asio::awaitable<protocol::integrity_hash_response> integrity_hash(protocol::admin_query value) = 0;
   virtual boost::asio::awaitable<bool> schedule_protocol_features(std::vector<protocol::digest> value) = 0;
};

} // namespace forge::chain::api

export namespace forge::api::core {

template <> struct method_descriptor_customization<::forge::chain::api::admin> {
   template <auto Method, bool EnableRaw>
   static void apply(method_builder<::forge::chain::api::admin, EnableRaw>& method) {
      static_cast<void>(Method);
      ::forge::chain::api::exceptions::descriptor::declare_common(method);
      using request_type = method_request_t<Method>;
      if constexpr (std::is_same_v<request_type, ::forge::chain::protocol::snapshot_status_request>) {
         ::forge::chain::api::exceptions::descriptor::declare_not_found(method);
      }
      if constexpr (std::is_same_v<request_type, ::forge::chain::protocol::snapshot_request>) {
         ::forge::chain::api::exceptions::descriptor::declare_snapshot_lost(method);
      }
      if constexpr (std::is_same_v<request_type, ::forge::chain::protocol::signed_block> ||
                    std::is_same_v<request_type, ::std::string> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::snapshot_request> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::prune_request> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::producer_pause_request> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::producer_runtime_options> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::greylist_update_request> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::producer_access_policy> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::snapshot_schedule_request> ||
                    std::is_same_v<request_type, ::forge::chain::protocol::snapshot_schedule_id> ||
                    std::is_same_v<request_type, ::std::vector<::forge::chain::protocol::digest>>) {
         ::forge::chain::api::exceptions::descriptor::declare_mutation(method);
      }
   }
};

} // namespace forge::api::core

FORGE_EXPORT_API(
    ::forge::chain::api::admin, FORGE_API_CONTRACT("forge.chain.api.admin", 2, 4),
    FORGE_API_METHOD_TYPED(push_block, ::forge::chain::protocol::signed_block,
                           ::forge::chain::protocol::push_block_response),
    FORGE_API_METHOD(create_snapshot, name),
    FORGE_API_METHOD_TYPED_SINCE(request_snapshot, ::forge::chain::protocol::snapshot_request,
                                 ::forge::chain::protocol::snapshot_lifecycle_response, 2),
    FORGE_API_METHOD_TYPED_SINCE(snapshot_status, ::forge::chain::protocol::snapshot_status_request,
                                 ::forge::chain::protocol::snapshot_lifecycle_response, 2),
    FORGE_API_METHOD_TYPED(prune, ::forge::chain::protocol::prune_request, ::forge::chain::protocol::prune_response),
    FORGE_API_METHOD_TYPED(producer_status, ::forge::chain::protocol::admin_query,
                           ::forge::chain::protocol::producer_status_response),
    FORGE_API_METHOD_TYPED_SINCE(get_finalizer_status, ::forge::chain::protocol::admin_query,
                                 ::forge::chain::protocol::finalizer_status, 4),
    FORGE_API_METHOD_TYPED_SINCE(get_operator_identity, ::forge::chain::protocol::admin_query,
                                 ::forge::chain::protocol::operator_identity, 1),
    FORGE_API_METHOD_TYPED_SINCE(get_node_status, ::forge::chain::protocol::admin_query,
                                 ::forge::chain::protocol::node_status, 1),
    FORGE_API_METHOD_TYPED(supported_protocol_features, ::forge::chain::protocol::supported_protocol_features_request,
                           ::forge::chain::protocol::supported_protocol_features_response),
    FORGE_API_METHOD_TYPED(account_ram_corrections, ::forge::chain::protocol::ram_corrections_request,
                           ::forge::chain::protocol::ram_corrections_response),
    FORGE_API_METHOD_TYPED(unapplied_transactions, ::forge::chain::protocol::unapplied_transactions_request,
                           ::forge::chain::protocol::unapplied_transactions_response),
    FORGE_API_METHOD_TYPED(snapshot_requests, ::forge::chain::protocol::admin_query,
                           ::forge::chain::protocol::snapshot_requests_response),
    FORGE_API_METHOD_TYPED(configure_pause, ::forge::chain::protocol::producer_pause_request, bool),
    FORGE_API_METHOD_TYPED(update_runtime_options, ::forge::chain::protocol::producer_runtime_options, bool),
    FORGE_API_METHOD_TYPED(update_greylist, ::forge::chain::protocol::greylist_update_request, bool),
    FORGE_API_METHOD_TYPED(set_access_policy, ::forge::chain::protocol::producer_access_policy, bool),
    FORGE_API_METHOD_TYPED(schedule_snapshot, ::forge::chain::protocol::snapshot_schedule_request,
                           ::forge::chain::protocol::snapshot_schedule),
    FORGE_API_METHOD_TYPED(unschedule_snapshot, ::forge::chain::protocol::snapshot_schedule_id,
                           ::forge::chain::protocol::snapshot_schedule),
    FORGE_API_METHOD_TYPED(integrity_hash, ::forge::chain::protocol::admin_query,
                           ::forge::chain::protocol::integrity_hash_response),
    FORGE_API_METHOD(schedule_protocol_features, features))

FORGE_HTTP_API(
    ::forge::chain::api::admin, FORGE_HTTP_POST(push_block, "/v1/chain/admin/blocks", accepted),
    FORGE_HTTP_POST(create_snapshot, "/v1/chain/admin/snapshots?name={name}", accepted),
    FORGE_HTTP_POST(request_snapshot, "/v1/chain/admin/snapshot-requests", accepted, FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(snapshot_status, "/v1/chain/admin/snapshot-requests/{request_id}", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_POST(prune, "/v1/chain/admin/pruning", ok),
    FORGE_HTTP_GET(producer_status, "/v1/chain/admin/producer", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_finalizer_status, "/v1/chain/admin/finalizer", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_operator_identity, "/v1/chain/admin/operator-identity", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(get_node_status, "/v1/chain/admin/node-status", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(supported_protocol_features,
                   "/v1/chain/admin/protocol-features/supported?exclude_disabled={exclude_disabled}"
                   "&exclude_unactivatable={exclude_unactivatable}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(account_ram_corrections,
                   "/v1/chain/admin/accounts/ram-corrections?lower_bound={lower_bound}&upper_bound={upper_bound}"
                   "&limit={limit}&reverse={reverse}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(unapplied_transactions,
                   "/v1/chain/admin/transactions/unapplied?lower_bound={lower_bound}&limit={limit}"
                   "&time_limit_ms={time_limit_ms}",
                   FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_GET(snapshot_requests, "/v1/chain/admin/snapshots/schedules", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_PUT(configure_pause, "/v1/chain/admin/producer/pause", ok),
    FORGE_HTTP_PATCH(update_runtime_options, "/v1/chain/admin/producer/runtime-options", ok),
    FORGE_HTTP_PATCH(update_greylist, "/v1/chain/admin/producer/greylist", ok),
    FORGE_HTTP_PUT(set_access_policy, "/v1/chain/admin/producer/access-policy", ok),
    FORGE_HTTP_POST(schedule_snapshot, "/v1/chain/admin/snapshots/schedules", accepted),
    FORGE_HTTP_DELETE(unschedule_snapshot, "/v1/chain/admin/snapshots/schedules/{id}", ok),
    FORGE_HTTP_GET(integrity_hash, "/v1/chain/admin/integrity", FORGE_HTTP_CACHE(no_store)),
    FORGE_HTTP_PUT(schedule_protocol_features, "/v1/chain/admin/protocol-features/scheduled", ok))
