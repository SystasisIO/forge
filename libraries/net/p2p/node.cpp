module;

#include "details/rendezvous_time.hxx"

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.symmetric.chacha20_poly1305;
import forge.crypto.pki.der;
import forge.crypto.asymmetric.ed25519;
import forge.crypto.digest.hmac;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.hole_punch;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.exceptions;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.crypto.core.random;
import forge.crypto.asymmetric.rsa;
import forge.crypto.digest.sha256;
import forge.crypto.asymmetric.x25519;
import forge.multiformats.types;
import forge.multiformats.multiaddr;
import forge.multiformats.varint;
import forge.multiformats.exceptions;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"

#include "details/peer_failure.hxx"
#include "details/protocol_capabilities.hxx"
#include "details/session_lifecycle.hxx"

namespace forge::net::p2p {

host_event node::reachability_status() const { return impl_->current_host_state(); }
host_event_subscription node::host_events() const { return impl_->subscribe_host_events(); }

void remember_dht_peer(peer_store& store, const protocol_id& protocol, dht::routing_table& routing,
                       std::chrono::milliseconds refresh_interval, const dht::peer& value,
                       dht::routing_admission admission) {
   store.upsert_routing_peer(protocol, value, discovery::source::dht,
                             detail::saturating_topology_expiry(std::chrono::system_clock::now(), refresh_interval));
   routing.upsert(value, admission);
}

void mark_dht_routing_failure(dht::routing_table& routing, const peer_id& peer) {
   routing.mark_failure(peer);
}

[[nodiscard]] bool remote_peer_attributable_failure(std::mutex& mutex, const bool& stopped,
                                                    const forge::exceptions::base& error) {
   auto node_stopped = false;
   {
      auto lock = std::scoped_lock{mutex};
      node_stopped = stopped;
   }
   return detail::remote_peer_attributable_failure(p2p_code(error), node_stopped);
}

[[nodiscard]] host_addresses::learning_context third_party_discovery_context() {
   return host_addresses::learning_context{.source = host_addresses::source_kind::third_party};
}

[[nodiscard]] dht::peer sanitize_discovered_peer(dht::peer value, host_addresses::learning_context context) {
   value.endpoints = host_addresses::sanitize_discovered_addresses(std::move(value.endpoints), value.id, context);
   return value;
}

[[nodiscard]] bool has_usable_endpoint(const dht::peer& value) noexcept {
   return !value.endpoints.empty();
}

[[nodiscard]] std::vector<forge::multiformats::multiaddr>
endpoints_from_registration(const rendezvous::registration& registration) {
   if (registration.signed_peer_record.empty()) {
      return registration.endpoints;
   }
   try {
      const auto record = rendezvous::codec::open_peer_record(signed_envelope::decode(registration.signed_peer_record),
                                                              registration.peer);
      return record.endpoints;
   } catch (const forge::exceptions::base&) {
      return {};
   }
}

[[nodiscard]] std::optional<rendezvous::registration>
sanitize_discovered_registration(rendezvous::registration registration, host_addresses::learning_context context) {
   const auto original_endpoints = endpoints_from_registration(registration);
   if (original_endpoints.empty()) {
      if (!registration.signed_peer_record.empty()) {
         return std::nullopt;
      }
      return registration;
   }
   auto sanitized =
       host_addresses::sanitize_discovered_addresses(original_endpoints, registration.peer, std::move(context));
   if (sanitized.empty()) {
      return std::nullopt;
   }
   if (registration.signed_peer_record.empty() || sanitized.size() != original_endpoints.size()) {
      registration.signed_peer_record.clear();
   } else {
      for (auto index = std::size_t{0}; index < sanitized.size(); ++index) {
         if (sanitized[index].to_string() != original_endpoints[index].to_string()) {
            registration.signed_peer_record.clear();
            break;
         }
      }
   }
   registration.endpoints = std::move(sanitized);
   return registration;
}

namespace {

[[nodiscard]] std::chrono::milliseconds elapsed_since(std::chrono::steady_clock::time_point now,
                                                      std::chrono::steady_clock::time_point started) {
   if (started == std::chrono::steady_clock::time_point{} || now <= started) {
      return std::chrono::milliseconds{0};
   }
   return std::chrono::duration_cast<std::chrono::milliseconds>(now - started);
}

[[nodiscard]] diagnostics::session_direction diagnostics_direction(connection_manager::direction value) noexcept {
   return value == connection_manager::direction::inbound ? diagnostics::session_direction::inbound
                                                          : diagnostics::session_direction::outbound;
}

[[nodiscard]] std::vector<diagnostics::endpoint_record>
diagnostics_endpoints(std::span<const peer_store::endpoint_record> records, std::size_t limit) {
   auto out = std::vector<diagnostics::endpoint_record>{};
   out.reserve(std::min(records.size(), limit));
   for (const auto& record : records) {
      if (out.size() >= limit) {
         break;
      }
      out.push_back(diagnostics::endpoint_record{
          .address = record.address,
          .kind = record.kind,
          .relay_peer = record.relay_peer,
          .successes = record.successes,
          .failures = record.failures,
          .last_latency = record.last_latency,
          .backoff_until = record.backoff_until,
          .score = record.score,
      });
   }
   return out;
}

[[nodiscard]] std::vector<diagnostics::relay_reservation>
diagnostics_relays(std::span<const peer_store::relay_record> records, std::size_t limit, std::size_t endpoint_limit) {
   auto out = std::vector<diagnostics::relay_reservation>{};
   out.reserve(std::min(records.size(), limit));
   for (const auto& record : records) {
      if (out.size() >= limit) {
         break;
      }
      auto endpoints = record.endpoints;
      if (endpoints.size() > endpoint_limit) {
         endpoints.resize(endpoint_limit);
      }
      out.push_back(diagnostics::relay_reservation{
          .relay = record.relay,
          .reservation_id = record.reservation_id,
          .expires_at = record.expires_at,
          .endpoints = std::move(endpoints),
          .successes = record.successes,
          .failures = record.failures,
          .last_latency = record.last_latency,
          .score = record.score,
      });
   }
   return out;
}

[[nodiscard]] diagnostics::peer diagnostics_peer(const peer_store::record& record, const diagnostics::options& options,
                                                 bool protected_peer) {
   auto protocols = record.protocols;
   if (protocols.size() > options.max_protocols_per_peer) {
      protocols.resize(options.max_protocols_per_peer);
   }
   return diagnostics::peer{
       .peer = record.peer,
       .capabilities = record.capabilities,
       .discovered_by = record.discovered_by,
       .protocol_version = record.protocol_version,
       .agent_version = record.agent_version,
       .protocols = std::move(protocols),
       .endpoints = diagnostics_endpoints(record.endpoints, options.max_endpoints_per_peer),
       .relay_reservations = diagnostics_relays(record.relay_reservations, options.max_relay_reservations_per_peer,
                                                options.max_endpoints_per_peer),
       .reachability = record.reachability,
       .observed_endpoint = record.observed_endpoint,
       .reachability_expires_at = record.reachability_expires_at,
       .discovered_at = record.discovered_at,
       .discovery_expires_at = record.discovery_expires_at,
       .discovery_backoff_until = record.discovery_backoff_until,
       .successes = record.successes,
       .failures = record.failures,
       .last_latency = record.last_latency,
       .score = record.score,
       .protected_peer = protected_peer,
   };
}

[[nodiscard]] pubsub::snapshot diagnostics_pubsub(const auto& impl) {
   auto mesh_edges = std::size_t{};
   for (const auto& [_, peers] : impl.pubsub_value.mesh) {
      mesh_edges += peers.size();
   }
   return pubsub::snapshot{
       .topics = impl.pubsub_value.handlers.size(),
       .peers = impl.pubsub_value.peer_topics.size(),
       .mesh_edges = mesh_edges,
       .cached_messages = impl.pubsub_value.cache.size(),
       .messages_published = impl.metrics_value.pubsub_messages_published,
       .messages_received = impl.metrics_value.pubsub_messages_received,
       .messages_delivered = impl.metrics_value.pubsub_messages_delivered,
       .duplicates = impl.metrics_value.pubsub_duplicates,
       .invalid_messages = impl.metrics_value.pubsub_invalid_messages,
       .control_messages = impl.metrics_value.pubsub_control_messages,
   };
}

boost::asio::awaitable<rendezvous::message>
exchange_rendezvous(const auto& self, const peer_id& peer, rendezvous::message request, std::string_view operation) {
   const auto started = std::chrono::steady_clock::now();
   const auto timeout = self->options.limits.topology.query_timeout;
   auto stream =
       co_await self->open_protocol_direct(peer, builtins::rendezvous, remaining_timeout(started, timeout, operation));
   auto deadline = operation_deadline{self->runtime.context(), remaining_timeout(started, timeout, operation)};
   deadline.arm([&stream] noexcept { stream.request_cancel(); });
   try {
      co_await stream.async_write(rendezvous::codec::encode(request, self->options.limits.rendezvous));
      auto buffer = std::vector<std::uint8_t>{};
      auto response = rendezvous::codec::decode(
          co_await async_read_length_delimited(stream, buffer, self->options.limits.rendezvous.max_message_size),
          self->options.limits.rendezvous);
      co_await stream.async_close();
      if (!deadline.finish()) {
         throw_operation_timeout(operation);
      }
      co_return response;
   } catch (...) {
      const auto completed = deadline.finish();
      stream.cancel();
      if (deadline.timed_out() || !completed) {
         throw_operation_timeout(operation);
      }
      throw;
   }
}

void stop_owned(auto self) {
   auto operations = std::vector<detail::session_teardown::operation>{};
   auto deadlines = std::vector<operation_deadline::stop_token>{};
   {
      auto lock = std::scoped_lock{self->mutex};
      if (self->stopped) {
         return;
      }
      operations.reserve(self->sessions.size() + self->retiring_sessions.size() + 1);
      operations.push_back(self->direct_registry.teardown_operation());
      deadlines.reserve(self->protocol_open_deadlines.size());
      for (auto& [_, deadline] : self->protocol_open_deadlines) {
         deadlines.push_back(std::move(deadline));
      }
      self->protocol_open_deadlines.clear();
      self->stop_requested_at = std::chrono::steady_clock::now();
      self->stopped = true;
      while (!self->sessions.empty()) {
         const auto session = self->sessions.begin()->second;
         session->closed = true;
         static_cast<void>(self->retire_session_locked(session, false));
      }
      self->connections.clear();
      for (const auto& [_, session] : self->retiring_sessions) {
         if (session->retirement.terminal() || session->retirement.tracked()) {
            continue;
         }
         operations.push_back(detail::session_teardown::operation{
             .close = [self, session]() -> boost::asio::awaitable<void> {
                co_await self->async_retire_session(session, true);
             },
             .cancel = [session] { detail::request_session_cancel(session->connection); },
         });
      }
      self->inbound_relay_reservations.clear();
      self->outbound_relay_reservations.clear();
      self->clear_pubsub_outbound_locked();
      self->pubsub_value.active_validations_by_peer.clear();
      self->pubsub_value.active_validations = 0;
      self->metrics_value.active_sessions = 0;
      self->metrics_value.active_relay_reservations = 0;
      self->metrics_value.stopped = true;
   }
   self->direct_registry.stop();
   for (const auto& deadline : deadlines) {
      static_cast<void>(deadline.request_stop());
   }
   self->teardown.start(std::move(operations));
}

boost::asio::awaitable<void> async_stop_owned(auto self) {
   auto failure = std::exception_ptr{};
   const auto capture_failure = [&failure] {
      if (!failure) {
         failure = std::current_exception();
      }
   };
   try {
      co_await self->provider_registry->async_drain();
   } catch (...) {
      capture_failure();
   }
   try {
      co_await self->lifecycle.wait();
   } catch (...) {
      capture_failure();
   }
   try {
      co_await self->teardown.wait();
   } catch (...) {
      capture_failure();
   }
   if (failure) {
      // Provider removals are durable state transitions. Keep their backing
      // stores open so a subsequent async_stop() can retry the failed drain.
      std::rethrow_exception(failure);
   }
   for (auto& [_, profile] : self->dht_profiles) {
      try {
         co_await profile->records.async_close();
      } catch (...) {
         capture_failure();
      }
   }
   try {
      co_await self->store.async_close();
   } catch (...) {
      capture_failure();
   }
   self->lifecycle.finish_stop();
   if (failure) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> async_stop_after_topology_join(auto self) {
   // Shutdown owns resource teardown once requested; caller cancellation cannot leave it half-complete.
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   const auto executor = co_await boost::asio::this_coro::executor;
   co_await boost::asio::co_spawn(
       executor,
       [self = std::move(self)]() mutable -> boost::asio::awaitable<void> {
          auto failure = std::exception_ptr{};
          self->request_lifecycle_stop();
          try {
             co_await self->join_reachability();
          } catch (...) {
             failure = std::current_exception();
          }
          try {
             co_await self->async_join_topology_manager();
          } catch (...) {
             if (!failure) { failure = std::current_exception(); }
          }
          try {
             co_await self->async_close_dial_scheduler();
          } catch (...) {
             if (!failure) {
                failure = std::current_exception();
             }
          }
          try {
             co_await self->async_retire_sessions_gracefully();
          } catch (...) {
             if (!failure) { failure = std::current_exception(); }
          }
          try {
             stop_owned(self);
             co_await async_stop_owned(self);
          } catch (...) {
             if (!failure) {
                failure = std::current_exception();
             }
          }
          if (failure) {
             std::rethrow_exception(failure);
          }
       },
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

} // namespace

boost::asio::awaitable<rendezvous::message>
node::impl::exchange_rendezvous(const peer_id& peer, rendezvous::message request, std::string_view operation) {
   co_return co_await ::forge::net::p2p::exchange_rendezvous(shared_from_this(), peer, std::move(request), operation);
}

node::node(forge::asio::runtime& runtime, node::options options) {
   normalize_legacy_discovery(options);
   validate(options);
   normalize_topology_capacity(options);
   impl_ = std::make_shared<impl>(runtime, std::move(options));
   impl_->validate_local_identify_document();
   impl_->initialize_dht_provider_registry();
   impl_->initialize_lifecycle();
   impl_->initialize_topology_manager();
   impl_->initialize_reachability();
   // Launch the self-owning maintenance task only after every throwing
   // constructor step has completed.
   impl_->initialize_dht_routing_refresh();
}

node::~node() {
   if (!impl_) {
      return;
   }
   try {
      stop();
   } catch (...) {
      impl_->request_lifecycle_stop();
   }
}

node::node(node&&) noexcept = default;

node& node::operator=(node&& other) noexcept {
   if (this == &other) {
      return *this;
   }
   if (impl_) {
      try {
         stop();
      } catch (...) {
         impl_->request_lifecycle_stop();
      }
   }
   impl_ = std::move(other.impl_);
   return *this;
}

const peer_id& node::local_peer() const noexcept {
   return impl_->local;
}

std::optional<forge::net::p2p::endpoint> node::local_endpoint() const {
   auto endpoints = local_endpoints();
   if (endpoints.empty()) {
      return std::nullopt;
   }
   return endpoints.front();
}

std::vector<forge::net::p2p::endpoint> node::local_endpoints() const {
   return impl_->local_endpoints_for_control();
}

node::metrics_snapshot node::metrics() const {
   auto lock = std::scoped_lock{impl_->mutex};
   impl_->cleanup_expired_relay_reservations_locked();
   auto out = impl_->metrics_value;
   out.gater_peer_dial_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::peer_dial);
   out.gater_address_dial_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::address_dial);
   out.gater_accept_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::accept);
   out.gater_secured_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::secured);
   out.gater_upgraded_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::upgraded);
   out.active_sessions = impl_->sessions.size();
   out.active_relay_reservations = impl_->inbound_relay_reservations.size();
   out.stopped = impl_->stopped;
   return out;
}

