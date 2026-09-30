#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config.hxx"
#include "lifecycle_phase.hxx"
#include "object_dht_record_store_adapter.hxx"

namespace forge::plugins::net::p2p::node {

struct plugin::impl {
   using route = std::pair<forge::net::p2p::protocol_id, forge::net::p2p::node::protocol_handler>;

   impl();
   ~impl();

   forge::net::p2p::node::options options{
       .allow_insecure_test_mode = false,
   };
   forge::api::transport::options api_options{
       .codec = forge::api::core::codec_id{.value = "forge.raw"},
       .max_inflight = 64,
   };
   parsed_policy policy{};
   std::vector<route> routes;
   std::string peer_store_name;
   std::string certificate_secret;
   std::string private_key_secret;
   forge::plugins::db::store::api* stores = nullptr;
   forge::plugins::crypto::secrets::api* secrets = nullptr;
   std::shared_ptr<forge::plugins::db::store::store_handle> peer_state_store;
   std::shared_ptr<forge::net::p2p::peer_store::persistence> peer_state;
   std::vector<std::pair<forge::net::p2p::protocol_id, std::shared_ptr<object_dht_record_store_adapter>>> dht_state;
   forge::net::p2p::pubsub::options pubsub_options{};
   std::shared_ptr<detail::api_publication_registry> api_publications;
   std::shared_ptr<forge::net::p2p::node> node;
   forge::asio::runtime* runtime = nullptr;
   std::atomic_bool stop_requested = false;
   std::atomic<lifecycle_phase> phase = lifecycle_phase::idle;
   mutable std::mutex configuration_mutex;
   bool pubsub_requested = false;
   bool reset_incompatible_peer_state = true;

   [[nodiscard]] std::optional<std::vector<route>> begin_startup();
   void mark_started() noexcept;
   void mark_stopped() noexcept;
   [[nodiscard]] bool is_started() const noexcept;
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> ensure_node(const std::vector<route>& startup_routes);
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> node_snapshot() const noexcept;
   [[nodiscard]] std::shared_ptr<forge::net::p2p::node> require_node() const;
   void add_route(forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler);
   void enable_pubsub(forge::net::p2p::pubsub::options options);
   [[nodiscard]] forge::net::p2p::node::open_options open_options_for(remote_options value) const;
   [[nodiscard]] forge::api::transport::options api_options_for(const remote_options& value) const;
};

} // namespace forge::plugins::net::p2p::node
