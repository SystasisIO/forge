module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
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
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.chrono.timestamp;
import forge.net.p2p.ipns;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/dht_fanout.hxx"
#include "details/dht_query.hxx"
#include "details/dht_time.hxx"
#include "details/cancellation_latch.hxx"
#include "details/identity_signature.hxx"
#include "details/node_impl.hxx"

namespace forge::net::p2p {

void remember_dht_peer(peer_store& store, const protocol_id& protocol, dht::routing_table& routing,
                       std::chrono::milliseconds refresh_interval, const dht::peer& value,
                       dht::routing_admission admission);
void mark_dht_routing_failure(dht::routing_table& routing, const peer_id& peer);
[[nodiscard]] bool remote_peer_attributable_failure(std::mutex& mutex, const bool& stopped,
                                                    const forge::exceptions::base& error);
[[nodiscard]] host_addresses::learning_context third_party_discovery_context();
[[nodiscard]] dht::peer sanitize_discovered_peer(dht::peer value, host_addresses::learning_context context);
[[nodiscard]] bool has_usable_endpoint(const dht::peer& value) noexcept;
[[nodiscard]] std::chrono::system_clock::time_point
dht_value_expiry(const dht::record& value, std::chrono::system_clock::time_point now, const dht::profile& profile);

namespace {

[[nodiscard]] dht::peer dht_peer_from_record(const peer_store::record& record) {
   auto endpoints = std::vector<forge::multiformats::multiaddr>{};
   endpoints.reserve(record.endpoints.size());
   for (const auto& item : record.endpoints) {
      endpoints.push_back(item.address);
   }
   return dht::peer{
       .id = record.peer, .endpoints = std::move(endpoints), .connection = dht::connection_type::can_connect};
}

[[nodiscard]] dht::peer provider_peer(const dht::record_store::provider_record& value) {
   return dht::peer{.id = value.provider,
                    .endpoints = value.endpoints,
                    .connection = value.endpoints.empty() ? dht::connection_type::not_connected
                                                          : dht::connection_type::can_connect};
}

void merge_provider(std::vector<dht::peer>& output, dht::peer value, std::size_t peer_limit,
                    std::size_t endpoint_limit) {
   const auto current = std::ranges::find_if(output, [&](const auto& item) { return item.id == value.id; });
   if (current == output.end()) {
      if (output.size() < peer_limit) {
         if (value.endpoints.size() > endpoint_limit) {
            value.endpoints.resize(endpoint_limit);
         }
         output.push_back(std::move(value));
      }
      return;
   }
   for (auto& endpoint : value.endpoints) {
      if (current->endpoints.size() >= endpoint_limit) {
         break;
      }
      const auto known = std::ranges::any_of(
          current->endpoints, [&](const auto& item) { return item.to_string() == endpoint.to_string(); });
      if (!known) {
         current->endpoints.push_back(std::move(endpoint));
      }
   }
}

void validate_query_options(const dht::query_options& options) {
   // A zero quorum is not a local-only mode: PUT/PROVIDE quorum is always remote.
   if (options.requested_count == 0 || options.quorum == 0 || options.timeout.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "DHT query count, quorum and timeout must be positive");
   }
}

[[nodiscard]] host_addresses::learning_context
routed_discovery_context(std::optional<endpoint> remote_endpoint, std::optional<endpoint> direct_endpoint) {
   auto provenance_endpoint = remote_endpoint ? std::move(remote_endpoint) : std::move(direct_endpoint);
   if (!provenance_endpoint) {
      return third_party_discovery_context();
   }
   return host_addresses::learning_context{
       .source = host_addresses::source_kind::routed,
       .remote_endpoint = std::move(provenance_endpoint),
   };
}

boost::asio::awaitable<dht_query::result> run_lookup_with_alpha(const auto& self, const protocol_id& protocol,
                                                                dht::key target, std::optional<peer_id> target_peer,
                                                                dht::message_type type,
                                                                dht::query_options query_options,
                                                                std::optional<std::size_t> alpha_limit,
                                                                auto response_complete,
                                                                std::shared_ptr<cancellation_latch> cancellation = {}) {
   validate_query_options(query_options);
   if (cancellation && cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P DHT lookup canceled before launch");
   }
   auto& state = self->dht_profile(protocol);
   auto limits = state.profile.limits;
   if (alpha_limit) {
      limits.alpha = std::min(limits.alpha, *alpha_limit);
   }
   const auto started = std::chrono::steady_clock::now();
   auto response_contexts =
       std::make_shared<std::map<peer_id, std::pair<std::optional<endpoint>, std::optional<endpoint>>>>();
   auto result = co_await dht_query::run(
       dht_query::request{
           .target = target,
           .target_peer = std::move(target_peer),
           .options = limits,
           // Kademlia starts from the local k-closest shortlist; alpha only bounds concurrent RPCs.
           .seeds = state.routing.query_seeds(target.bytes, state.profile.limits.replication),
           .requested_provider_count = type == dht::message_type::get_providers ? query_options.requested_count : 0,
           .collect_value_responses = type == dht::message_type::get_value,
       },
       [self, protocol, target, type, started, response_contexts, cancellation,
        timeout = query_options.timeout](const dht::peer& candidate) -> boost::asio::awaitable<dht::message> {
          if (cancellation && cancellation->stop_requested()) {
             FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P DHT lookup canceled");
          }
          auto response = co_await self->exchange_dht(
              protocol, candidate.id, dht::message{.type = type, .key_value = target},
              remaining_timeout(started, timeout, "P2P DHT lookup"), cancellation);
          auto [_, inserted] = response_contexts->insert_or_assign(
              candidate.id, std::pair{std::move(response.remote_endpoint), std::move(response.direct_endpoint)});
          static_cast<void>(inserted);
          co_return std::move(response.message);
       },
       [self, protocol, response_contexts, response_complete = std::move(response_complete)](
           const dht::peer& candidate, dht::message& response) mutable -> boost::asio::awaitable<bool> {
          auto& current = self->dht_profile(protocol);
          current.routing.upsert(candidate, dht::routing_admission::verified_server);
          self->notify_dht_routing_refresh();
          auto context = third_party_discovery_context();
          if (const auto exchanged = response_contexts->find(candidate.id); exchanged != response_contexts->end()) {
             context = routed_discovery_context(exchanged->second.first, exchanged->second.second);
          }
          for (auto& closer : response.closer_peers) {
             closer = sanitize_discovered_peer(std::move(closer), context);
             if (has_usable_endpoint(closer)) {
                remember_dht_peer(self->store, protocol, current.routing, current.profile.limits.refresh_interval,
                                  closer, dht::routing_admission::candidate);
             }
          }
          response.closer_peers.erase(std::remove_if(response.closer_peers.begin(), response.closer_peers.end(),
                                                     [](const auto& peer) { return !has_usable_endpoint(peer); }),
                                      response.closer_peers.end());
          for (auto& provider : response.provider_peers) {
             provider = sanitize_discovered_peer(std::move(provider), context);
          }
          co_return co_await response_complete(candidate, response);
       },
       [self](const dht::peer&, const forge::exceptions::base& error) {
          return remote_peer_attributable_failure(self->mutex, self->stopped, error);
       });
   if (cancellation && cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P DHT lookup canceled");
   }
   for (const auto& failed : result.failed) {
      mark_dht_routing_failure(state.routing, failed);
   }
   (void)remaining_timeout(started, query_options.timeout, "P2P DHT lookup");
   co_return result;
}

boost::asio::awaitable<dht_query::result> run_lookup(const auto& self, const protocol_id& protocol, dht::key target,
                                                     std::optional<peer_id> target_peer, dht::message_type type,
                                                     dht::query_options query_options, auto response_complete,
                                                     std::shared_ptr<cancellation_latch> cancellation = {}) {
   co_return co_await run_lookup_with_alpha(self, protocol, std::move(target), std::move(target_peer), type,
                                            query_options, std::nullopt, std::move(response_complete),
                                            std::move(cancellation));
}

boost::asio::awaitable<dht_query::result> run_lookup(const auto& self, const protocol_id& protocol, dht::key target,
                                                     std::optional<peer_id> target_peer, dht::message_type type,
                                                     dht::query_options query_options,
                                                     std::shared_ptr<cancellation_latch> cancellation = {}) {
   auto never_complete = [](const dht::peer&, dht::message&) -> boost::asio::awaitable<bool> { co_return false; };
   co_return co_await run_lookup(self, protocol, std::move(target), std::move(target_peer), type, query_options,
                                 std::move(never_complete), std::move(cancellation));
}

boost::asio::awaitable<dht_query::result>
run_lookup_with_alpha(const auto& self, const protocol_id& protocol, dht::key target,
                      std::optional<peer_id> target_peer, dht::message_type type, dht::query_options query_options,
                      std::size_t alpha_limit, std::shared_ptr<cancellation_latch> cancellation = {}) {
   auto never_complete = [](const dht::peer&, dht::message&) -> boost::asio::awaitable<bool> { co_return false; };
   co_return co_await run_lookup_with_alpha(self, protocol, std::move(target), std::move(target_peer), type,
                                            query_options, alpha_limit, std::move(never_complete),
                                            std::move(cancellation));
}

boost::asio::awaitable<std::size_t> publish_provider(const auto& self, const protocol_id& protocol, const dht::key& key,
                                                     const dht::peer& provider, dht::query_options query_options) {
   const auto started = std::chrono::steady_clock::now();
   query_options.timeout = remaining_timeout(started, query_options.timeout, "P2P DHT provide");
   auto lookup = co_await run_lookup(self, protocol, key, std::nullopt, dht::message_type::find_node, query_options);
   auto& state = self->dht_profile(protocol);
   auto candidates = std::move(lookup.query.closest_peers);
   if (candidates.empty()) {
      candidates = state.routing.closest(key.bytes, state.profile.limits.replication);
   }
   if (candidates.size() > state.profile.limits.replication) {
      candidates.resize(state.profile.limits.replication);
   }
   auto peers = std::vector<peer_id>{};
   peers.reserve(candidates.size());
   auto unique = std::set<peer_id>{};
   for (auto& candidate : candidates) {
      if (unique.insert(candidate.id).second) {
         peers.push_back(std::move(candidate.id));
      }
   }
   if (peers.empty()) {
      co_return 0;
   }
   const auto replication_target = peers.size();
   const auto fanout = co_await detail::dht_fanout::run(
       self->runtime.context(),
       detail::dht_fanout::request{
           .peers = std::move(peers),
           .concurrency = state.profile.limits.alpha,
           .success_target = replication_target,
           .timeout = remaining_timeout(started, query_options.timeout, "P2P DHT provide"),
           .operation = "P2P DHT provide",
       },
       [self, protocol, key, provider](const peer_id& candidate, std::chrono::milliseconds timeout,
                                      std::shared_ptr<cancellation_latch> cancellation)
           -> boost::asio::awaitable<bool> {
          try {
             co_await self->send_dht(
                 protocol, candidate,
                 dht::message{.type = dht::message_type::add_provider, .key_value = key, .provider_peers = {provider}},
                 timeout, std::move(cancellation));
             co_return true;
          } catch (const forge::exceptions::base& error) {
             if (!remote_peer_attributable_failure(self->mutex, self->stopped, error)) {
                throw;
             }
             auto& current = self->dht_profile(protocol);
             mark_dht_routing_failure(current.routing, candidate);
             co_return false;
          }
       });
   co_return fanout.succeeded;
}

} // namespace