forge::net::p2p::diagnostics::snapshot node::diagnostics(forge::net::p2p::diagnostics::options options) const {
   // Snapshot the detector under its own lock before entering the node mutex.
   const auto black_holes = impl_->dial_black_hole_status();
   const auto reachability = impl_->reachability_diagnostics();
   const auto persistence = impl_->store.persistence_state();
   const auto lifecycle = lifecycle_state();
   const auto retained_identify_attempts = impl_->identify_service.retained();
   const auto records =
       options.max_peers > 0 ? impl_->store.snapshot(options.max_peers) : std::vector<peer_store::record>{};
   auto dht_profiles = std::vector<forge::net::p2p::diagnostics::dht_profile_state>{};
   dht_profiles.reserve(std::min(options.max_dht_profiles, impl_->dht_profiles.size()));
   for (const auto& [protocol, state] : impl_->dht_profiles) {
      if (dht_profiles.size() == options.max_dht_profiles) {
         break;
      }
      const auto routing = state->routing.status();
      const auto records_state = state->records.persistence_state();
      const auto maintenance = impl_->routing_refresh ? impl_->routing_refresh->status(protocol) : std::nullopt;
      dht_profiles.push_back(forge::net::p2p::diagnostics::dht_profile_state{
          .protocol = protocol,
          .kind = state->profile.kind == dht::profile_kind::amino_v1 ? "amino-v1" : "custom",
          .mode = state->profile.operating_mode == dht::mode::server ? "server" : "client",
          .peers = state->profile.capabilities.peers,
          .providers = state->profile.capabilities.providers,
          .values = state->profile.capabilities.values,
          .routing_active = routing.active,
          .routing_replacements = routing.replacements,
          .routing_candidates = routing.candidates,
          .routing_nonempty_buckets = routing.nonempty_buckets,
          .maintenance_enabled = maintenance.has_value(),
          .maintenance_startup_pending = maintenance && maintenance->startup_lookup_pending,
          .maintenance_in_flight = maintenance && maintenance->in_flight,
          .maintenance_failures = maintenance ? maintenance->failures : 0,
          .maintenance_next_attempt = maintenance ? maintenance->next_attempt_in : std::chrono::milliseconds{0},
          .persistence_failures = records_state.failure_count,
          .persistence_degraded = records_state.degraded,
          .durability_uncertain = records_state.durability_uncertain,
          .persistence_closing = records_state.closing,
          .persistence_closed = records_state.closed,
          .last_persistence_failure = records_state.last_failure,
      });
   }
   const auto topology_status = impl_->topology_manager_value->current();
   const auto topology_phase = [&] {
      switch (topology_status.lifecycle_phase) {
      case detail::topology_manager::phase::idle:
         return std::string{"idle"};
      case detail::topology_manager::phase::running:
         return std::string{"running"};
      case detail::topology_manager::phase::stopping:
         return std::string{"stopping"};
      case detail::topology_manager::phase::stopped:
         return std::string{"stopped"};
      }
      return std::string{"unknown"};
   }();
   const auto& topology_policy = impl_->options.limits.topology;
   auto lock = std::scoped_lock{impl_->mutex};
   auto out = forge::net::p2p::diagnostics::snapshot{};
   out.network = forge::net::p2p::diagnostics::network_state{
       .local_peer = impl_->local,
       .local_endpoints = impl_->local_endpoints_for_control_locked(),
       .stopped = impl_->stopped,
   };
   out.lifecycle = lifecycle;
   out.black_holes = black_holes;
   out.reachability = reachability;
   out.effective_limits = impl_->resources.configured_limits();
   out.metrics = impl_->metrics_value;
   out.metrics.gater_peer_dial_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::peer_dial);
   out.metrics.gater_address_dial_rejections =
       impl_->connection_gate->denied(detail::connection_gater_stage::address_dial);
   out.metrics.gater_accept_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::accept);
   out.metrics.gater_secured_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::secured);
   out.metrics.gater_upgraded_rejections = impl_->connection_gate->denied(detail::connection_gater_stage::upgraded);
   out.metrics.active_sessions = impl_->sessions.size();
   out.metrics.active_relay_reservations = impl_->inbound_relay_reservations.size();
   out.metrics.stopped = impl_->stopped;
   out.resources = impl_->resources.current();
   out.pubsub = diagnostics_pubsub(*impl_);

   auto connection_snapshot = impl_->connections.current(options.max_sessions);
   out.connections = forge::net::p2p::diagnostics::connection_state{
       .active_sessions = connection_snapshot.active_sessions,
       .protected_peers = std::move(connection_snapshot.protected_peers),
       .retained_identify_attempts = retained_identify_attempts,
   };
   out.persistence = forge::net::p2p::diagnostics::persistence_state{
       .pending_peer_mutations = persistence.pending_peer_mutations,
       .failure_count = persistence.failure_count,
       .degraded = persistence.degraded,
       .closing = persistence.closing,
       .closed = persistence.closed,
       .last_failure = persistence.last_failure,
   };
   out.dht_profiles = std::move(dht_profiles);
   out.topology = forge::net::p2p::diagnostics::topology_state{
       .mode = topology_policy.operating_mode == topology::mode::managed ? "managed" : "static-only",
       .phase = topology_phase,
       .low_watermark = topology_policy.peers.low,
       .target_watermark = topology_policy.peers.target,
       .high_watermark = topology_policy.peers.high,
       .refresh_interval = topology_policy.refresh_interval,
       .query_timeout = topology_policy.query_timeout,
       .max_candidates = topology_policy.max_candidates,
       .max_parallel_queries = topology_policy.max_parallel_queries,
       .max_parallel_dials = topology_policy.max_parallel_dials,
       .configured_rendezvous_points = topology_policy.rendezvous_points.size(),
       .max_peer_exchange_peers = topology_policy.max_peer_exchange_peers,
       .dht_enabled = topology_policy.dht_enabled,
       .peer_exchange_enabled = topology_policy.peer_exchange_enabled,
       .refresh_queued = topology_status.refresh_queued,
       .refresh_in_flight = topology_status.refresh_in_flight,
       .observations = topology_status.observations,
       .active_operations = topology_status.active_operations,
       .waiting_refreshes = topology_status.waiting_refreshes,
       .completed_refreshes = topology_status.completed_refreshes,
       .failed_refreshes = topology_status.failed_refreshes,
   };

   const auto now = std::chrono::steady_clock::now();
   out.sessions.reserve(connection_snapshot.sessions.size());
   for (const auto& record : connection_snapshot.sessions) {
      const auto found = impl_->sessions.find(record.id);
      if (found == impl_->sessions.end()) {
         continue;
      }
      const auto& session = *found->second;
      out.sessions.push_back(forge::net::p2p::diagnostics::session{
          .id = session.id,
          .remote_peer = session.info.remote_peer,
          .capabilities = session.info.capabilities,
          .path = session.info.path,
          .relay_peer = session.info.relay_peer,
          .direct_endpoint = session.direct_endpoint,
          .remote_endpoint = session.remote_endpoint,
          .direction = diagnostics_direction(record.direction),
          .age = elapsed_since(now, record.opened_at),
          .idle = elapsed_since(now, record.last_used_at),
          .closed = session.closed,
          .protected_peer = impl_->connections.is_protected(session.info.remote_peer),
          .identify_state = session.info.identify_state,
          .identify_error = session.identify_error,
      });
   }

   if (!records.empty()) {
      out.peers.reserve(std::min(options.max_peers, records.size()));
      for (const auto& record : records) {
         if (out.peers.size() >= options.max_peers) {
            break;
         }
         out.peers.push_back(diagnostics_peer(record, options, impl_->connections.is_protected(record.peer)));
      }
   }
   return out;
}

