#pragma once

#include "connection_manager.hxx"
#include "autorelay_manager.hxx"
#include "connection_gate.hxx"
#include "connection_singleflight_registry.hxx"
#include "direct_dial_root.hxx"
#include "direct_transport.hxx"
#include "dht_profile_state.hxx"
#include "dht_provider_registry.hxx"
#include "dht_routing_refresh.hxx"
#include "host_addresses.hxx"
#include "identify_service.hxx"
#include "length_delimited.hxx"
#include "libp2p_identity_material.hxx"
#include "lifecycle_tracker.hxx"
#include "operation_deadline.hxx"
#include "path_selector.hxx"
#include "path_manager.hxx"
#include "peer_exchange_cancellation.hxx"
#include "peer_exchange_codec.hxx"
#include "peer_exchange_scheduler.hxx"
#include "pubsub_backoff.hxx"
#include "pubsub_control_queue.hxx"
#include "pubsub_outbound_budget.hxx"
#include "relay_discovery.hxx"
#include "relay_transport.hxx"
#include "resource_stream.hxx"
#include "session_retirement.hxx"
#include "session_teardown.hxx"
#include "topology_manager.hxx"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "direct_attempt.hxx"

namespace forge::net::p2p {

class cancellation_latch;

namespace detail {

class bootstrap_service;
class dial_scheduler;
class lifecycle_wakeup;
class resource_stream;
class worker_terminal_owner;
class reachability_manager;
class observed_address_manager;
class host_event_source;
class mdns_service;
class coordinated_dial;
class pubsub_peer_score;
class pubsub_router;

} // namespace detail

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[noreturn]] void rethrow_transport_as_p2p(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;
[[nodiscard]] bool is_clean_stream_eof(const forge::exceptions::base& error) noexcept;
[[nodiscard]] std::uint64_t random_nonce();
[[nodiscard]] std::string bytes_key(std::span<const std::uint8_t> bytes);
[[nodiscard]] std::vector<std::uint8_t> wrap_length_delimited(std::span<const std::uint8_t> payload);
[[nodiscard]] std::vector<std::uint8_t> unwrap_length_delimited(std::span<const std::uint8_t> bytes,
                                                                std::size_t max_payload_size);
[[nodiscard]] peer_exchange_codec::options codec_for(const node::options& options) noexcept;
void validate_operation_timeout(std::chrono::milliseconds timeout, std::string_view name);
void validate_bootstrap(const std::vector<bootstrap_peer>& peers, bool require_nonempty,
                        const address_resolution::policy& resolution, bool tcp_only = false);
[[nodiscard]] std::chrono::milliseconds remaining_timeout(std::chrono::steady_clock::time_point started,
                                                          std::chrono::milliseconds timeout,
                                                          std::string_view operation);
[[nodiscard]] std::chrono::milliseconds
attempt_timeout(std::chrono::milliseconds remaining, std::chrono::milliseconds configured, std::string_view operation);
[[noreturn]] void throw_operation_timeout(std::string_view operation);
[[nodiscard]] resource_manager::limits resource_limits_for(const node::limits& limits);
void normalize_legacy_discovery(node::options& options);
void normalize_topology_capacity(node::options& options) noexcept;
void validate(const node::options& options);

struct node::impl : std::enable_shared_from_this<impl> {
   struct admitted_stream {
      protocol_id protocol;
      forge::net::p2p::stream stream;
      std::shared_ptr<detail::resource_stream> resource;
   };

   struct session_state {
      std::uint64_t id = 0;
      node::session_info info;
      peer_authentication authentication = peer_authentication::unverified;
      forge::net::transport::session connection;
      resource_manager::session_reservation resource;
      detail::session_retirement retirement;
      // Keeps the native socket descriptor reservation through security handoff.
      std::shared_ptr<void> native_lifetime;
      std::optional<forge::net::p2p::endpoint> direct_endpoint;
      std::vector<forge::multiformats::multiaddr> direct_roots;
      std::optional<forge::net::p2p::endpoint> local_endpoint;
      std::optional<forge::net::p2p::endpoint> remote_endpoint;
      connection_manager::direction direction = connection_manager::direction::outbound;
      std::string identify_error;
      bool identify_completed = false;
      std::uint64_t identify_push_attempted_generation = 0;
      std::uint64_t identify_push_delivered_generation = 0;
      bool identify_push_supported = false;
      std::vector<protocol_id> remote_protocols;
      std::atomic_bool closed = false;
   };

   struct dht_exchange_result {
      dht::message message;
      std::optional<forge::net::p2p::endpoint> remote_endpoint;
      std::optional<forge::net::p2p::endpoint> direct_endpoint;
   };

   struct opened_direct_stream {
      forge::net::p2p::stream stream;
      std::optional<forge::net::p2p::endpoint> remote_endpoint;
      std::optional<forge::net::p2p::endpoint> direct_endpoint;
   };

   struct topology_dht_batch {
      mutable std::mutex mutex;
      std::vector<protocol_id> profiles;
      std::vector<discovery::result> results;
      std::size_t next_profile = 0;
      std::size_t successful_profiles = 0;
      std::size_t failed_profiles = 0;
      std::exception_ptr first_failure;
   };

   struct identify_snapshot {
      std::uint64_t generation = 0;
      identify::document document;
   };

   struct identify_push_state {
      std::uint64_t generation = 1;
      mutable std::uint64_t cached_generation = 0;
      mutable std::uint64_t peer_record_sequence = 0;
      mutable identify::document cached_document;
      mutable std::vector<endpoint> cached_relay_endpoints;
      bool coordinator_running = false;
   };

   struct relay_reservation_operation;
   using path_dial_batch = detail::path_manager::dial_batch;

   struct relay_reservation_state {
      peer_id owner;
      peer_id relay_peer;
      std::uint64_t id = 0;
      std::chrono::steady_clock::time_point expires_at{};
      std::size_t max_streams = 0;
      std::uint64_t max_bytes = 0;
      std::size_t max_queued_bytes = 0;
      std::size_t active_streams = 0;
      bool canceled = false;
      resource_manager::relay_reservation resource;
      relay::reservation::info info;
      std::uint64_t session_id = 0;
      bool automatic = false;
      std::weak_ptr<relay_reservation_operation> operation;
      std::string remote_ip;
   };

   struct relay_admission {
      resource_manager::relay_reservation circuit;
      std::optional<std::uint64_t> reservation_id;
      peer_id source;
   };

   struct pubsub_state {
      struct request {
         peer_id peer;
         std::string id;
         std::uint64_t generation = 0;
      };
      struct outbound_generation {
         std::uint64_t session_id = 0;
         std::uint64_t generation = 0;
         protocol_id protocol;
         std::shared_ptr<forge::asio::gate> write_gate;
         std::shared_ptr<forge::net::p2p::stream> stream;
         bool snapshot_pending = true;
      };

      struct validation {
         enum class status : std::uint8_t {
            claimed,
            in_progress,
            accepted,
            rejected,
            retryable,
            ignored,
         };

         status state = status::claimed;
         std::size_t attempts = 0;
         std::size_t redeliveries = 0;
         std::size_t requests = 0;
         peer_id source;
         std::uint64_t generation = 0;
         std::uint64_t score_generation = 0;
         std::uint64_t cache_epoch = 0;
         std::map<peer_id, std::size_t> retransmissions;
         std::chrono::steady_clock::time_point retry_after{};
         std::chrono::steady_clock::time_point request_after{};
      };

      enum class claim_status : std::uint8_t {
         claimed,
         backpressured,
         duplicate,
         invalid,
      };

      struct claim {
         claim_status status = claim_status::duplicate;
         std::uint64_t generation = 0;
      };

      std::map<std::string, pubsub::handler> handlers;
      std::map<peer_id, std::set<std::string>> peer_topics;
      std::map<peer_id, std::map<std::uint64_t, std::uint64_t>> inbound;
      std::map<std::string, std::set<peer_id>> mesh;
      std::map<std::string, pubsub::message> cache;
      std::deque<std::string> history;
      std::map<std::string, validation> validations;
      std::string retry_cursor;
      std::map<peer_id, pubsub::score> scores;
      struct peer_state {
         std::uint64_t generation = 0;
         bool connected = true;
         std::chrono::steady_clock::time_point retain_until{};
         std::size_t have = 0;
         std::size_t requested = 0;
      };
      struct fanout_state {
         std::set<peer_id> peers;
         std::chrono::steady_clock::time_point last_publish{};
      };
      std::map<peer_id, peer_state> peers;
      std::map<std::string, fanout_state> fanout;
      std::shared_ptr<detail::pubsub_peer_score> scoring;
      std::shared_ptr<detail::pubsub_router> router;
      std::shared_ptr<detail::pubsub_control_queue> controls;
      std::size_t remote_topic_entries = 0;
      std::uint64_t epoch = 0;
      std::uint64_t next_peer_generation = 1;
      std::atomic_uint64_t trace_failures = 0;
      std::atomic_uint64_t application_score_failures = 0;
      std::map<peer_id, outbound_generation> outbound;
      detail::connection_singleflight_registry connection_gates;
      detail::pubsub_outbound_budget outbound_budget;
      detail::pubsub_backoff backoffs;
      std::map<peer_id, std::size_t> active_validations_by_peer;
      std::size_t active_validations = 0;
      std::uint64_t next_validation_generation = 1;
      std::uint64_t next_inbound_generation = 1;
      std::uint64_t next_outbound_generation = 1;
      std::uint64_t next_seqno = 1;
      bool heartbeat_started = false;
   };

   struct relay_reservation_operation {
      std::shared_ptr<cancellation_latch> cancellation;
      bool automatic = false;
      bool canceled = false;
   };

   struct peer_exchange_batch {
      mutable std::mutex mutex;
      std::shared_ptr<detail::lifecycle_wakeup> completed;
      std::shared_ptr<cancellation_latch> cancellation;
      std::size_t remaining_workers = 0;
      bool launches_complete = false;
      bool completion_notified = false;
   };

   struct peer_exchange_operation {
      detail::peer_exchange_cancellation cancellation;
   };

   impl(forge::asio::runtime& runtime_value, node::options options_value);
   forge::asio::runtime& runtime;
   node::options options;
   libp2p_identity_material identity;
   peer_id local;
   resource_manager resources;
   std::shared_ptr<detail::connection_gate> connection_gate;
   std::shared_ptr<detail::dial_scheduler> dial_scheduler;
   direct::registry direct_registry;
   detail::session_teardown teardown;
   detail::lifecycle_tracker lifecycle;
   std::shared_ptr<detail::lifecycle_wakeup> lifecycle_wakeup;
   std::shared_ptr<detail::path_manager> paths;
   detail::identify_service identify_service;
   std::shared_ptr<detail::bootstrap_service> bootstrap;
   forge::asio::gate session_admission_gate;
   forge::asio::gate peer_state_hydration_gate;

   mutable std::mutex mutex;
   peer_store store;
   std::map<protocol_id, std::unique_ptr<detail::dht_profile_state>> dht_profiles;
   std::shared_ptr<detail::dht_routing_refresh> routing_refresh;
   std::shared_ptr<detail::dht_provider_registry> provider_registry;
   std::shared_ptr<detail::topology_manager> topology_manager_value;
   std::shared_ptr<detail::mdns_service> mdns_service_value;
   mutable connection_manager connections{connection_policy_for(options.limits)};
   std::map<protocol_id, node::protocol_handler> handlers;
   std::map<std::uint64_t, std::shared_ptr<session_state>> sessions;
   std::map<std::pair<std::string, std::string>, std::shared_ptr<detail::coordinated_dial>> coordinated_dials;
   std::uint64_t coordinated_generation = 0;
   bool coordinated_admission_closed = false;
   std::shared_ptr<detail::reachability_manager> reachability_manager_value;
   std::shared_ptr<detail::observed_address_manager> observed_addresses;
   std::shared_ptr<detail::host_event_source> host_event_source;
   std::vector<endpoint> confirmed_observed_addresses;
   bool reachability_started = false;
   bool reachability_finished = false;
   bool reachability_identify_dirty = false;
   std::map<std::uint64_t, std::shared_ptr<session_state>> retiring_sessions;
   std::map<std::uint64_t, operation_deadline::stop_token> protocol_open_deadlines;
   std::map<peer_id, relay_reservation_state> inbound_relay_reservations;
   struct relay_service_request {
      peer_id peer;
      std::string ip;
      std::chrono::steady_clock::time_point observed_at;
   };
   std::vector<relay_service_request> relay_service_requests;
   // Counts both source and destination participation independently of grants.
   std::map<peer_id, std::size_t> relay_peer_active;
   std::map<peer_id, relay_reservation_state> outbound_relay_reservations;
   std::map<peer_id, std::shared_ptr<relay_reservation_operation>> relay_reservation_operations;
   std::shared_ptr<detail::autorelay_manager> autorelay_manager_value;
   std::uint64_t autorelay_generation = 1;
   bool autorelay_public = false;
   std::uint64_t autorelay_session_cursor = 0;
   bool autorelay_hints_first = false;
   std::vector<endpoint> published_relay_endpoints;
   struct autonat_nonce {
      std::uint64_t value = 0;
      std::chrono::steady_clock::time_point expires_at;
      std::optional<endpoint> observed;
      std::shared_ptr<forge::asio::notification> changed;
   };
   std::map<peer_id, autonat_nonce> pending_autonat_v2_nonces;
   std::vector<std::pair<peer_id, std::chrono::steady_clock::time_point>> autonat_service_requests;
   std::set<peer_id> autonat_service_active;
   std::size_t autonat_handlers_active = 0;
   std::uint64_t next_reservation_id = 1;
   std::uint64_t next_session_id = 1;
   std::uint64_t next_protocol_open_deadline_id = 1;
   std::uint64_t next_peer_exchange_operation_id = 1;
   pubsub_state pubsub_value;
   detail::peer_exchange_scheduler peer_exchange_value;
   std::map<std::uint64_t, std::shared_ptr<peer_exchange_operation>> peer_exchange_operations;
   mutable identify_push_state identify_push_value;
   node::metrics_snapshot metrics_value;
   std::optional<std::chrono::steady_clock::time_point> stop_requested_at;
   bool stopped = false;
   bool path_shutdown_pending = false;
   bool session_admission_closed = false;
   std::exception_ptr session_shutdown_error;
   bool peer_exchange_admission_closed = false;
   bool peer_state_hydrated = false;

   void initialize_lifecycle();
   void initialize_autorelay();
   void start_autorelay();
   void notify_autorelay_changed() noexcept;
   void stop_autorelay() noexcept;
   boost::asio::awaitable<void> join_autorelay();
   [[nodiscard]] detail::autorelay_manager::snapshot autorelay_snapshot();
   [[nodiscard]] std::vector<endpoint> relay_advertised_endpoints_locked() const;
   void refresh_relay_publication();
   void invalidate_relay_session_locked(std::uint64_t session_id) noexcept;
   void cancel_outbound_relay(const peer_id& peer);
   static boost::asio::awaitable<relay::reservation::info> reserve_autorelay_owned(
       std::shared_ptr<impl> self, detail::autorelay_manager::candidate candidate, std::uint64_t generation,
       std::shared_ptr<cancellation_latch> cancellation);
   void initialize_mdns();
   void stop_mdns() noexcept;
   boost::asio::awaitable<void> join_mdns();
   void initialize_dht_routing_refresh();
   void initialize_dht_provider_registry();
   void initialize_topology_manager();
   void start_topology_manager();
   boost::asio::awaitable<void> async_join_topology_manager();
   [[nodiscard]] bool launch_tracked(std::function<boost::asio::awaitable<void>()> operation) noexcept;
   [[nodiscard]] bool launch_tracked_cleanup(std::function<boost::asio::awaitable<void>()> operation) noexcept;
   void request_lifecycle_stop() noexcept;
   void cancel_coordinated_dials() noexcept;
   boost::asio::awaitable<void> join_coordinated_dials();
   [[nodiscard]] bool has_coordinated_dials() const;
   void request_dial_scheduler_stop() noexcept;
   boost::asio::awaitable<void> async_close_dial_scheduler();
   [[nodiscard]] dialing::black_hole_status dial_black_hole_status() const;
   boost::asio::awaitable<lifecycle_status> async_start_lifecycle();
   boost::asio::awaitable<void> async_hydrate_peer_state();
   void listen(forge::net::p2p::endpoint endpoint);
   [[nodiscard]] bool private_network_enabled() const noexcept;
   void require_private_protocol_allowed(const protocol_id& protocol) const;
   void require_private_direct_tcp(const forge::net::p2p::endpoint& endpoint, std::string_view operation) const;

   void invalidate_pubsub_outbound_locked(const peer_id& peer,
                                          std::optional<std::uint64_t> owner_session_id = std::nullopt,
                                          const std::shared_ptr<forge::asio::gate>& owner_write_gate = {},
                                          const std::shared_ptr<forge::net::p2p::stream>& owner_stream = {}) noexcept;
   void forget_pubsub_peer_locked(const peer_id& peer);
   void disconnect_pubsub_peer_locked(const peer_id& peer, std::chrono::steady_clock::time_point now);
   void disconnect_pubsub_sessions_locked(std::span<const std::uint64_t> selected,
                                          std::chrono::steady_clock::time_point now);
   void finish_pubsub_inbound(const peer_id& peer, std::uint64_t generation);
   void clear_pubsub_outbound_locked();

   void reserve_pubsub_outbound_bytes(const peer_id& peer, std::size_t bytes);

   void release_pubsub_outbound_bytes(const peer_id& peer, std::size_t bytes) noexcept;

   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints_for_control() const;
   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_hole_punch_endpoints() const;
   void initialize_reachability();
   void start_reachability();
   void stop_reachability() noexcept;
   void finish_reachability() noexcept;
   void close_reachability_results_locked() noexcept;
   void sync_reachability_addresses_locked();
   void refresh_reachability_locked();
   void invalidate_reachability_locked() noexcept;
   void remove_address_observation_locked(std::uint64_t session) noexcept;
   boost::asio::awaitable<void> join_reachability();
   void observe_address(const std::shared_ptr<session_state>& session, const identify::document& document);
   void notify_reachability_changed() noexcept;
   void publish_host_state(host_event state);
   [[nodiscard]] host_event current_host_state() const;
   [[nodiscard]] forge::net::p2p::diagnostics::reachability_state reachability_diagnostics() const;
   [[nodiscard]] host_event_subscription subscribe_host_events() const;
   [[nodiscard]] std::shared_ptr<session_state> reachability_session_locked(const peer_id& peer,
       std::optional<protocol_id> protocol = std::nullopt) const;
   [[nodiscard]] std::vector<std::shared_ptr<session_state>> reachability_sessions_locked() const;
   static boost::asio::awaitable<reachability::state> probe_reachability_owned(std::shared_ptr<impl> self, peer_id observer);
   static boost::asio::awaitable<reachability::result> exchange_reachability_owned(std::shared_ptr<impl> self,
       peer_id observer, endpoint remote, bool v2, std::vector<endpoint> candidates,
       std::shared_ptr<cancellation_latch> cancellation, std::uint64_t session_id);
   static boost::asio::awaitable<void> ping_reachability_owned(std::shared_ptr<impl> self, peer_id peer,
       std::shared_ptr<cancellation_latch> cancellation);
   static boost::asio::awaitable<opened_direct_stream> open_reachability_stream_owned(std::shared_ptr<impl> self,
       peer_id peer, protocol_id protocol, std::chrono::milliseconds timeout,
       std::shared_ptr<cancellation_latch> cancellation, std::uint64_t session_id);
   static boost::asio::awaitable<void> select_reachability_stream_owned(std::shared_ptr<impl> self,
       std::shared_ptr<session_state> session, protocol_id protocol, detail::stream_admission_handler admission,
       opened_direct_stream& opened);
   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints_for_control_locked() const;

   [[nodiscard]] identify::document
   local_identify_document(std::optional<forge::net::p2p::endpoint> observed_endpoint = std::nullopt) const;

   void validate_local_identify_document() const;

   [[nodiscard]] identify_snapshot local_identify_snapshot() const;

   void register_protocol_handler(protocol_id protocol, node::protocol_handler handler);

   [[nodiscard]] bool unregister_protocol_handler(const protocol_id& protocol);

   void set_advertised_endpoints(std::vector<forge::net::p2p::endpoint> endpoints);

   void notify_listen_endpoints_changed();

   void learn_from_identify(const std::shared_ptr<session_state>& session, const identify::document& document,
                            bool received_push = false);

   boost::asio::awaitable<void> identify_session(const std::shared_ptr<session_state>& session);

   void launch_identify(const std::shared_ptr<session_state>& session);

   [[nodiscard]] bool advance_identify_generation_locked() noexcept;

   [[nodiscard]] bool schedule_identify_push_locked() noexcept;

   void launch_identify_pushes();

   boost::asio::awaitable<void> run_identify_pushes();

   boost::asio::awaitable<void> send_identify_push(const std::shared_ptr<session_state>& session,
                                                   std::uint64_t generation,
                                                   std::shared_ptr<const identify::document> document);

   boost::asio::awaitable<std::optional<identify::document>>
   identify_peer_for_discovery(const peer_id& peer, discovery::source source, std::chrono::milliseconds timeout);

   boost::asio::awaitable<void> remember_session(std::shared_ptr<session_state> session,
                                                 connection_manager::direction direction,
                                                 std::function<void()> before_publish = {});

   void refresh_connection_scores();
   [[nodiscard]] connection_manager::snapshot topology_sessions() const;
   [[nodiscard]] connection_manager::peer_prune_plan
   topology_peer_prune_plan(std::size_t target_peers, std::size_t max_victims,
                            std::chrono::steady_clock::time_point now);
   boost::asio::awaitable<void> async_close_topology_sessions(std::vector<std::uint64_t> session_ids);
   boost::asio::awaitable<bool>
   async_dial_topology_candidate(discovery::result candidate, std::shared_ptr<cancellation_latch> cancellation,
                                 detail::direct_dial_provenance provenance = detail::direct_dial_provenance::persistent);
   boost::asio::awaitable<std::vector<discovery::result>>
   async_collect_topology_discovery(std::shared_ptr<cancellation_latch> cancellation);
   boost::asio::awaitable<void>
   async_collect_topology_dht_worker(const std::shared_ptr<topology_dht_batch>& batch,
                                     std::chrono::system_clock::time_point expires_at,
                                     std::shared_ptr<detail::worker_terminal_owner> terminal);
   [[nodiscard]] detail::topology_manager::callbacks::rendezvous_local_record topology_rendezvous_local_record() const;
   boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_register_result>
   async_register_topology_rendezvous(std::size_t point_index, std::string namespace_name,
                                      std::vector<std::uint8_t> signed_peer_record,
                                      std::shared_ptr<cancellation_latch> cancellation);
   boost::asio::awaitable<detail::topology_manager::callbacks::rendezvous_discover_result>
   async_discover_topology_rendezvous(std::size_t point_index, std::string namespace_name, std::size_t limit,
                                      std::vector<std::uint8_t> cookie,
                                      std::shared_ptr<cancellation_latch> cancellation);
   boost::asio::awaitable<void> async_unregister_topology_rendezvous(std::size_t point_index,
                                                                     std::string namespace_name);
   boost::asio::awaitable<std::shared_ptr<session_state>>
   ensure_topology_rendezvous_session(std::size_t point_index, bool allow_dial,
                                      std::shared_ptr<cancellation_latch> cancellation = {});
   boost::asio::awaitable<rendezvous::message>
   exchange_topology_rendezvous(const std::shared_ptr<session_state>& session, rendezvous::message request,
                                std::string_view operation, std::shared_ptr<cancellation_latch> cancellation);
   boost::asio::awaitable<std::vector<discovery::result>>
   async_collect_topology_peer_exchange(std::shared_ptr<cancellation_latch> cancellation,
                                        std::size_t max_parallel_queries);

   void launch_pruned_session_teardown(const std::shared_ptr<session_state>& session) noexcept;
   [[nodiscard]] std::shared_ptr<session_state>
   retire_session_locked(const std::shared_ptr<session_state>& session, bool track_close) noexcept;
   void request_cancel_session(const std::shared_ptr<session_state>& session) noexcept;
   boost::asio::awaitable<void> async_retire_session(const std::shared_ptr<session_state>& session,
                                                      bool allow_untracked);
   boost::asio::awaitable<void> async_retire_sessions_gracefully();
   void forget_retired_session(const std::shared_ptr<session_state>& session) noexcept;

   void forget_session(const peer_id& peer);

   void forget_session(const std::shared_ptr<session_state>& session);

   [[nodiscard]] std::shared_ptr<session_state> session_for(const peer_id& peer) const;
   [[nodiscard]] std::shared_ptr<session_state> session_for_locked(const peer_id& peer) const;
   [[nodiscard]] std::shared_ptr<session_state>
   session_for_path(const peer_id& peer, path::kind kind, std::optional<peer_id> relay_peer = std::nullopt) const;
   [[nodiscard]] std::shared_ptr<session_state> session_for_path_locked(const peer_id& peer, path::kind kind,
                                                                        const std::optional<peer_id>& relay_peer) const;
   [[nodiscard]] node::session_info session_info_for(const std::shared_ptr<session_state>& session) const;

   [[nodiscard]] std::optional<node::protocol_handler> handler_for(const protocol_id& protocol) const;

   [[nodiscard]] std::vector<protocol_id> supported_protocols_locked() const;

   [[nodiscard]] std::vector<protocol_id> supported_protocols() const;

   boost::asio::awaitable<rendezvous::message> exchange_rendezvous(const peer_id& peer, rendezvous::message request,
                                                                   std::string_view operation);

   void remember_autonat_v2_nonce(const peer_id& peer, std::uint64_t nonce);

   void forget_autonat_v2_nonce(const peer_id& peer, std::uint64_t nonce);

   [[nodiscard]] bool consume_autonat_v2_nonce(std::uint64_t nonce, const endpoint& local_endpoint);
   [[nodiscard]] std::optional<endpoint> autonat_v2_observation(const peer_id& observer,
                                                               std::uint64_t nonce) const;
   boost::asio::awaitable<std::optional<endpoint>> async_autonat_v2_observation(
       peer_id observer, std::uint64_t nonce, std::shared_ptr<cancellation_latch> cancellation = {});
   static boost::asio::awaitable<std::optional<endpoint>> wait_autonat_v2_observation(
       std::shared_ptr<impl> self, peer_id observer, std::uint64_t nonce,
       std::shared_ptr<cancellation_latch> cancellation);

   void increment_opened_protocol();

   void increment_protocol_accepted();

   void increment_protocol_rejected();

   void increment_peer_exchange();

   void increment_reachability_check(reachability::state state);

   void cleanup_expired_relay_reservations_locked();

   [[nodiscard]] bool has_outbound_relay_reservation(const peer_id& relay_peer);

   [[nodiscard]] bool has_fresh_outbound_relay_reservation(const peer_id& relay_peer,
                                                           std::chrono::milliseconds refresh_margin);

   [[nodiscard]] std::vector<peer_id> fresh_outbound_relay_candidates(std::size_t limit,
                                                                      std::chrono::milliseconds refresh_margin);

   void remember_relay_reservation_in_store(const relay::reservation::info& info);

   [[nodiscard]] bool remember_inbound_relay_reservation(const std::shared_ptr<session_state>& session,
                                                        relay::reservation::options request);
   [[nodiscard]] bool admit_relay_service_locked(const session_state& session);

   bool cancel_inbound_relay_reservation(const peer_id& owner, std::uint64_t reservation_id);

   [[nodiscard]] std::optional<relay_admission> begin_relay(const peer_id& owner, const peer_id& source,
                                                          relay::status& status);

   [[nodiscard]] std::uint64_t relay_byte_limit(const peer_id& owner);

   void finish_relay(const peer_id& owner, std::optional<std::uint64_t> reservation_id, const peer_id& source);

   void erase_inbound_relay_reservation_locked(const peer_id& owner) noexcept;

   void record_relay_bytes(std::uint64_t bytes) noexcept;

   void record_path_open(path::kind kind);

   void record_path_attempt(path::kind kind);

   void record_hole_punch_result(hole_punch::status status);

   void record_direct_failure(const peer_id& peer);

   void increment_direct_failure();
   void record_direct_session_failure(const std::shared_ptr<session_state>& session);

   [[nodiscard]] std::chrono::system_clock::time_point
   endpoint_backoff_until(const peer_id& peer, const forge::net::p2p::endpoint& endpoint, path::kind kind) const;

   [[nodiscard]] std::chrono::system_clock::time_point
   endpoint_backoff_until(const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind) const;

   void record_relay_failure();

   void increment_dht_query();

   void increment_dht_response();

   [[nodiscard]] detail::dht_profile_state& dht_profile(const protocol_id& protocol);
   [[nodiscard]] const detail::dht_profile_state& dht_profile(const protocol_id& protocol) const;
   boost::asio::awaitable<dht::query_result> async_find_dht_peer(protocol_id protocol, peer_id peer,
                                                                 dht::query_options options,
                                                                 std::optional<std::size_t> alpha_limit = std::nullopt,
                                                                 std::shared_ptr<cancellation_latch> cancellation = {});
   boost::asio::awaitable<bool> async_refresh_dht_routing(protocol_id protocol, dht::key target,
                                                          std::chrono::milliseconds timeout,
                                                          std::shared_ptr<cancellation_latch> cancellation = {});
   void notify_dht_routing_refresh() noexcept;

   void increment_rendezvous_registration();

   void increment_rendezvous_discover();

   void increment_pubsub_published();

   void increment_pubsub_received();

   void increment_pubsub_delivered();

   void increment_pubsub_duplicate();

   [[nodiscard]] bool increment_pubsub_invalid(const std::shared_ptr<session_state>& session,
       const std::optional<pubsub::topic>& subject, bool protocol_rejected);


   void increment_pubsub_control();

   [[nodiscard]] std::vector<std::uint8_t> next_pubsub_seqno();

   [[nodiscard]] pubsub::snapshot pubsub_snapshot() const;
   [[nodiscard]] pubsub::score_snapshot pubsub_scores() const;
   void initialize_pubsub();
   [[nodiscard]] bool connect_pubsub_peer_locked(const peer_id& peer);
   [[nodiscard]] bool pubsub_session_live_locked(const std::shared_ptr<session_state>& session) const noexcept;
   void forget_pubsub_endpoint_locked(const session_state& session);
   void sample_pubsub_application_scores(const std::optional<peer_id>& peer = std::nullopt);
   template<typename Builder>
   void trace_pubsub(Builder&& build) noexcept {
      if (!options.limits.pubsub.tracer) { return; }
      try {
         const auto event = build();
         options.limits.pubsub.tracer(event);
      } catch (...) {
         pubsub_value.trace_failures.fetch_add(1, std::memory_order_relaxed);
      }
   }
   [[nodiscard]] double pubsub_score_locked(const peer_id& peer);
   [[nodiscard]] bool pubsub_peer_live_locked(const peer_id& peer) const;
   [[nodiscard]] bool pubsub_peer_outbound_locked(const peer_id& peer) const;
   void graft_pubsub_peer_locked(const std::string& topic, const peer_id& peer);
   void prune_pubsub_peer_locked(const std::string& topic, const peer_id& peer);
   struct pubsub_control_change {
      struct action {
         peer_id peer;
         std::string topic;
         detail::pubsub_control_queue::kind operation;
      };
      std::vector<action> actions;
      detail::pubsub_control_queue::prepared queued;
      detail::pubsub_backoff::prepared_local backoff;
      std::map<std::string, std::set<peer_id>> mesh;
   };
   [[nodiscard]] std::vector<pubsub::peer_info> prepare_pubsub_prune_peers();
   [[nodiscard]] detail::pubsub_control_queue::command make_pubsub_control_locked(
       const peer_id& peer, const pubsub::topic& subject, detail::pubsub_control_queue::kind operation,
       std::chrono::seconds backoff, std::span<const pubsub::peer_info> px);
   [[nodiscard]] std::optional<pubsub_control_change> prepare_pubsub_controls_locked(
       std::vector<detail::pubsub_control_queue::command> commands, std::chrono::steady_clock::time_point now);
   void commit_pubsub_controls_locked(pubsub_control_change change, std::chrono::steady_clock::time_point now) noexcept;
   [[nodiscard]] bool admit_pubsub_control_locked(detail::pubsub_control_queue::command command,
                                                 std::chrono::steady_clock::time_point now);
   struct pubsub_control_dispatch {
      std::shared_ptr<impl> owner;
      std::shared_ptr<const detail::pubsub_control_queue::batch> lease;
      std::shared_ptr<session_state> origin;
      std::optional<pubsub_state::request> request;
      pubsub_control_dispatch(std::shared_ptr<impl> owner,
                              std::shared_ptr<const detail::pubsub_control_queue::batch> lease,
                              std::shared_ptr<session_state> origin = {});
      ~pubsub_control_dispatch() noexcept;
   };
   [[nodiscard]] bool validate_pubsub_control_locked(const detail::pubsub_control_queue::batch& batch,
                                                     bool check_intents = true) noexcept;
   void flush_pubsub_controls(std::optional<peer_id> peer = std::nullopt,
                              std::map<peer_id, pubsub::control> ephemeral = {},
                              std::shared_ptr<session_state> origin = {});
   boost::asio::awaitable<void> run_pubsub_control_dispatch(std::shared_ptr<pubsub_control_dispatch> dispatch,
                                                           pubsub::rpc ephemeral);
   [[nodiscard]] std::vector<peer_id> pubsub_publish_peers(const std::string& topic);
   [[nodiscard]] std::vector<peer_id> pubsub_forward_peers(const std::string& topic,
                                                         const peer_id& sender, const std::optional<peer_id>& author);
   [[nodiscard]] std::optional<pubsub_state::request>
   stage_pubsub_request_locked(const peer_id& peer, const std::vector<std::vector<std::uint8_t>>& ids);
   void finish_pubsub_request(const pubsub_state::request& request, bool sent,
                              std::shared_ptr<session_state> origin = {}) noexcept;
   boost::asio::awaitable<void> handle_pubsub_control(std::shared_ptr<session_state> session, const pubsub::control& value,
                                                    const protocol_id& protocol);

   [[nodiscard]] std::vector<pubsub::subscription> local_pubsub_subscriptions() const;

   [[nodiscard]] std::vector<peer_id> pubsub_candidate_peers(const std::string& topic_value,
                                                             std::optional<peer_id> except = std::nullopt) const;

   boost::asio::awaitable<bool> send_pubsub_rpc(const peer_id& peer, pubsub::rpc value,
      std::optional<std::uint64_t>& send_generation,
      std::shared_ptr<const detail::pubsub_control_queue::batch> control = {}, bool check_intents = true,
      std::shared_ptr<session_state> origin = {});
   void record_pubsub_send_failure(const peer_id& peer, const forge::exceptions::base& error,
      std::optional<std::uint64_t> expected_generation, std::shared_ptr<session_state> origin = {});

   boost::asio::awaitable<std::shared_ptr<session_state>> ensure_pubsub_direct_session(
       const peer_id& peer, std::shared_ptr<session_state> origin = {});

   boost::asio::awaitable<void> announce_pubsub_subscriptions(const peer_id& peer);

   void finish_pubsub_validation(const peer_id& peer);

   [[nodiscard]] pubsub_state::claim claim_pubsub_message(const peer_id& peer, const std::string& key,
      const pubsub::message& value, bool requires_validation, const std::shared_ptr<session_state>& session = {});

   [[nodiscard]] bool complete_pubsub_message(const std::string& key, std::uint64_t generation,
      pubsub::validation_result result, const std::shared_ptr<session_state>& session = {});

   void defer_pubsub_message(const std::string& key, std::uint64_t generation,
      const std::shared_ptr<session_state>& session = {});

   [[nodiscard]] bool should_request_pubsub_message_locked(const std::string& key, const peer_id& source,
                                                           std::chrono::steady_clock::time_point now);

   [[nodiscard]] bool record_pubsub_subscription_locked(const peer_id& peer, const std::string& topic);

   [[nodiscard]] bool can_serve_pubsub_message_locked(const std::string& key) const;

   void remember_local_pubsub_message_locked(const std::string& key, pubsub::message value);

   void prune_pubsub_cache_locked();


   void launch_pubsub_heartbeat();

   boost::asio::awaitable<void> pubsub_heartbeat_once();

   boost::asio::awaitable<detail::direct_attempt>
   connect_direct_attempt(forge::net::p2p::endpoint endpoint, node::connect_options connect_options_value,
                          std::shared_ptr<cancellation_latch> cancellation = {},
                          direct::tcp_transport_progress_handler tcp_transport_progress = {});

   boost::asio::awaitable<void> async_close_direct_attempt(detail::direct_attempt& attempt);

   boost::asio::awaitable<node::session_info>
   connect_coordinated(endpoint remote, node::coordinated_connect_options value,
                       std::shared_ptr<cancellation_latch> parent = {}, bool upgrade_only = false);
   boost::asio::awaitable<void> run_coordinated_dial(std::shared_ptr<detail::coordinated_dial> operation);

   boost::asio::awaitable<void> async_discard_session(const std::shared_ptr<session_state>& session);

   // The suspended caller retains ownership across coroutine-frame allocation.
   boost::asio::awaitable<std::shared_ptr<session_state>>
   commit_direct_attempt(detail::direct_attempt&& attempt, std::vector<forge::multiformats::multiaddr> roots,
                         std::chrono::steady_clock::time_point deadline,
                         std::shared_ptr<cancellation_latch> cancellation, bool upgrade_only = false,
                         connection_manager::direction direction = connection_manager::direction::outbound,
                         bool announce = true);

   boost::asio::awaitable<std::shared_ptr<session_state>>
   connect_direct(forge::net::p2p::endpoint endpoint, node::connect_options connect_options_value,
                  std::shared_ptr<cancellation_latch> cancellation = {});

   boost::asio::awaitable<std::shared_ptr<session_state>>
   connect_direct(std::vector<forge::multiformats::multiaddr> roots, node::connect_options connect_options_value,
                  std::shared_ptr<cancellation_latch> cancellation = {});

   boost::asio::awaitable<std::shared_ptr<session_state>>
   connect_direct(std::vector<detail::direct_dial_root> roots, node::connect_options connect_options_value,
                  std::shared_ptr<cancellation_latch> cancellation = {});

   static boost::asio::awaitable<node::session_info>
   async_connect_owned(std::shared_ptr<impl> self, forge::multiformats::multiaddr address,
                       node::connect_options value);

   boost::asio::awaitable<std::shared_ptr<session_state>> ensure_direct_session(
       const peer_id& peer, std::chrono::milliseconds timeout = node::connect_options{}.timeout,
       std::size_t max_direct_endpoints = node::connect_options{}.max_direct_endpoints,
       std::chrono::milliseconds direct_attempt_timeout = node::connect_options{}.direct_attempt_timeout,
       std::shared_ptr<cancellation_latch> cancellation = {});

   boost::asio::awaitable<forge::net::p2p::stream>
   open_protocol_on_direct_session(const peer_id& peer, const protocol_id& protocol,
                                   std::shared_ptr<session_state> session, std::chrono::milliseconds timeout,
                                   std::shared_ptr<cancellation_latch> cancellation = {},
                                   detail::stream_open_phase* phase = nullptr);

   boost::asio::awaitable<forge::net::p2p::stream>
   open_protocol_direct(const peer_id& peer, const protocol_id& protocol, std::chrono::milliseconds timeout,
                        std::size_t max_direct_endpoints = node::open_options{}.max_direct_endpoints,
                        std::chrono::milliseconds direct_attempt_timeout = node::open_options{}.direct_attempt_timeout);

   boost::asio::awaitable<opened_direct_stream> open_protocol_direct_with_context(
       const peer_id& peer, const protocol_id& protocol, std::chrono::milliseconds timeout,
       std::size_t max_direct_endpoints = node::open_options{}.max_direct_endpoints,
       std::chrono::milliseconds direct_attempt_timeout = node::open_options{}.direct_attempt_timeout,
       std::shared_ptr<cancellation_latch> cancellation = {});

   static boost::asio::awaitable<opened_direct_stream> open_protocol_direct_owned(
       std::shared_ptr<impl> self, std::shared_ptr<session_state> cached,
       peer_id peer, protocol_id protocol, std::chrono::steady_clock::time_point started,
       std::chrono::milliseconds timeout, std::size_t max_direct_endpoints,
       std::chrono::milliseconds direct_attempt_timeout,
       std::shared_ptr<cancellation_latch> cancellation);

   boost::asio::awaitable<dht_exchange_result> exchange_dht(const protocol_id& profile, const peer_id& peer,
                                                            dht::message request, std::chrono::milliseconds timeout,
                                                            std::shared_ptr<cancellation_latch> cancellation = {});
   boost::asio::awaitable<void> send_dht(const protocol_id& profile, const peer_id& peer, dht::message request,
                                         std::chrono::milliseconds timeout,
                                         std::shared_ptr<cancellation_latch> cancellation = {});

   boost::asio::awaitable<relay::reservation::info>
   request_relay_reservation(const peer_id& relay_peer, relay::reservation::options reservation_options,
                              std::chrono::milliseconds timeout,
                              std::shared_ptr<cancellation_latch> cancellation = {},
                              bool automatic = false, std::uint64_t generation = 0,
                              std::uint64_t session_id = 0);

   boost::asio::awaitable<void> ensure_relay_reservation(const peer_id& relay_peer, std::chrono::milliseconds timeout);

   boost::asio::awaitable<std::vector<relay::reservation::info>>
   refresh_relay_candidates(std::optional<peer_id> target, std::chrono::milliseconds timeout);

   boost::asio::awaitable<upgraded_session>
   open_relay_yamux(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout,
                    std::function<void(const peer_id&)> authenticated_admission);

   boost::asio::awaitable<std::shared_ptr<session_state>>
   ensure_relay_session(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout);

   boost::asio::awaitable<forge::net::p2p::stream> open_protocol_via_relay(const peer_id& peer,
                                                                           const protocol_id& protocol,
                                                                           const peer_id& relay_peer,
                                                                           std::chrono::milliseconds timeout);

   boost::asio::awaitable<void> request_peer_exchange(const peer_id& peer);
   void launch_peer_exchange();
   boost::asio::awaitable<void> await_peer_exchange_claim(detail::peer_exchange_scheduler::claim& claim,
                                                          std::shared_ptr<detail::worker_terminal_owner> terminal = {});
   boost::asio::awaitable<void> run_peer_exchange(detail::peer_exchange_scheduler::claim& claim,
                                                  std::shared_ptr<detail::worker_terminal_owner> terminal = {});
   [[nodiscard]] std::vector<detail::peer_exchange_scheduler::session> peer_exchange_sessions_locked() const;

   void launch_accept_loop(forge::net::p2p::endpoint local_endpoint);

   boost::asio::awaitable<void> handle_inbound_connection(direct::connection connection,
                                                          resource_manager::session_reservation reservation);

   boost::asio::awaitable<forge::net::p2p::stream> open_session_stream(const std::shared_ptr<session_state>& session,
                                                                       const protocol_id& protocol, bool relay = false,
                                                                       detail::stream_admission_handler admitted = {},
                                                                       detail::stream_open_phase* phase = nullptr);

   boost::asio::awaitable<forge::net::p2p::stream>
   open_yamux_stream(const peer_id& peer, const std::shared_ptr<forge::net::yamux::session>& yamux,
                     const protocol_id& protocol, bool relay = true);

   boost::asio::awaitable<admitted_stream> accept_resource_stream(const peer_id& peer,
                                                                  forge::net::transport::stream stream,
                                                                  resource_manager::stream_reservation reservation);

   boost::asio::awaitable<bool> dispatch_registered_handler(const std::shared_ptr<session_state>& session,
                                                            admitted_stream& admitted);

   bool launch_session_accept_loop(std::shared_ptr<session_state> session);

   boost::asio::awaitable<void> handle_incoming_stream(std::shared_ptr<session_state> session,
                                                       forge::net::transport::stream raw,
                                                       resource_manager::stream_reservation reservation);

   boost::asio::awaitable<void> handle_ping(forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_identify(std::shared_ptr<session_state> session, forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_identify_push(std::shared_ptr<session_state> session,
                                                     forge::net::p2p::stream stream,
                                                     std::shared_ptr<detail::resource_stream> resource);

   boost::asio::awaitable<void> handle_autonat_v2_dial_back(std::shared_ptr<session_state> session,
                                                            forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_autonat_v2_dial_request(std::shared_ptr<session_state> session,
                                                               forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_autonat_v1(std::shared_ptr<session_state> session,
                                                 forge::net::p2p::stream stream);

   struct autonat_operation;
   enum class autonat_protocol { v1, v2_request, v2_dial_back };
   boost::asio::awaitable<void> handle_autonat(std::shared_ptr<session_state> session,
                                              forge::net::p2p::stream stream, autonat_protocol protocol);
   boost::asio::awaitable<void> run_autonat(std::shared_ptr<autonat_operation> operation,
                                            std::shared_ptr<detail::worker_terminal_owner> terminal);
   boost::asio::awaitable<void> serve_autonat_v1(std::shared_ptr<autonat_operation> operation);
   boost::asio::awaitable<void> serve_autonat_v2(std::shared_ptr<autonat_operation> operation);
   boost::asio::awaitable<reachability::v2::dial_status>
   probe_autonat(std::shared_ptr<autonat_operation> operation, endpoint target,
                  std::optional<std::uint64_t> nonce);

   boost::asio::awaitable<void> handle_relayed_yamux_stream(std::shared_ptr<session_state> session,
                                                            forge::net::transport::stream stream,
                                                            resource_manager::stream_reservation reservation);

   boost::asio::awaitable<void> handle_relay_stop(std::shared_ptr<session_state> session,
                                                  forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_relay_hop(std::shared_ptr<session_state> session,
                                                 forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_dcutr(std::shared_ptr<session_state> session, forge::net::p2p::stream stream,
                                            std::shared_ptr<detail::resource_stream> resource = {});

   [[nodiscard]] bool path_session_eligible(const std::shared_ptr<session_state>& session,
                                           detail::path_manager::role side) const;
   [[nodiscard]] std::shared_ptr<session_state> authenticated_direct_session(const peer_id& peer) const;
   [[nodiscard]] detail::path_manager::claim request_path_upgrade(const std::shared_ptr<session_state>& session,
                                                                 std::chrono::steady_clock::time_point deadline);
   boost::asio::awaitable<hole_punch::status> run_path_upgrade(std::shared_ptr<session_state> session,
                                                             std::shared_ptr<detail::path_manager::operation> owner);
   boost::asio::awaitable<bool> wait_path_identify(const std::shared_ptr<session_state>& session,
       const std::shared_ptr<detail::path_manager::operation>& owner,
       const std::shared_ptr<detail::path_manager::exchange>& ticket = {});
   boost::asio::awaitable<bool> dial_coordinated_path(
       std::vector<endpoint> candidates, const std::shared_ptr<detail::path_manager::operation>& owner,
       const std::shared_ptr<detail::path_manager::exchange>& ticket);
   boost::asio::awaitable<void> dial_path_candidate(endpoint candidate,
       std::shared_ptr<detail::path_manager::operation> owner, std::shared_ptr<detail::path_manager::exchange> ticket,
       std::shared_ptr<path_dial_batch> batch,
       std::chrono::steady_clock::time_point deadline, std::optional<endpoint> local_source);

   boost::asio::awaitable<void> handle_dht(std::shared_ptr<session_state> session, protocol_id profile,
                                           forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_rendezvous(std::shared_ptr<session_state> session,
                                                  forge::net::p2p::stream stream);

   boost::asio::awaitable<void> handle_pubsub(std::shared_ptr<session_state> session, forge::net::p2p::stream stream,
                                           protocol_id protocol);
   boost::asio::awaitable<void> handle_pubsub_stream(std::shared_ptr<session_state> session,
                                                     forge::net::p2p::stream stream, protocol_id protocol,
                                                     std::uint64_t generation);

   boost::asio::awaitable<bool> wait_for_direct_session(const peer_id& peer, std::chrono::milliseconds timeout);

   boost::asio::awaitable<bool> run_dcutr_initiator(
       const std::shared_ptr<session_state>& session,
       const std::shared_ptr<detail::path_manager::operation>& owner,
       const std::shared_ptr<detail::path_manager::exchange>& ticket);

   boost::asio::awaitable<void> handle_peer_exchange(forge::net::p2p::stream stream, std::uint64_t request_id,
                                                     std::uint64_t remote_receive_limit);

   void launch_relay_pumps(peer_id owner, forge::net::p2p::stream left, forge::net::p2p::stream right,
                           relay_admission admission);

   boost::asio::awaitable<hole_punch::status> attempt_hole_punch(peer_id peer, std::optional<peer_id> relay_peer,
                                                                 std::chrono::milliseconds timeout);
};

} // namespace forge::net::p2p