boost::asio::awaitable<bool> node::impl::async_refresh_dht_routing(protocol_id protocol, dht::key target,
                                                                   std::chrono::milliseconds timeout,
                                                                   std::shared_ptr<cancellation_latch> cancellation) {
   auto self = shared_from_this();
   const auto requested_count = dht_profile(protocol).profile.limits.replication;
   const auto result =
       co_await run_lookup(self, protocol, std::move(target), std::nullopt, dht::message_type::find_node,
                           dht::query_options{.requested_count = requested_count, .quorum = 1, .timeout = timeout},
                           std::move(cancellation));
   co_return result.converged;
}

void node::impl::initialize_dht_provider_registry() {
   const auto weak = weak_from_this();
   provider_registry = std::make_shared<detail::dht_provider_registry>(detail::dht_provider_registry::callbacks{
       .track = [weak]() -> std::shared_ptr<void> {
          const auto self = weak.lock();
          if (!self) {
             return {};
          }
          auto operation = self->lifecycle.track();
          if (!operation.active()) {
             return {};
          }
          return std::make_shared<detail::lifecycle_tracker::operation>(std::move(operation));
       },
       .launch =
           [weak](std::function<boost::asio::awaitable<void>()> task) {
              const auto self = weak.lock();
              return self && self->launch_tracked(std::move(task));
           },
       .prepare = [weak](protocol_id protocol, dht::key key, detail::dht_provider_registry::schedule renewal)
           -> boost::asio::awaitable<detail::dht_provider_registry::prepared_provider> {
          const auto self = weak.lock();
          if (!self) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node no longer owns DHT provider state");
          }
          const auto local_endpoints = self->local_endpoints_for_control();
          if (local_endpoints.empty()) {
             FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                   "DHT provider publication requires an advertised endpoint");
          }
          auto& state = self->dht_profile(protocol);
          auto endpoints = std::vector<forge::multiformats::multiaddr>{};
          endpoints.reserve(local_endpoints.size());
          for (const auto& endpoint : local_endpoints) {
             if (endpoints.size() == state.profile.limits.max_peer_endpoints) {
                break;
             }
             endpoints.push_back(endpoint.to_multiaddr());
          }
          const auto stamped_at = std::chrono::steady_clock::now();
          const auto now = std::chrono::system_clock::now();
          co_await state.records.async_upsert_provider(dht::record_store::provider_record{
              .key = key,
              .provider = self->local,
              .endpoints = endpoints,
              .provider_expires_at = detail::dht_expiry_after(now, renewal.provider_ttl),
              .addresses_expires_at = detail::dht_expiry_after(now, renewal.address_ttl),
              .local_owned = true,
          });
          co_return detail::dht_provider_registry::prepared_provider{
              .provider = dht::peer{.id = self->local,
                                    .endpoints = std::move(endpoints),
                                    .connection = dht::connection_type::connected},
              .stamped_at = stamped_at,
          };
       },
       .publish = [weak](protocol_id protocol, dht::key key, dht::peer provider,
                         dht::query_options query) -> boost::asio::awaitable<std::size_t> {
          const auto self = weak.lock();
          if (!self) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node no longer owns DHT provider state");
          }
          if (provider.id != self->local || provider.endpoints.empty()) {
             FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                                   "DHT provider publication snapshot has no advertised endpoint");
          }
          co_return co_await publish_provider(self, protocol, key, provider, query);
       },
       .remove = [weak](protocol_id protocol, dht::key key) -> boost::asio::awaitable<void> {
          const auto self = weak.lock();
          if (!self) {
             FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node no longer owns DHT provider state");
          }
          co_await self->dht_profile(protocol).records.async_remove_provider(
              {.key = std::move(key), .provider = self->local});
       },
       .publication_limit = [weak](const protocol_id& protocol) -> std::size_t {
          const auto self = weak.lock();
          if (!self) {
             return 0;
          }
          return self->dht_profile(protocol).profile.limits.alpha;
       },
   });
}