peer_store& node::peers() noexcept {
   return impl_->store;
}

const peer_store& node::peers() const noexcept {
   return impl_->store;
}

dht::routing_status node::routing_status(const protocol_id& profile) const {
   return impl_->dht_profile(profile).routing.status();
}

void node::protect_peer(peer_id peer, std::string tag) {
   auto lock = std::scoped_lock{impl_->mutex};
   impl_->connections.protect(peer, std::move(tag));
}

void node::tag_peer(peer_id peer, std::string tag, std::int64_t value) {
   auto lock = std::scoped_lock{impl_->mutex};
   impl_->connections.tag(peer, std::move(tag), value);
}

boost::asio::awaitable<void> node::async_hydrate_peer_state() {
   auto self = impl_;
   co_await self->async_hydrate_peer_state();
}

bool node::unprotect_peer(peer_id peer, std::string tag) {
   auto lock = std::scoped_lock{impl_->mutex};
   return impl_->connections.unprotect(peer, tag);
}

bool node::untag_peer(peer_id peer, std::string_view tag) {
   auto lock = std::scoped_lock{impl_->mutex};
   return impl_->connections.untag(peer, tag);
}

bool node::is_peer_protected(const peer_id& peer) const {
   auto lock = std::scoped_lock{impl_->mutex};
   return impl_->connections.is_protected(peer);
}

