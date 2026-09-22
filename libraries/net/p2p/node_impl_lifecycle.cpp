module;

#include "details/mdns_registry.hxx"
#include "details/interface_watcher.hxx"
#include "details/mdns_wire.hxx"
#include <boost/asio/ip/udp.hpp>

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <algorithm>
#include <array>
#include <atomic>
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

module forge.net.p2p.node;

import forge.exceptions;

import forge.asio.gate;
import forge.codec.hex;
import forge.net.pnet.network_fingerprint;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.address_resolution;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.exceptions;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/bootstrap_service.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/node_impl.hxx"
#include "details/mdns_service.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] bool lifecycle_queryable(const peer_store::record& record) noexcept {
   return !record.endpoints.empty() && (record.discovery_backoff_until == std::chrono::system_clock::time_point{} ||
                                        record.discovery_backoff_until <= std::chrono::system_clock::now());
}

[[nodiscard]] dht::peer lifecycle_dht_peer(const peer_store::record& record) {
   auto endpoints = std::vector<forge::multiformats::multiaddr>{};
   endpoints.reserve(record.endpoints.size());
   for (const auto& item : record.endpoints) {
      endpoints.push_back(item.address);
   }
   return dht::peer{
       .id = record.peer, .endpoints = std::move(endpoints), .connection = dht::connection_type::can_connect};
}

} // namespace

void validate_bootstrap(const std::vector<bootstrap_peer>& peers, bool require_nonempty,
                        const address_resolution::policy& resolution, bool tcp_only) {
   if (peers.size() > 4'096) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P bootstrap root count exceeds 4096");
   }
   if (require_nonempty && peers.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P bootstrap connection is required but no bootstrap peers are configured");
   }
   auto keys = std::set<std::string>{};
   for (const auto& peer : peers) {
      const auto& components = peer.address.components();
      if (components.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "empty P2P bootstrap address");
      }
      if (peer.address.to_bytes().size() > resolution.bounds.max_multiaddr_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P bootstrap root exceeds DNS address size limit");
      }
      auto dnsaddr = false;
      for (auto index = std::size_t{}; index < components.size(); ++index) {
         const auto& component = components[index];
         using code = forge::multiformats::protocol_code;
         dnsaddr = dnsaddr || component.code == code::dnsaddr;
         if (component.code == code::p2p_circuit || component.code == code::ws || component.code == code::wss ||
             component.code == code::quic ||
             (tcp_only && (component.code == code::udp || component.code == code::quic_v1))) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unsupported P2P bootstrap transport");
         }
         if (component.code == code::p2p) {
            if (index + 1 != components.size()) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P bootstrap peer must be a terminal suffix");
            }
            try {
               if (!valid_peer_id(peer_id::from_string(component.value))) {
                  FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P bootstrap peer id");
               }
            } catch (...) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P bootstrap peer id");
            }
         }
      }
      if (!dnsaddr) {
         auto concrete = std::optional<endpoint>{};
         try {
            concrete = parse_endpoint(peer.address.to_string());
         } catch (...) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P bootstrap direct address");
         }
         if ((!concrete->is_direct_tcp() && !concrete->is_direct_quic()) || (tcp_only && !concrete->is_direct_tcp())) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P bootstrap direct address");
         }
      }
      if (!keys.insert(peer.address.to_string()).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "duplicate P2P bootstrap endpoint");
      }
   }
}