namespace {

boost::asio::awaitable<dht::query_result> async_find_peer_owned(auto self, protocol_id protocol, peer_id peer,
                                                                dht::query_options options,
                                                                std::optional<std::size_t> alpha_limit = std::nullopt,
                                                                std::shared_ptr<cancellation_latch> cancellation = {}) {
   auto& state = self->dht_profile(protocol);
   if (!state.profile.capabilities.peers) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile does not enable peer routing");
   }
   auto lookup = dht_query::result{};
   if (alpha_limit) {
      lookup = co_await run_lookup_with_alpha(self, protocol, make_dht_key(peer), peer,
                                              dht::message_type::find_node, options, *alpha_limit, cancellation);
   } else {
      lookup = co_await run_lookup(self, protocol, make_dht_key(peer), peer, dht::message_type::find_node, options,
                                   cancellation);
   }
   if (lookup.query.complete) {
      if (const auto record = self->store.find(peer)) {
         auto exact = dht_peer_from_record(*record);
         const auto current = std::ranges::find_if(lookup.query.closest_peers,
                                                   [&](const auto& candidate) { return candidate.id == peer; });
         if (current == lookup.query.closest_peers.end()) {
            lookup.query.closest_peers.insert(lookup.query.closest_peers.begin(), std::move(exact));
         } else {
            *current = std::move(exact);
         }
         if (lookup.query.closest_peers.size() > state.profile.limits.replication) {
            lookup.query.closest_peers.resize(state.profile.limits.replication);
         }
      }
   }
   co_return std::move(lookup.query);
}