void node::register_protocol_handler(protocol_id protocol, node::protocol_handler handler) {
   impl_->register_protocol_handler(std::move(protocol), std::move(handler));
}

bool node::unregister_protocol_handler(const protocol_id& protocol) {
   return impl_->unregister_protocol_handler(protocol);
}

void node::set_advertised_endpoints(std::vector<forge::net::p2p::endpoint> endpoints) {
   if (impl_->private_network_enabled()) {
      for (const auto& endpoint : endpoints) {
         impl_->require_private_direct_tcp(endpoint, "advertised endpoint");
      }
   }
   impl_->set_advertised_endpoints(std::move(endpoints));
}

boost::asio::awaitable<void> node::async_listen(forge::net::p2p::endpoint endpoint) {
   auto self = impl_;
   self->require_private_direct_tcp(endpoint, "listener");
   self->listen(std::move(endpoint));
   self->notify_listen_endpoints_changed();
   co_return;
}

boost::asio::awaitable<node::session_info> node::async_connect(forge::net::p2p::endpoint endpoint) {
   return async_connect(std::move(endpoint), connect_options{});
}

boost::asio::awaitable<node::session_info> node::async_connect(forge::net::p2p::endpoint endpoint,
                                                               node::connect_options options) {
   impl_->require_private_direct_tcp(endpoint, "connect");
   return async_connect(endpoint.to_multiaddr(), std::move(options));
}

