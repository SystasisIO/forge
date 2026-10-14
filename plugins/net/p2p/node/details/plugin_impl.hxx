#pragma once

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "configuration_policy.hxx"
#include "configuration_snapshot.hxx"
#include "object_dht_record_store_adapter.hxx"
#include "protocol_registry.hxx"
#include "peer_contributions.hxx"
#include "shutdown_attempt.hxx"

namespace forge::plugins::net::p2p::node {

struct plugin::impl {
   explicit impl(forge::app::plugin_lifecycle state);
   const forge::app::plugin_lifecycle lifecycle;
   ~impl();

   std::shared_ptr<const detail::configuration_snapshot> configuration;
   std::shared_ptr<const detail::configuration_snapshot> startup_configuration;
   forge::plugins::db::store::api* stores = nullptr;
   forge::plugins::crypto::secrets::api* secrets = nullptr;
   std::shared_ptr<forge::plugins::db::store::store_handle> peer_state_store;
   std::shared_ptr<forge::net::p2p::peer_store::persistence> peer_state;
   std::vector<std::pair<forge::net::p2p::protocol_id, std::shared_ptr<object_dht_record_store_adapter>>> dht_state;
   std::shared_ptr<detail::protocol_registry> protocols;
   std::shared_ptr<detail::peer_contributions> contributions;
   std::shared_ptr<detail::pubsub_service> pubsub;
   std::shared_ptr<forge::net::p2p::node> node;
   forge::asio::runtime* runtime = nullptr;
   mutable std::mutex configuration_mutex;
   bool providers_bound = false;
   std::optional<std::string> bound_peer_store_name;
   std::size_t setup_operations = 0;
   forge::asio::notification setup_changed;
   std::shared_ptr<detail::shutdown_attempt> shutdown;

   [[nodiscard]] std::shared_ptr<const detail::configuration_snapshot> configuration_snapshot() const;
   [[nodiscard]] std::shared_ptr<const detail::configuration_snapshot> configuration_candidate_base() const;
   void commit_configuration(std::shared_ptr<const detail::configuration_snapshot> previous,
                             std::shared_ptr<const detail::configuration_snapshot> candidate);
   void commit_diagnostics_configuration(std::shared_ptr<const detail::configuration_snapshot> previous,
                                         config candidate);
   [[nodiscard]] bool begin_startup(forge::net::p2p::node::options& prepared,
                                    std::shared_ptr<const detail::configuration_snapshot>& frozen);
   void finish_setup() noexcept;
   void request_stop() noexcept;
   [[nodiscard]] bool is_started() const noexcept;
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> ensure_node(forge::net::p2p::node::options options);
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> node_snapshot() const noexcept;
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> require_node() const;
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> require_running_node() const;
   [[nodiscard]] forge::net::p2p::node::open_options open_options_for(remote_options value) const;
   [[nodiscard]] forge::api::transport::options api_options_for(const remote_options& value) const;
};

} // namespace forge::plugins::net::p2p::node