boost::asio::awaitable<provider_registration> async_provide_owned(auto self, protocol_id protocol, dht::key key,
                                                                  dht::query_options options) {
   auto& state = self->dht_profile(protocol);
   if (!state.profile.capabilities.providers) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile does not enable providers");
   }
   validate_query_options(options);
   if (self->local_endpoints_for_control().empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "DHT provider publication requires at least one advertised endpoint");
   }
   auto registry = self->provider_registry;
   auto lease =
       co_await registry->async_acquire(protocol, key, options,
                                        detail::dht_provider_registry::schedule{
                                            .provider_ttl = state.profile.limits.provider_record_ttl,
                                            .address_ttl = state.profile.limits.provider_address_ttl,
                                            .republish_interval = state.profile.limits.provider_republish_interval,
                                        });
   co_return detail::provider_registration_access::make(
       std::move(protocol), std::move(key), [registry, lease] { return registry->active(lease); },
       [registry, lease] { registry->request_release(lease); },
       [registry, lease]() -> boost::asio::awaitable<void> { co_await registry->async_release(lease); });
}

boost::asio::awaitable<std::vector<dht::peer>> async_find_providers_owned(auto self, protocol_id protocol, dht::key key,
                                                                          dht::query_options options) {
   auto& state = self->dht_profile(protocol);
   if (!state.profile.capabilities.providers) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile does not enable providers");
   }
   validate_query_options(options);
   auto output = std::vector<dht::peer>{};
   for (const auto& provider : state.records.find_providers(key, options.requested_count)) {
      merge_provider(output, provider_peer(provider), options.requested_count, state.profile.limits.max_peer_endpoints);
   }
   if (output.size() >= options.requested_count) {
      co_return output;
   }

   auto lookup = co_await run_lookup(self, protocol, key, std::nullopt, dht::message_type::get_providers, options);
   for (auto provider : lookup.query.provider_peers) {
      // A third-party GET_PROVIDERS response is discovery evidence, not an
      // authenticated provider announcement. Only inbound ADD_PROVIDER stores
      // durable provider ownership.
      merge_provider(output, std::move(provider), options.requested_count, state.profile.limits.max_peer_endpoints);
   }
   co_return output;
}