boost::asio::awaitable<node::session_info> node::async_connect(forge::multiformats::multiaddr address) {
   return async_connect(std::move(address), connect_options{});
}

boost::asio::awaitable<node::session_info> node::async_connect(forge::multiformats::multiaddr address,
                                                               node::connect_options options) {
   return impl::async_connect_owned(impl_, std::move(address), std::move(options));
}

boost::asio::awaitable<void> node::async_request_peer_exchange(peer_id peer) {
   auto self = impl_;
   co_await self->request_peer_exchange(peer);
}

boost::asio::awaitable<reachability::state> node::async_probe_reachability(peer_id observer) {
   return impl::probe_reachability_owned(impl_, std::move(observer));
}

boost::asio::awaitable<rendezvous::register_response>
node::async_rendezvous_register(peer_id rendezvous_peer, rendezvous::register_request request) {
   auto self = impl_;
   auto response = co_await self->exchange_rendezvous(rendezvous_peer,
                                                      rendezvous::message{
                                                          .type = rendezvous::message_type::register_peer,
                                                          .register_value = std::move(request),
                                                      },
                                                      "P2P rendezvous registration");
   if (response.type != rendezvous::message_type::register_response || !response.register_response_value) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "rendezvous expected register response");
   }
   co_return *response.register_response_value;
}

