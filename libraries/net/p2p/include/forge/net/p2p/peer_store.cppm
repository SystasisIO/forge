module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.net.p2p.peer_store;

import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.scoring;
import forge.multiformats.multiaddr;

export namespace forge::net::p2p {

class peer_store {
 public:
   class persistence;

   struct endpoint_sources {
      bool learned = true;
      bool identify_unsigned = false;
      bool identify_signed = false;
   };

   struct endpoint_record {
      forge::multiformats::multiaddr address;
      path::kind kind = path::kind::direct;
      std::optional<peer_id> relay_peer;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      std::chrono::system_clock::time_point backoff_until{};
      double score = 0.0;
      endpoint_sources sources;
   };

   struct relay_record {
      peer_id relay;
      std::uint64_t reservation_id = 0;
      std::chrono::system_clock::time_point expires_at{};
      std::vector<forge::net::p2p::endpoint> endpoints;
      std::vector<std::uint8_t> voucher;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      double score = 0.0;
   };

   struct record {
      peer_id peer;
      capability_set capabilities{};
      discovery::source discovered_by = discovery::source::explicit_config;
      std::string protocol_version;
      std::string agent_version;
      std::vector<std::uint8_t> public_key;
      std::vector<protocol_id> protocols;
      // Original envelope bytes. Identify verifies signatures and sequence;
      // direct upsert and hydration do not authenticate these bytes. Consumers
      // must verify and apply their address policy before forwarding.
      std::vector<std::uint8_t> signed_peer_record;
      std::vector<endpoint_record> endpoints;
      std::vector<relay_record> relay_reservations;
      reachability::state reachability = reachability::state::unknown;
      std::optional<forge::net::p2p::endpoint> observed_endpoint;
      std::chrono::system_clock::time_point reachability_expires_at{};
      std::chrono::system_clock::time_point discovered_at{};
      std::chrono::system_clock::time_point discovery_expires_at{};
      std::chrono::system_clock::time_point discovery_backoff_until{};
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      double score = 0.0;
   };

   struct identify_update {
      std::optional<std::string> protocol_version;
      std::optional<std::string> agent_version;
      std::optional<std::vector<std::uint8_t>> public_key;
      std::optional<std::vector<protocol_id>> protocols;
      std::optional<capability_set> capabilities;
      std::optional<std::vector<std::uint8_t>> signed_peer_record;
      std::optional<std::vector<forge::multiformats::multiaddr>> signed_endpoints;
      std::optional<std::vector<forge::multiformats::multiaddr>> unsigned_endpoints;
      bool replace_observed_endpoint = false;
      std::optional<forge::net::p2p::endpoint> observed_endpoint;
   };

   struct discovery_update {
      discovery::source source = discovery::source::explicit_config;
      std::chrono::system_clock::time_point observed_at{};
      std::chrono::system_clock::time_point expires_at{};
   };

   struct rendezvous_key {
      std::string namespace_name;
      peer_id peer;
   };

   struct mutation_batch {
      std::vector<record> peer_upserts;
      std::vector<peer_id> peer_removals;
      std::vector<rendezvous::registration> rendezvous_upserts;
      std::vector<rendezvous_key> rendezvous_removals;
      std::uint64_t rendezvous_sequence_high_watermark = 0;
      bool durable = false;
   };

   enum class hydration_kind : std::uint8_t {
      peers,
      rendezvous,
   };

   struct hydration_request {
      hydration_kind kind = hydration_kind::peers;
      std::optional<std::vector<std::byte>> cursor;
      std::size_t limit = 256;
   };

   struct hydration_page {
      std::vector<record> peers;
      std::vector<rendezvous::registration> rendezvous_registrations;
      std::uint64_t rendezvous_sequence_high_watermark = 0;
      std::optional<std::vector<std::byte>> cursor;
   };

   struct apply_result {
      bool durability_confirmed = true;
      std::string durability_failure;
   };

   struct prune_result {
      std::vector<peer_id> peers;
      std::vector<rendezvous::registration> rendezvous_registrations;
      bool may_have_more = false;
   };

   struct persistence_status {
      std::size_t pending_peer_mutations = 0;
      std::uint64_t failure_count = 0;
      bool degraded = false;
      bool closing = false;
      bool closed = false;
      std::string last_failure;
   };

   struct options {
      std::shared_ptr<persistence> persistence;
      std::size_t max_peers = 4'096;
      std::size_t max_rendezvous = 16'384;
      std::size_t max_pending = 4'096;
      std::size_t max_endpoints_per_peer = 64;
      std::size_t max_protocols_per_peer = 128;
      std::size_t max_relay_reservations_per_peer = 64;
      std::size_t max_relay_endpoints_per_reservation = 16;
      std::size_t max_peer_record_bytes = 1024 * 1024;
      std::size_t hydration_page_limit = 256;
      std::size_t prune_page_limit = 256;
      std::size_t max_persistence_waiters = 256;
   };

