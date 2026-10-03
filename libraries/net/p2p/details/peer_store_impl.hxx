#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace forge::net::p2p {

struct peer_store::impl {
   class persistence_admission;
   class close_admission;
   class peer_mutation_stage;

   struct peer_mutation {
      std::optional<peer_store::record> value;
   };

   explicit impl(peer_store::options options_value);

   void upsert(peer_store::record value);
   [[nodiscard]] peer_store::record apply_identify(const peer_id& peer, peer_store::identify_update update);
   [[nodiscard]] std::optional<peer_store::record> apply_discovery(const peer_id& peer,
                                                                   peer_store::discovery_update update);
   void apply_peer_exchange(const peer_id& peer, capability_set capabilities);
   void upsert_relay_reservation(peer_store::relay_record value);
   [[nodiscard]] bool mark_discovery_failure(const peer_id& peer, std::chrono::system_clock::time_point backoff_until);
   [[nodiscard]] std::size_t prune_expired_relay_reservations(const peer_id& peer,
                                                              std::chrono::system_clock::time_point now);
   void learn_address(peer_id peer, forge::multiformats::multiaddr address, capability_set capabilities);
   void mark_reachability(peer_id peer, reachability::state state, std::optional<forge::net::p2p::endpoint> observed);
   void mark_success(const peer_id& peer, path::kind kind, std::chrono::milliseconds latency);
   void mark_failure(const peer_id& peer);
   void mark_address_success(const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind,
                             std::chrono::milliseconds latency);
   void mark_address_failure(const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind,
                             std::chrono::system_clock::time_point backoff_until);
   void upsert_routing_peer(protocol_id protocol, dht::peer value, discovery::source source,
                            std::chrono::system_clock::time_point expires_at);

   boost::asio::awaitable<void> async_upsert_rendezvous(rendezvous::registration value);
   boost::asio::awaitable<void> async_register_rendezvous(rendezvous::registration value,
                                                          std::size_t max_registrations_per_peer);
   boost::asio::awaitable<void> async_remove_rendezvous(peer_id peer, std::string namespace_name);
   boost::asio::awaitable<void> async_hydrate();
   boost::asio::awaitable<peer_store::prune_result> async_prune_expired(std::chrono::system_clock::time_point now);
   boost::asio::awaitable<void> async_flush();
   boost::asio::awaitable<void> async_close();

   static boost::asio::awaitable<void> async_upsert_rendezvous_owned(std::shared_ptr<impl> self,
                                                                     rendezvous::registration value);
   static boost::asio::awaitable<void> async_register_rendezvous_owned(std::shared_ptr<impl> self,
                                                                       rendezvous::registration value,
                                                                       std::size_t max_registrations_per_peer);
   static boost::asio::awaitable<void> async_remove_rendezvous_owned(std::shared_ptr<impl> self, peer_id peer,
                                                                     std::string namespace_name);
   static boost::asio::awaitable<void> async_hydrate_owned(std::shared_ptr<impl> self);
   static boost::asio::awaitable<peer_store::prune_result>
   async_prune_expired_owned(std::shared_ptr<impl> self, std::chrono::system_clock::time_point now);
   static boost::asio::awaitable<void> async_flush_owned(std::shared_ptr<impl> self);
   static boost::asio::awaitable<void> async_close_owned(std::shared_ptr<impl> self);

   [[nodiscard]] std::optional<peer_store::record> find(const peer_id& peer) const;
   [[nodiscard]] std::optional<public_key> find_public_key(const peer_id& peer) const;
   [[nodiscard]] std::vector<peer_store::record> snapshot(std::size_t limit) const;
   [[nodiscard]] std::vector<peer_store::record> candidates(std::uint64_t capability, std::size_t limit,
                                                            std::span<const peer_id> excluded_peers) const;
   [[nodiscard]] std::vector<peer_store::record> scored_candidates(std::size_t limit) const;
   [[nodiscard]] std::vector<peer_store::record> scored_candidates(discovery::source source, std::size_t limit) const;
   [[nodiscard]] std::vector<rendezvous::registration>
   discover_rendezvous(std::string_view namespace_name, std::uint64_t after_sequence, std::size_t limit) const;
   [[nodiscard]] peer_store::persistence_status persistence_state() const;

 private:
   using rendezvous_map_key = std::pair<std::string, peer_id>;
   using rendezvous_sequence_key = std::tuple<std::string, std::uint64_t, peer_id>;
   using rendezvous_global_sequence_key = std::pair<std::uint64_t, rendezvous_map_key>;
   using score_key = std::pair<double, peer_id>;
   using peer_expiry_key = std::pair<std::chrono::system_clock::time_point, peer_id>;
   using rendezvous_expiry_key = std::pair<std::chrono::system_clock::time_point, rendezvous_map_key>;