void node::impl::initialize_lifecycle() {
   const auto weak = weak_from_this();
   bootstrap = std::make_shared<detail::bootstrap_service>(
       runtime.context().get_executor(), options.lifecycle,
       detail::bootstrap_service::callbacks{
           .connect = [weak](bootstrap_peer peer, std::chrono::milliseconds timeout,
                             std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<peer_id> {
              const auto self = weak.lock();
              if (!self) {
                 FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node no longer owns bootstrap state");
              }
              auto roots = std::vector<forge::multiformats::multiaddr>{std::move(peer.address)};
              const auto session = co_await self->connect_direct(std::move(roots),
                                                                 node::connect_options{
                                                                     .allow_relay = false,
                                                                     .timeout = timeout,
                                                                     .direct_attempt_timeout = timeout,
                                                                     .allow_hole_punch = false,
                                                                 },
                                                                 std::move(cancellation));
              co_return session->info.remote_peer;
           },
           .connected =
               [weak](const bootstrap_peer& configured, const peer_id& peer) {
                  const auto self = weak.lock();
                  if (!self) {
                     return false;
                  }
                  const auto root_key = configured.address.to_string();
                  const auto lock = std::scoped_lock{self->mutex};
                  return std::ranges::any_of(self->sessions, [&](const auto& item) {
                     const auto& session = item.second;
                     return !session->closed && session->info.remote_peer == peer &&
                            session->info.path == path::kind::direct &&
                            std::ranges::any_of(session->direct_roots, [&](const auto& root) {
                               return root.to_string() == root_key;
                            });
                  });
               },
           .protect =
               [weak](const peer_id& peer) {
                  if (const auto self = weak.lock()) {
                     const auto lock = std::scoped_lock{self->mutex};
                     if (!self->stopped) {
                        self->connections.protect(peer, "bootstrap");
                     }
                  }
               },
           .unprotect =
               [weak](const peer_id& peer) {
                  if (const auto self = weak.lock()) {
                     const auto lock = std::scoped_lock{self->mutex};
                     static_cast<void>(self->connections.unprotect(peer, "bootstrap"));
                  }
               },
           .prune_peer_state = [weak]() -> boost::asio::awaitable<void> {
              const auto self = weak.lock();
              if (!self) {
                 co_return;
              }
              auto failure = std::exception_ptr{};
              try {
                 static_cast<void>(co_await self->store.async_prune_expired());
              } catch (...) {
                 failure = std::current_exception();
              }
              for (auto& [_, state] : self->dht_profiles) {
                 try {
                    // One bounded page per profile keeps maintenance fair.
                    static_cast<void>(co_await state->records.async_prune_expired());
                 } catch (...) {
                    if (!failure) {
                       failure = std::current_exception();
                    }
                 }
              }
              if (failure) {
                 std::rethrow_exception(failure);
              }
           },
       });
}

void node::impl::request_lifecycle_stop() noexcept {
   stop_mdns();
   auto active_peer_exchange_operations = std::map<std::uint64_t, std::shared_ptr<peer_exchange_operation>>{};
   {
      const auto lock = std::scoped_lock{mutex};
      peer_exchange_admission_closed = true;
      session_admission_closed = true;
      close_reachability_results_locked();
      peer_exchange_value.close();
      active_peer_exchange_operations.swap(peer_exchange_operations);
   }
   request_dial_scheduler_stop();
   stop_reachability();
   for (const auto& [_, operation] : active_peer_exchange_operations) {
      operation->cancellation.request_stop();
   }
   if (topology_manager_value) {
      topology_manager_value->request_stop();
   }
   if (routing_refresh) {
      routing_refresh->request_stop();
   }
   if (provider_registry) {
      provider_registry->seal();
   }
   lifecycle.request_stop();
   bootstrap->request_stop();
   identify_service.close();
   session_admission_gate.close();
   lifecycle_wakeup->notify();
}

boost::asio::awaitable<void> node::impl::async_hydrate_peer_state() {
   auto hydration = co_await peer_state_hydration_gate.acquire();
   {
      const auto lock = std::scoped_lock{mutex};
      if (peer_state_hydrated) {
         co_return;
      }
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
   }
   co_await store.async_hydrate();
   for (auto& [_, state] : dht_profiles) {
      co_await state->records.async_hydrate();
   }
   for (const auto& record : store.snapshot(options.peer_state.max_peers)) {
      if (record.peer == local || !lifecycle_queryable(record)) {
         continue;
      }
      for (auto& [protocol, state] : dht_profiles) {
         if (std::ranges::find(record.protocols, protocol) != record.protocols.end()) {
            state->routing.upsert(lifecycle_dht_peer(record), dht::routing_admission::candidate);
         }
      }
   }
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node stopped during peer-state hydration");
      }
      peer_state_hydrated = true;
   }
   provider_registry->open_admission();
}

