module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.asio.runtime;
import forge.asio.task;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/config.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

[[nodiscard]] bool contains_protocol(
    const std::vector<std::pair<forge::net::p2p::protocol_id, forge::net::p2p::node::protocol_handler>>& routes,
    const forge::net::p2p::protocol_id& protocol) {
   return std::any_of(routes.begin(), routes.end(), [&](const auto& route) { return route.first == protocol; });
}

} // namespace

plugin::impl::impl() : api_publications{detail::make_api_publication_registry()} {}
plugin::impl::~impl() = default;

std::optional<std::vector<plugin::impl::route>> plugin::impl::begin_startup() {
   auto lock = std::scoped_lock{configuration_mutex};
   if (stop_requested.load(std::memory_order_acquire)) {
      phase.store(lifecycle_phase::stopping, std::memory_order_release);
      return std::nullopt;
   }
   if (phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P node startup has already begun");
   }
   auto startup_routes = routes;
   phase.store(lifecycle_phase::starting, std::memory_order_release);
   return startup_routes;
}

void plugin::impl::mark_started() noexcept {
   auto expected = lifecycle_phase::starting;
   static_cast<void>(phase.compare_exchange_strong(expected, lifecycle_phase::started, std::memory_order_acq_rel,
                                                   std::memory_order_acquire));
}

void plugin::impl::mark_stopped() noexcept {
   phase.store(lifecycle_phase::stopped, std::memory_order_release);
}

bool plugin::impl::is_started() const noexcept {
   return phase.load(std::memory_order_acquire) == lifecycle_phase::started;
}

std::shared_ptr<forge::net::p2p::node> plugin::impl::ensure_node(const std::vector<route>& startup_routes) {
   if (!runtime) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not initialized");
   }
   auto current = std::shared_ptr<forge::net::p2p::node>{};
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      current = std::atomic_load_explicit(&node, std::memory_order_acquire);
      if (!current) {
         if (pubsub_requested) {
            options.capabilities.add(forge::net::p2p::capabilities::pubsub);
            options.limits.pubsub = pubsub_options;
         }
         auto candidate = std::make_shared<forge::net::p2p::node>(*runtime, options);
         for (const auto& route : startup_routes) {
            candidate->register_protocol_handler(route.first, route.second);
         }
         try {
            detail::attach_api_publication_registry(api_publications, candidate);
         } catch (...) {
            candidate->stop();
            throw;
         }
         std::atomic_store_explicit(&node, candidate, std::memory_order_release);
         current = std::move(candidate);
      }
   }
   if (stop_requested.load(std::memory_order_acquire)) {
      current->stop();
   }
   return current;
}

std::shared_ptr<forge::net::p2p::node> plugin::impl::node_snapshot() const noexcept {
   return std::atomic_load_explicit(&node, std::memory_order_acquire);
}

std::shared_ptr<forge::net::p2p::node> plugin::impl::require_node() const {
   auto current = node_snapshot();
   if (!current) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not initialized");
   }
   return current;
}

std::shared_ptr<forge::net::p2p::node> plugin::impl::require_running_node() const {
   const auto started = is_started();
   if (stop_requested.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P node plugin admission is closed");
   }
   if (!started) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not running");
   }
   return require_node();
}

void plugin::impl::add_route(forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) {
   auto lock = std::scoped_lock{configuration_mutex};
   if (phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P routes must be published before startup",
                            forge::exceptions::ctx("protocol", protocol.value));
   }
   if (protocol.value.empty() || !handler) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P route is invalid");
   }
   if (contains_protocol(routes, protocol)) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "duplicate P2P route",
                            forge::exceptions::ctx("protocol", protocol.value));
   }
   if (detail::api_publication_registry_contains(api_publications, protocol)) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P raw route conflicts with an API publication",
                            forge::exceptions::ctx("protocol", protocol.value));
   }
   routes.emplace_back(std::move(protocol), std::move(handler));
}

void plugin::impl::enable_pubsub(forge::net::p2p::pubsub::options options_value) {
   auto lock = std::scoped_lock{configuration_mutex};
   if (phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P PubSub capability must be requested before startup");
   }
   pubsub_options = std::move(options_value);
   pubsub_requested = true;
}

forge::net::p2p::node::open_options plugin::impl::open_options_for(remote_options value) const {
   if (value.open_deadline.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P remote open deadline must be positive");
   }
   return forge::net::p2p::node::open_options{
       .allow_relay = policy.relay_client_enabled && policy.path.allow_relay,
       .timeout = value.open_deadline,
       .direct_attempt_timeout = std::min(std::chrono::milliseconds{2'000}, value.open_deadline),
       .relay_attempt_timeout = std::min(std::chrono::milliseconds{5'000}, value.open_deadline),
       .max_direct_endpoints = policy.path.max_direct_endpoints,
       .max_relay_candidates = policy.path.max_relay_candidates,
       .allow_hole_punch = policy.relay_client_enabled && policy.path.allow_hole_punch,
   };
}

forge::api::transport::options plugin::impl::api_options_for(const remote_options& value) const {
   auto out = api_options;
   if (value.codec.has_value()) {
      if (value.codec->value.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P remote API codec override is invalid");
      }
      out.codec = *value.codec;
   }
   if (value.max_inflight.has_value()) {
      if (*value.max_inflight == 0) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P remote API max inflight override is invalid");
      }
      out.max_inflight = *value.max_inflight;
   }
   if (value.deadline.has_value()) {
      out.deadline = *value.deadline;
   }
   if (value.max_frame_size.has_value()) {
      if (*value.max_frame_size == 0) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P remote API max frame size override is invalid");
      }
      out.max_frame_size = *value.max_frame_size;
   }
   out.max_item_size = std::min(out.max_item_size, out.max_frame_size);
   return out;
}

} // namespace forge::plugins::net::p2p::node