   [[nodiscard]] peer_store::record mutate_peer(const peer_id& peer,
                                                const std::function<void(peer_store::record&)>& mutation);
   void commit_peer_mutation(peer_store::record value);
   void commit_peer_mutation_locked(peer_store::record value);
   [[nodiscard]] std::optional<peer_id> store_peer_operational(peer_store::record value);
   void erase_peer_operational(const peer_id& peer);
   void add_peer_indexes(const peer_store::record& value);
   void remove_peer_indexes(const peer_store::record& value);
   void ensure_peer_mutation_capacity_locked(const std::vector<peer_id>& peers) const;
   void requeue_peer_mutations_locked(const std::map<peer_id, peer_mutation>& values);
   void complete_peer_mutations_locked(const std::map<peer_id, peer_mutation>& values);
   void ensure_open_locked() const;
   void mark_persistence_failure_locked(std::string message);
   void mark_durability_uncertain_locked(std::string message);
   void mark_persistence_healthy_locked(bool durability_confirmed = false);
   [[nodiscard]] std::pair<peer_store::mutation_batch, std::map<peer_id, peer_mutation>> take_pending_batch_locked();
   [[nodiscard]] std::size_t queued_unique_count_locked() const;
   [[nodiscard]] peer_store::record record_for_mutation_locked(const peer_id& peer) const;
   void hydrate_page_locked(peer_store::hydration_page page);
   void store_rendezvous_operational(rendezvous::registration value);
   void erase_rendezvous_operational(const rendezvous_map_key& key);
   boost::asio::awaitable<void> async_store_rendezvous(rendezvous::registration value,
                                                       std::optional<std::size_t> max_registrations_per_peer);
   void prune_operational_locked(std::chrono::system_clock::time_point now, const peer_store::prune_result& result);
   boost::asio::awaitable<void> apply_pending_locked_gate(bool flush_backend);
   boost::asio::awaitable<void> wait_for_persistence_admissions();
   [[nodiscard]] persistence_admission admit_persistence_operation();
   [[nodiscard]] std::optional<close_admission> admit_close_operation();
   void release_persistence_admission() noexcept;
   void release_close_admission() noexcept;

   peer_store::options options_;
   std::shared_ptr<peer_store::persistence> persistence_;
   mutable std::mutex mutex_;
   forge::asio::gate persistence_gate_;
   std::map<peer_id, peer_store::record> records_;
   std::set<score_key> score_index_;
   std::set<peer_expiry_key> peer_expiry_index_;
   std::map<std::uint64_t, std::set<score_key>> candidates_by_capability_;
   std::map<discovery::source, std::set<score_key>> candidates_by_source_;
   std::map<rendezvous_map_key, rendezvous::registration> rendezvous_;
   std::map<peer_id, std::size_t> rendezvous_per_peer_;
   std::map<rendezvous_sequence_key, rendezvous_map_key> rendezvous_by_sequence_;
   std::map<rendezvous_global_sequence_key, rendezvous_map_key> rendezvous_by_global_sequence_;
   std::set<rendezvous_expiry_key> rendezvous_expiry_index_;
   std::map<peer_id, peer_mutation> pending_peer_mutations_;
   std::map<peer_id, peer_mutation> in_flight_peer_mutations_;
   std::uint64_t rendezvous_sequence_ = 0;
   std::uint64_t persistence_failures_ = 0;
   std::size_t persistence_admissions_ = 0;
   std::map<const void*, std::function<void()>> persistence_admission_drainers_;
   std::size_t close_waiters_ = 0;
   bool degraded_ = false;
   bool durability_uncertain_ = false;
   bool closing_ = false;
   bool closed_ = false;
   std::string last_failure_;
};

class peer_store::impl::persistence_admission {
 public:
   explicit persistence_admission(peer_store::impl* owner) noexcept;
   ~persistence_admission();

   persistence_admission(const persistence_admission&) = delete;
   persistence_admission& operator=(const persistence_admission&) = delete;
   persistence_admission(persistence_admission&& other) noexcept;
   persistence_admission& operator=(persistence_admission&& other) noexcept;

 private:
   peer_store::impl* owner_ = nullptr;
};

class peer_store::impl::close_admission {
 public:
   explicit close_admission(peer_store::impl* owner) noexcept;
   ~close_admission();

   close_admission(const close_admission&) = delete;
   close_admission& operator=(const close_admission&) = delete;
   close_admission(close_admission&& other) noexcept;
   close_admission& operator=(close_admission&& other) noexcept;

 private:
   peer_store::impl* owner_ = nullptr;
};

class peer_store::impl::peer_mutation_stage {
 public:
   peer_mutation_stage(peer_store::impl* owner, std::map<peer_id, peer_mutation> updates);
   ~peer_mutation_stage();

   peer_mutation_stage(const peer_mutation_stage&) = delete;
   peer_mutation_stage& operator=(const peer_mutation_stage&) = delete;

   void commit() noexcept;

 private:
   peer_store::impl* owner_ = nullptr;
   std::vector<peer_id> keys_;
   std::map<peer_id, peer_mutation> previous_;
};

} // namespace forge::net::p2p