boost::asio::awaitable<dht::value_put_result> async_put_value_owned(auto self, protocol_id protocol, dht::record value,
                                                                    dht::query_options options) {
   auto& state = self->dht_profile(protocol);
   if (!state.profile.capabilities.values) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile does not enable values");
   }
   validate_query_options(options);
   const auto now = std::chrono::system_clock::now();
   const auto expires_at = dht_value_expiry(value, now, state.profile);
   auto stored = co_await state.records.async_put({.record = std::move(value), .expires_at = expires_at}, now);
   if (stored.outcome != dht::record_store::put_outcome::incoming_stored) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DHT PUT_VALUE incoming record was not selected locally");
   }
   const auto selected = stored.selected.record;
   const auto started = std::chrono::steady_clock::now();
   options.timeout = remaining_timeout(started, options.timeout, "P2P DHT PUT_VALUE");
   auto lookup = co_await run_lookup(self, protocol, stored.selected.record.key_value, std::nullopt,
                                     dht::message_type::find_node, options);
   auto peers = std::vector<peer_id>{};
   peers.reserve(lookup.query.closest_peers.size());
   for (auto& candidate : lookup.query.closest_peers) {
      peers.push_back(std::move(candidate.id));
   }
   const auto fanout = co_await detail::dht_fanout::run(
       self->runtime.context(),
       detail::dht_fanout::request{
           .peers = std::move(peers),
           .concurrency = state.profile.limits.alpha,
           .success_target = options.quorum,
           .timeout = remaining_timeout(started, options.timeout, "P2P DHT PUT_VALUE"),
           .operation = "P2P DHT PUT_VALUE",
       },
       [self, protocol, selected](const peer_id& peer, std::chrono::milliseconds timeout,
                                  std::shared_ptr<cancellation_latch> cancellation)
           -> boost::asio::awaitable<bool> {
          try {
             static_cast<void>(co_await self->exchange_dht(protocol, peer,
                                                           dht::message{.type = dht::message_type::put_value,
                                                                        .key_value = selected.key_value,
                                                                        .record_value = selected},
                                                           timeout, std::move(cancellation)));
             co_return true;
          } catch (const forge::exceptions::base& error) {
             if (!remote_peer_attributable_failure(self->mutex, self->stopped, error)) {
                throw;
             }
             auto& current = self->dht_profile(protocol);
             mark_dht_routing_failure(current.routing, peer);
             co_return false;
          }
       });
   auto result = dht::value_put_result{
       .selected = selected,
       .accepted = fanout.succeeded,
       .attempted = fanout.attempted,
   };
   result.quorum_reached = result.accepted >= options.quorum;
   co_return result;
}