boost::asio::awaitable<rendezvous::discover_response>
node::async_rendezvous_discover(peer_id rendezvous_peer, rendezvous::discover_request request) {
   auto self = impl_;
   auto response = co_await self->exchange_rendezvous(rendezvous_peer,
                                                      rendezvous::message{
                                                          .type = rendezvous::message_type::discover,
                                                          .discover_value = std::move(request),
                                                      },
                                                      "P2P rendezvous discovery");
   if (response.type != rendezvous::message_type::discover_response || !response.discover_response_value) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "rendezvous expected discover response");
   }
   auto sanitized_registrations = std::vector<rendezvous::registration>{};
   const auto context = third_party_discovery_context();
   const auto received_at = std::chrono::system_clock::now();
   for (auto& registration : response.discover_response_value->registrations) {
      registration.expires_at = detail::rendezvous_expiry_after(received_at, registration.ttl);
      auto sanitized = sanitize_discovered_registration(std::move(registration), context);
      if (!sanitized) {
         continue;
      }
      sanitized_registrations.push_back(std::move(*sanitized));
   }
   response.discover_response_value->registrations = std::move(sanitized_registrations);
   co_return *response.discover_response_value;
}

boost::asio::awaitable<std::chrono::milliseconds> node::async_ping(peer_id peer) {
   co_return co_await async_ping(std::move(peer), open_options{});
}