   peer_store();
   explicit peer_store(options options_value);
   ~peer_store();

   peer_store(const peer_store&) = delete;
   peer_store& operator=(const peer_store&) = delete;

   peer_store(peer_store&&) noexcept;
   peer_store& operator=(peer_store&&) noexcept;

   [[nodiscard]] static std::shared_ptr<persistence> make_memory_persistence();

   void upsert(record value);
   [[nodiscard]] record apply_identify(const peer_id& peer, identify_update update);
   [[nodiscard]] std::optional<record> apply_discovery(const peer_id& peer, discovery_update update);
   void apply_peer_exchange(const peer_id& peer, capability_set capabilities);
   void upsert_relay_reservation(relay_record value);
   [[nodiscard]] bool mark_discovery_failure(const peer_id& peer, std::chrono::system_clock::time_point backoff_until);
   [[nodiscard]] std::size_t prune_expired_relay_reservations(const peer_id& peer,
                                                              std::chrono::system_clock::time_point now);
   void learn_address(peer_id peer, forge::multiformats::multiaddr address, capability_set capabilities = {});
   void learn_endpoint(peer_id peer, forge::net::p2p::endpoint endpoint, capability_set capabilities = {});
   void mark_reachability(peer_id peer, reachability::state state,
                          std::optional<forge::net::p2p::endpoint> observed = std::nullopt);
   void mark_success(const peer_id& peer, path::kind kind, std::chrono::milliseconds latency);
   void mark_failure(const peer_id& peer);
   void mark_address_success(const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind,
                             std::chrono::milliseconds latency);
   void mark_address_failure(const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind,
                             std::chrono::system_clock::time_point backoff_until);
   void mark_endpoint_success(const peer_id& peer, const forge::net::p2p::endpoint& endpoint, path::kind kind,
                              std::chrono::milliseconds latency);
   void mark_endpoint_failure(const peer_id& peer, const forge::net::p2p::endpoint& endpoint, path::kind kind,
                              std::chrono::system_clock::time_point backoff_until);
   void upsert_routing_peer(protocol_id protocol, dht::peer value, discovery::source source,
                            std::chrono::system_clock::time_point expires_at);

   boost::asio::awaitable<void> async_upsert_rendezvous(rendezvous::registration value);
   boost::asio::awaitable<void> async_register_rendezvous(rendezvous::registration value,
                                                          std::size_t max_registrations_per_peer);
   boost::asio::awaitable<void> async_remove_rendezvous(peer_id peer, std::string namespace_name);
   boost::asio::awaitable<void> async_hydrate();
   boost::asio::awaitable<prune_result>
   async_prune_expired(std::chrono::system_clock::time_point now = std::chrono::system_clock::now());
   boost::asio::awaitable<void> async_flush();
   boost::asio::awaitable<void> async_close();

   [[nodiscard]] std::optional<record> find(const peer_id& peer) const;
   [[nodiscard]] std::optional<public_key> find_public_key(const peer_id& peer) const;
   [[nodiscard]] std::vector<record> snapshot(std::size_t limit) const;
   [[nodiscard]] std::vector<record> candidates(std::uint64_t capability, std::size_t limit) const;
   [[nodiscard]] std::vector<record> scored_candidates(std::size_t limit) const;
   [[nodiscard]] std::vector<record> scored_candidates(discovery::source source, std::size_t limit) const;
   [[nodiscard]] std::vector<rendezvous::registration>
   discover_rendezvous(std::string_view namespace_name, std::uint64_t after_sequence, std::size_t limit) const;
   [[nodiscard]] persistence_status persistence_state() const;

 private:
   struct impl;
   std::shared_ptr<impl> impl_;
};

class peer_store::persistence {
 public:
   virtual ~persistence();

   virtual boost::asio::awaitable<hydration_page> async_hydrate(hydration_request request) = 0;
   // A false durability confirmation means the mutation committed, but the
   // requested durable flush failed. Throwing means no commit is known.
   virtual boost::asio::awaitable<apply_result> async_apply(mutation_batch batch) = 0;
   virtual boost::asio::awaitable<prune_result> async_prune_expired(std::chrono::system_clock::time_point now,
                                                                    std::size_t limit) = 0;
   virtual boost::asio::awaitable<void> async_flush() = 0;
   virtual boost::asio::awaitable<void> async_close() = 0;
};

} // namespace forge::net::p2p