boost::asio::awaitable<dht::value_get_result> async_get_value_owned(auto self, protocol_id protocol, dht::key key,
                                                                    dht::query_options options) {
   auto& state = self->dht_profile(protocol);
   if (!state.profile.capabilities.values) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile does not enable values");
   }
   validate_query_options(options);
   const auto started = std::chrono::steady_clock::now();
   auto result = dht::value_get_result{};
   if (const auto local = state.records.find_value(key)) {
      result.selected = local->record;
      ++result.valid_records;
   }
   if (result.valid_records >= options.quorum) {
      result.quorum_reached = true;
      co_return result;
   }
   auto lookup_options = options;
   lookup_options.timeout = remaining_timeout(started, options.timeout, "P2P DHT GET_VALUE");
   auto lookup =
       co_await run_lookup(self, protocol, key, std::nullopt, dht::message_type::get_value, lookup_options,
                           [&state, &result, quorum = options.quorum](
                               const dht::peer&, dht::message& response) -> boost::asio::awaitable<bool> {
                              if (!response.record_value) {
                                 co_return false;
                              }
                              const auto now = std::chrono::system_clock::now();
                              const auto stored = co_await state.records.async_put_received(
                                  {.record = *response.record_value,
                                   .expires_at = dht_value_expiry(*response.record_value, now, state.profile)},
                                  now);
                              if (!stored) {
                                 co_return false;
                              }
                              result.selected = stored->selected.record;
                              ++result.valid_records;
                              co_return result.valid_records >= quorum;
                           });
   result.responses = lookup.value_responses.size();
   result.quorum_reached = result.valid_records >= options.quorum;
   if (!result.selected) {
      co_return result;
   }

   auto correction_peers = std::vector<peer_id>{};
   correction_peers.reserve(lookup.value_responses.size());
   for (const auto& [peer, observed] : lookup.value_responses) {
      if (observed && observed->value == result.selected->value) {
         continue;
      }
      correction_peers.push_back(peer);
   }
   if (result.quorum_reached && !correction_peers.empty()) {
      const auto concurrency = state.profile.limits.alpha;
      const auto correction_timeout = state.profile.limits.query_timeout;
      const auto success_target = correction_peers.size();
      const auto selected = *result.selected;
      static_cast<void>(self->launch_tracked([self, protocol = std::move(protocol), key = std::move(key), selected,
                                              peers = std::move(correction_peers), concurrency, success_target,
                                              correction_timeout]() mutable -> boost::asio::awaitable<void> {
         try {
            static_cast<void>(co_await detail::dht_fanout::run(
                self->runtime.context(),
                detail::dht_fanout::request{
                    .peers = std::move(peers),
                    .concurrency = concurrency,
                    .success_target = success_target,
                    .timeout = correction_timeout,
                    .operation = "P2P DHT value correction",
                },
                [self, protocol, key, selected](const peer_id& peer, std::chrono::milliseconds timeout,
                                                std::shared_ptr<cancellation_latch> cancellation)
                    -> boost::asio::awaitable<bool> {
                   try {
                      static_cast<void>(co_await self->exchange_dht(protocol, peer,
                                                                    dht::message{.type = dht::message_type::put_value,
                                                                                 .key_value = key,
                                                                                 .record_value = selected},
                                                                    timeout, std::move(cancellation)));
                      co_return true;
                   } catch (const forge::exceptions::base& error) {
                      if (!remote_peer_attributable_failure(self->mutex, self->stopped, error)) {
                         throw;
                      }
                      co_return false;
                   }
                }));
         } catch (...) {
            // Value correction is best-effort and must not revoke an accepted quorum.
         }
      }));
   }
   co_return result;
}

} // namespace

