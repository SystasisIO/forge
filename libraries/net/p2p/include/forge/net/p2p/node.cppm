module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

export module forge.net.p2p.node;

import forge.asio.runtime;
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dht;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.connection_gater;
import forge.net.p2p.dialing;
import forge.net.p2p.discovery;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.chrono.timestamp;
import forge.net.p2p.ipns;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
export import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.rendezvous;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.transport.limits;

export namespace forge::net::p2p {

class node {
 public:
   struct limits {
      std::size_t max_sessions = 1024;
      std::size_t session_low_watermark = 1024;
      std::chrono::milliseconds session_grace_period{60'000};
      std::chrono::milliseconds session_prune_silence{10'000};
      std::chrono::milliseconds dial_backoff_base{5'000};
      std::chrono::milliseconds dial_backoff_step{1'000};
      std::chrono::milliseconds dial_backoff_max{300'000};
      std::size_t max_protocol_handlers = 1024;
      std::size_t max_peer_exchange_message_size = 4 * 1024 * 1024;
      std::size_t max_peer_exchange_records = 1024;
      std::size_t max_peer_exchange_queue = 4096;
      relay::limits relay{};
      resource_manager::limits resources{};
      discovery::policy discovery{};
      rendezvous::options rendezvous{};
      pubsub::options pubsub{};
      topology::policy topology{};
   };

   struct options {
      std::string certificate_pem;
      std::string private_key_pem;
      std::optional<peer_id> explicit_peer_id;
      // The gater may run on concurrent direct transport operations. Implementations
      // must be synchronous, nonblocking and internally thread-safe.
      std::shared_ptr<connection_gater> connection_gater;
      capability_set capabilities{.bits = capabilities::direct_quic | capabilities::peer_exchange};
      limits limits{};
      relay::policy relay_policy{.service_enabled = true, .client_enabled = true, .public_relay_allowed = false};
      path::policy path_policy{};
      forge::net::p2p::reachability_policy reachability_policy{};
      std::optional<forge::net::p2p::private_network::options> private_network;
      address_resolution::policy dns_resolution{};
      forge::net::dns::resolver_options dns_resolver{};
      dialing::policy direct_dial{};
      forge::net::transport::limits transport_limits{};
      std::vector<forge::net::p2p::endpoint> advertised_endpoints;
      std::vector<std::uint8_t> public_key;
      std::string protocol_version = "/forge/1.0.0";
      std::string agent_version = "forge/1.0.0";
      peer_store::options peer_state;
      bool allow_insecure_test_mode = false;
      identify::limits identify;
      lifecycle_options lifecycle;
      std::vector<dht::profile> dht_profiles;
      std::map<protocol_id, std::shared_ptr<dht::record_store::persistence>> dht_record_persistence;
   };

   struct connect_options {
      std::optional<peer_id> expected_peer;
      bool allow_relay = true;
      std::optional<peer_id> relay_peer;
      std::chrono::milliseconds timeout{10'000};
      std::chrono::milliseconds direct_attempt_timeout{2'000};
      std::chrono::milliseconds relay_attempt_timeout{5'000};
      std::size_t max_direct_endpoints = 4;
      std::size_t max_relay_candidates = 4;
      bool allow_hole_punch = true;
   };

   struct open_options {
      bool allow_relay = true;
      std::optional<peer_id> relay_peer;
      std::chrono::milliseconds timeout{10'000};
      std::chrono::milliseconds direct_attempt_timeout{2'000};
      std::chrono::milliseconds relay_attempt_timeout{5'000};
      std::size_t max_direct_endpoints = 4;
      std::size_t max_relay_candidates = 4;
      bool allow_hole_punch = true;
   };

   struct session_info {
      peer_id remote_peer;
      capability_set capabilities{};
      path::kind path = path::kind::direct;
      std::optional<peer_id> relay_peer;
      identify::state identify_state = identify::state::unknown;
   };

   struct incoming_protocol_stream {
      session_info session;
      protocol_id protocol;
      forge::net::p2p::stream stream;
   };

   using protocol_handler = std::function<boost::asio::awaitable<void>(incoming_protocol_stream)>;

   using metrics_snapshot = diagnostics::metrics_snapshot;

   node(forge::asio::runtime& runtime, options options_value);
   ~node();

   node(const node&) = delete;
   node& operator=(const node&) = delete;

   node(node&&) noexcept;
   node& operator=(node&&) noexcept;

   [[nodiscard]] const peer_id& local_peer() const noexcept;
   [[nodiscard]] std::optional<forge::net::p2p::endpoint> local_endpoint() const;
   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints() const;
   [[nodiscard]] metrics_snapshot metrics() const;
   [[nodiscard]] forge::net::p2p::diagnostics::snapshot
   diagnostics(forge::net::p2p::diagnostics::options options = {}) const;
   [[nodiscard]] peer_store& peers() noexcept;
   [[nodiscard]] const peer_store& peers() const noexcept;
   [[nodiscard]] dht::routing_status routing_status(const protocol_id& profile) const;
   [[nodiscard]] lifecycle_status lifecycle_state() const;
   [[nodiscard]] host_event reachability_status() const;
   [[nodiscard]] host_event_subscription host_events() const;

   void protect_peer(peer_id peer, std::string tag = "manual");
   [[nodiscard]] bool unprotect_peer(peer_id peer, std::string tag = "manual");
   [[nodiscard]] bool is_peer_protected(const peer_id& peer) const;
   void tag_peer(peer_id peer, std::string tag, std::int64_t value);
   [[nodiscard]] bool untag_peer(peer_id peer, std::string_view tag);

   void register_protocol_handler(protocol_id protocol, protocol_handler handler);
   [[nodiscard]] bool unregister_protocol_handler(const protocol_id& protocol);
   void set_advertised_endpoints(std::vector<forge::net::p2p::endpoint> endpoints);
   boost::asio::awaitable<lifecycle_status> async_start();
   boost::asio::awaitable<void> async_set_bootstrap(std::vector<bootstrap_peer> peers);
   boost::asio::awaitable<void> async_listen(forge::net::p2p::endpoint endpoint);
   boost::asio::awaitable<void> async_hydrate_peer_state();
   boost::asio::awaitable<session_info> async_connect(forge::net::p2p::endpoint endpoint);
   boost::asio::awaitable<session_info> async_connect(forge::net::p2p::endpoint endpoint, connect_options options);
   boost::asio::awaitable<session_info> async_connect(forge::multiformats::multiaddr address);
   boost::asio::awaitable<session_info> async_connect(forge::multiformats::multiaddr address, connect_options options);
   boost::asio::awaitable<void> async_request_peer_exchange(peer_id peer);
   boost::asio::awaitable<reachability::state> async_probe_reachability(peer_id observer);
   boost::asio::awaitable<relay::reservation::info> async_reserve_relay(peer_id relay_peer);
   boost::asio::awaitable<relay::reservation::info> async_reserve_relay(peer_id relay_peer,
                                                                        relay::reservation::options options);
   boost::asio::awaitable<std::vector<relay::reservation::info>> async_refresh_relay_candidates();
   boost::asio::awaitable<std::vector<discovery::result>> async_refresh_discovery();
   boost::asio::awaitable<void> async_cancel_relay(peer_id relay_peer);
   boost::asio::awaitable<dht::query_result> async_find_peer(protocol_id profile, peer_id peer,
                                                             dht::query_options options = {});
   boost::asio::awaitable<provider_registration> async_provide(protocol_id profile, dht::key key,
                                                               dht::query_options options = {});
   boost::asio::awaitable<std::vector<dht::peer>> async_find_providers(protocol_id profile, dht::key key,
                                                                       dht::query_options options = {});
   boost::asio::awaitable<dht::value_put_result> async_put_value(protocol_id profile, dht::record value,
                                                                 dht::query_options options = {});
   boost::asio::awaitable<dht::value_get_result> async_get_value(protocol_id profile, dht::key key,
                                                                 dht::query_options options = {});
   [[nodiscard]] ipns::record create_ipns_record(std::span<const std::uint8_t> value, std::uint64_t sequence,
                                                 forge::chrono::timestamp eol, std::chrono::nanoseconds ttl,
                                                 ipns::create_options options = {}) const;
   boost::asio::awaitable<rendezvous::register_response>
   async_rendezvous_register(peer_id rendezvous_peer, rendezvous::register_request request);
   boost::asio::awaitable<rendezvous::discover_response>
   async_rendezvous_discover(peer_id rendezvous_peer, rendezvous::discover_request request);
   boost::asio::awaitable<pubsub::subscription> async_subscribe(pubsub::topic subject, pubsub::handler handler);
   boost::asio::awaitable<void> async_unsubscribe(pubsub::topic subject);
   boost::asio::awaitable<pubsub::message> async_publish(pubsub::topic subject, std::vector<std::uint8_t> data);
   boost::asio::awaitable<pubsub::message> async_publish(pubsub::topic subject, std::vector<std::uint8_t> data,
                                                         pubsub::publish_options options);
   [[nodiscard]] pubsub::snapshot pubsub_snapshot() const;
   boost::asio::awaitable<std::chrono::milliseconds> async_ping(peer_id peer);
   boost::asio::awaitable<std::chrono::milliseconds> async_ping(peer_id peer, open_options options);
   boost::asio::awaitable<hole_punch::status>
   async_attempt_hole_punch(peer_id peer, std::optional<peer_id> relay_peer = std::nullopt,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds{10'000});
   boost::asio::awaitable<forge::net::p2p::stream> async_open_protocol_stream(peer_id peer, protocol_id protocol);
   boost::asio::awaitable<forge::net::p2p::stream> async_open_protocol_stream(peer_id peer, protocol_id protocol,
                                                                              open_options options);
   // Preview: begins graceful lifecycle shutdown without tearing down sessions.
   void request_stop() noexcept;
   boost::asio::awaitable<void> async_stop();
   void stop();

 private:
   friend struct node_session_fixture;
   struct impl;
   std::shared_ptr<impl> impl_;
};

void validate(const node::options& options);

} // namespace forge::net::p2p
