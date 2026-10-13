module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
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
import forge.asio.notification;
import forge.asio.gate;
import forge.asio.runtime;
import forge.asio.task;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/pubsub_service.hxx"
#include "details/configuration_policy.hxx"
#include "details/protocol_registry.hxx"

namespace forge::plugins::net::p2p::node {
plugin::impl::impl(forge::app::plugin_lifecycle state)
    : lifecycle{std::move(state)}, configuration{std::make_shared<const detail::configuration_snapshot>()},
      protocols{detail::make_protocol_registry(configuration->options.limits.max_protocol_handlers, lifecycle)},
      contributions{std::make_shared<detail::peer_contributions>()},
      pubsub{std::make_shared<detail::pubsub_service>(lifecycle)} {}
plugin::impl::~impl() = default;

void plugin::impl::request_stop() noexcept {
   contributions->close();
   pubsub->request_stop();
   detail::request_close_protocol_registry(protocols);
   if (auto current = node_snapshot()) {
      try {
         current->request_stop();
      } catch (...) {
         // Awaited shutdown retries and reports a synchronous initiation failure.
      }
   }
}

bool plugin::impl::is_started() const noexcept {
   return lifecycle.snapshot().phase == forge::app::lifecycle_phase::started;
}

std::shared_ptr<forge::net::p2p::node> plugin::impl::ensure_node(forge::net::p2p::node::options options) {
   const auto generation = lifecycle.snapshot().generation;
   if (!runtime) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not initialized");
   }
   auto current = std::shared_ptr<forge::net::p2p::node>{};
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      current = std::atomic_load_explicit(&node, std::memory_order_acquire);
   }
   // begin_startup owns the only construction attempt and freezes contributions.
   // Neither native attachment nor retiring user captures may hold the config lock.
   if (!current) {
      const auto topology = options.limits.topology;
      auto candidate = std::make_shared<forge::net::p2p::node>(*runtime, std::move(options));
      try {
         detail::attach_protocol_registry(protocols, candidate);
         contributions->attach(candidate, topology);
      } catch (...) {
         candidate->stop();
         throw;
      }
      if (!lifecycle.try_admit(generation, [&] {
         std::atomic_store_explicit(&node, candidate, std::memory_order_release);
      })) {
         candidate->stop();
         FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P node startup was stopped");
      }
      current = std::move(candidate);
   }
   if (lifecycle.snapshot().stop_requested) {
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
   const auto state = lifecycle.snapshot();
   if (state.stop_requested) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P node plugin admission is closed");
   }
   if (state.phase != forge::app::lifecycle_phase::started) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not running");
   }
   auto current = std::shared_ptr<forge::net::p2p::node>{};
   if (!lifecycle.try_admit(state.generation, forge::app::lifecycle_phase::started,
                            [&] { current = node_snapshot(); })) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P node plugin admission is closed");
   }
   if (!current) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P node plugin is not running");
   }
   return current;
}

forge::net::p2p::node::open_options plugin::impl::open_options_for(remote_options value) const {
   if (value.open_deadline.count() <= 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P remote open deadline must be positive");
   }
   const auto snapshot = configuration_snapshot();
   const auto& policy = snapshot->policy;
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
   auto out = configuration_snapshot()->api_options;
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