boost::asio::awaitable<dht::query_result>
node::impl::async_find_dht_peer(protocol_id protocol, peer_id peer, dht::query_options options,
                                std::optional<std::size_t> alpha_limit,
                                std::shared_ptr<cancellation_latch> cancellation) {
   co_return co_await async_find_peer_owned(shared_from_this(), std::move(protocol), std::move(peer), options,
                                             alpha_limit, std::move(cancellation));
}

boost::asio::awaitable<dht::query_result> node::async_find_peer(protocol_id protocol, peer_id peer,
                                                                dht::query_options options) {
   return impl_->async_find_dht_peer(std::move(protocol), std::move(peer), options);
}

boost::asio::awaitable<provider_registration> node::async_provide(protocol_id protocol, dht::key key,
                                                                  dht::query_options options) {
   return async_provide_owned(impl_, std::move(protocol), std::move(key), options);
}

boost::asio::awaitable<std::vector<dht::peer>> node::async_find_providers(protocol_id protocol, dht::key key,
                                                                          dht::query_options options) {
   return async_find_providers_owned(impl_, std::move(protocol), std::move(key), options);
}

boost::asio::awaitable<dht::value_put_result> node::async_put_value(protocol_id protocol, dht::record value,
                                                                    dht::query_options options) {
   return async_put_value_owned(impl_, std::move(protocol), std::move(value), options);
}

boost::asio::awaitable<dht::value_get_result> node::async_get_value(protocol_id protocol, dht::key key,
                                                                    dht::query_options options) {
   return async_get_value_owned(impl_, std::move(protocol), std::move(key), options);
}

ipns::record node::create_ipns_record(std::span<const std::uint8_t> value, std::uint64_t sequence, forge::chrono::timestamp eol,
                                      std::chrono::nanoseconds ttl, ipns::create_options options) const {
   const auto self = impl_;
   if (!self) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is closed");
   }
   const auto& private_key = require_libp2p_identity_private_key(self->identity);
   const auto public_key = decode_public_key(self->identity.public_key);
   return ipns::create(
       public_key,
       [&private_key](std::span<const std::uint8_t> message) { return sign_identity(private_key, message); }, value,
       sequence, eol, ttl, std::move(options));
}

} // namespace forge::net::p2p