boost::asio::awaitable<std::chrono::milliseconds> node::async_ping(peer_id peer, open_options options) {
   auto started = std::chrono::steady_clock::now();
   auto stream = co_await async_open_protocol_stream(std::move(peer), builtins::ping, std::move(options));
   const auto payload = forge::crypto::core::random_bytes(32);
   co_await stream.async_write(payload);
   const auto reply = co_await stream.async_read();
   if (reply != payload) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "libp2p ping payload mismatch");
   }
   co_await stream.async_close();
   co_return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
}

boost::asio::awaitable<forge::net::p2p::stream> node::async_open_protocol_stream(peer_id peer, protocol_id protocol) {
   return async_open_protocol_stream(std::move(peer), std::move(protocol), open_options{});
}

boost::asio::awaitable<forge::net::p2p::stream> node::async_open_protocol_stream(peer_id peer, protocol_id protocol,
                                                                                 node::open_options options) {
   validate_operation_timeout(options.timeout, "P2P protocol open timeout");
   validate_operation_timeout(options.direct_attempt_timeout, "P2P direct attempt timeout");
   validate_operation_timeout(options.relay_attempt_timeout, "P2P relay attempt timeout");
   if (options.max_direct_endpoints == 0 || options.max_relay_candidates == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P path attempt limits must be positive");
   }
   auto self = impl_;
   self->require_private_protocol_allowed(protocol);
   if (self->private_network_enabled()) {
      if (options.relay_peer) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network protocol open does not permit a relay peer");
      }
      options.allow_relay = false;
      options.allow_hole_punch = false;
   }
   auto effective = options;
   effective.allow_relay =
       effective.allow_relay && self->options.path_policy.allow_relay && self->options.relay_policy.client_enabled;
   effective.allow_hole_punch = effective.allow_hole_punch && self->options.path_policy.allow_hole_punch;
   effective.max_direct_endpoints =
       std::min(effective.max_direct_endpoints, self->options.path_policy.max_direct_endpoints);
   effective.max_relay_candidates =
       std::min(effective.max_relay_candidates, self->options.path_policy.max_relay_candidates);
   const auto started = std::chrono::steady_clock::now();
   auto last_kind = std::optional<exceptions::code>{};
   auto last_message = std::string{};
   if (self->options.path_policy.allow_direct) {
      try {
         co_return co_await self->open_protocol_direct(
             peer, protocol, effective.timeout, effective.max_direct_endpoints, effective.direct_attempt_timeout);
      } catch (const forge::exceptions::base& error) {
         const auto kind = p2p_code(error);
         last_kind = kind;
         last_message = error.what();
         auto node_stopped = false;
         if (kind == exceptions::code::closed) {
            auto lock = std::scoped_lock{self->mutex};
            node_stopped = self->stopped;
         }
         if ((kind == exceptions::code::closed && node_stopped) || kind == exceptions::code::canceled ||
             kind == exceptions::code::unsupported_protocol || kind == exceptions::code::protocol_error ||
             kind == exceptions::code::codec_error) {
            throw;
         }
         try {
            (void)remaining_timeout(started, effective.timeout, "P2P protocol open");
         } catch (const forge::exceptions::base&) {
            throw;
         }
         if (!effective.allow_relay && !(effective.allow_hole_punch && effective.relay_peer)) {
            throw;
         }
      }
   }

   auto relay_candidates = std::vector<peer_id>{};
   if (effective.relay_peer) {
      relay_candidates.push_back(*effective.relay_peer);
   } else if (effective.allow_relay || effective.allow_hole_punch) {
      relay_candidates = self->fresh_outbound_relay_candidates(effective.max_relay_candidates,
                                                               self->options.relay_policy.refresh_margin);
      if (relay_candidates.empty() && self->options.relay_policy.auto_discovery_enabled) {
         const auto remaining = remaining_timeout(started, effective.timeout, "P2P AutoRelay refresh");
         const auto refresh_timeout =
             attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P AutoRelay refresh");
         try {
            (void)co_await self->refresh_relay_candidates(peer, refresh_timeout);
            relay_candidates = self->fresh_outbound_relay_candidates(effective.max_relay_candidates,
                                                                     self->options.relay_policy.refresh_margin);
         } catch (const forge::exceptions::base& error) {
            last_kind = p2p_code(error);
            last_message = error.what();
         }
      }
   }

   if (effective.allow_hole_punch) {
      for (const auto& relay_peer : relay_candidates) {
         const auto remaining = remaining_timeout(started, effective.timeout, "P2P hole punch");
         const auto per_attempt = attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P hole punch attempt");
         try {
            const auto status = co_await self->attempt_hole_punch(peer, relay_peer, per_attempt);
            if (status == hole_punch::status::succeeded) {
               co_return co_await self->open_protocol_direct(
                   peer, protocol, remaining_timeout(started, effective.timeout, "P2P protocol open after hole punch"),
                   effective.max_direct_endpoints, effective.direct_attempt_timeout);
            }
         } catch (const forge::exceptions::base& error) {
            last_kind = p2p_code(error);
            last_message = error.what();
         }
      }
   }

   if (!effective.allow_relay) {
      if (last_kind) {
         FORGE_THROW_CODE(*last_kind, last_message);
      }
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay fallback is disabled");
   }

   if (relay_candidates.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P path manager found no reserved relay candidate");
   }
   for (const auto& relay_peer : relay_candidates) {
      const auto remaining = remaining_timeout(started, effective.timeout, "P2P protocol open");
      const auto per_attempt = attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P relay path attempt");
      try {
         co_return co_await self->open_protocol_via_relay(peer, protocol, relay_peer, per_attempt);
      } catch (const forge::exceptions::base& error) {
         last_kind = p2p_code(error);
         last_message = error.what();
      }
   }
   if (last_kind) {
      FORGE_THROW_CODE(*last_kind, last_message);
   }
   FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P path manager exhausted relay candidates");
}

boost::asio::awaitable<void> node::async_stop() {
   return async_stop_after_topology_join(impl_);
}

void node::request_stop() noexcept {
   impl_->request_lifecycle_stop();
}

void node::stop() {
   auto self = impl_;
   self->request_lifecycle_stop();
   stop_owned(std::move(self));
}

} // namespace forge::net::p2p