void node::impl::listen(forge::net::p2p::endpoint endpoint) {
   auto local_endpoint = forge::net::p2p::endpoint{};
   auto launch_identify_push = false;
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped");
      }
      local_endpoint = direct_registry.listen(std::move(endpoint));
      refresh_reachability_locked();
      launch_identify_push = advance_identify_generation_locked() && schedule_identify_push_locked();
   }
   if (provider_registry) {
      provider_registry->notify_endpoints_changed();
   }
   notify_reachability_changed();
   if (mdns_service_value) { mdns_service_value->notify_addresses_changed(); }
   if (launch_identify_push) {
      launch_identify_pushes();
   }
   launch_accept_loop(std::move(local_endpoint));
   launch_pubsub_heartbeat();
   launch_relay_discovery_maintenance();
}

boost::asio::awaitable<lifecycle_status> node::impl::async_start_lifecycle() {
   co_await async_hydrate_peer_state();
   lifecycle.set_phase(lifecycle_phase::listening);
   for (const auto& endpoint : options.lifecycle.listen) {
      listen(endpoint);
   }
   if (mdns_service_value) { mdns_service_value->start(lifecycle); }

   lifecycle.set_phase(lifecycle_phase::bootstrapping);
   const auto connected = co_await bootstrap->async_initial_bootstrap();
   if (options.lifecycle.requirement == bootstrap_requirement::require_connection && connected == 0) {
      FORGE_THROW_EXCEPTION(exceptions::timeout,
                            "P2P initial bootstrap did not establish a required connection within startup budget");
   }

   start_topology_manager();
   start_reachability();
   lifecycle.set_phase(lifecycle_phase::maintenance);
   bootstrap->start_maintenance(lifecycle);
   co_return lifecycle_status{
       .phase = lifecycle_phase::maintenance,
       .requirement = options.lifecycle.requirement,
       .configured_bootstrap = bootstrap->configured_count(),
       .connected_bootstrap = connected,
       .degraded = bootstrap->configured_count() != 0 && connected == 0,
       .last_bootstrap_failure = bootstrap->last_failure(),
   };
}

void node::impl::initialize_mdns() {
   if (!options.mdns.enabled) { return; }
   auto service = detail::mdns_codec::name{"_p2p", "_udp", "local"};
   if (options.private_network) {
      const auto fingerprint = options.private_network->protector->network_fingerprint();
      service.front() += "-" + forge::codec::hex::encode(fingerprint.bytes);
   }
   const auto weak = weak_from_this();
   mdns_service_value = std::make_shared<detail::mdns_service>(runtime.context().get_executor(), resources,
       options.mdns, local, std::move(service), detail::mdns_service::callbacks{
           .listeners = [weak] {
              if (const auto self = weak.lock()) {
                 const auto lock = std::scoped_lock{self->mutex};
                 return self->direct_registry.local_endpoints();
              }
              return std::vector<endpoint>{};
           },
           .replace = [weak](std::vector<detail::mdns_registry::lease> leases) {
              if (const auto self = weak.lock()) {
                 auto values = std::vector<detail::topology_manager::mdns_lease>{};
                 values.reserve(leases.size());
                 for (auto& lease : leases) {
                    values.push_back({std::move(lease.peer), lease.address.to_multiaddr(),
                                      lease.interface_index, lease.generation, lease.expires_at});
                 }
                 self->topology_manager_value->replace_mdns_snapshot(std::move(values));
              }
           },
           .error = [](std::exception_ptr failure) {
              try { if (failure) { std::rethrow_exception(failure); } }
              catch (...) { forge::exceptions::capture_and_log("P2P mDNS service failed"); }
           },
       });
}

void node::impl::stop_mdns() noexcept {
   if (mdns_service_value) { mdns_service_value->request_stop(); }
}

boost::asio::awaitable<void> node::impl::join_mdns() {
   if (mdns_service_value) { co_await mdns_service_value->async_join(); }
}

} // namespace forge::net::p2p
