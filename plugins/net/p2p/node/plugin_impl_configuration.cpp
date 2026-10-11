module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <type_traits>
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
import forge.asio.runtime;
import forge.asio.task;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
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
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/config.hxx"
#include "details/protocol_registry.hxx"

namespace forge::plugins::net::p2p::node {

std::shared_ptr<const detail::configuration_snapshot> plugin::impl::configuration_snapshot() const {
   const auto lock = std::scoped_lock{configuration_mutex};
   return configuration;
}

std::shared_ptr<const detail::configuration_snapshot> plugin::impl::configuration_candidate_base() const {
   const auto lock = std::scoped_lock{configuration_mutex};
   if (stop_requested.load(std::memory_order_acquire) || phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P configuration is frozen after startup or stop");
   }
   return configuration;
}

void plugin::impl::commit_configuration(std::shared_ptr<const detail::configuration_snapshot> previous,
                                         std::shared_ptr<const detail::configuration_snapshot> candidate) {
   candidate->validate_candidate();
   auto reserved = candidate->reserved_protocols();
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      if (configuration != previous || stop_requested.load(std::memory_order_acquire) ||
          phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P configuration changed or is frozen");
      }
      if (providers_bound && ((!candidate->peer_store_name.empty() && !stores) ||
                              (!candidate->certificate_secret.empty() && !secrets))) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P configuration requires an unbound provider");
      }
      if (bound_peer_store_name && *bound_peer_store_name != candidate->peer_store_name) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P peer store binding is frozen");
      }
      // The registry commits strongly. Only a nonthrowing pointer swap follows;
      // previous/candidate retain all callback owners until after unlocking.
      detail::configure_protocol_registry(protocols, std::move(reserved), candidate->options.limits.max_protocol_handlers);
      configuration.swap(candidate);
   }
}

bool plugin::impl::begin_startup(forge::net::p2p::node::options& prepared,
                                std::shared_ptr<const detail::configuration_snapshot>& frozen) {
   auto previous = std::shared_ptr<const detail::configuration_snapshot>{};
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      if (stop_requested.load(std::memory_order_acquire)) { return false; }
      if (phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P node startup has already begun");
      }
      if (!providers_bound || !runtime) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P dependencies are not initialized");
      }
      if (!configuration->peer_store_name.empty() &&
          (!bound_peer_store_name || setup_operations != 0 || !peer_state_store)) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P peer store is not initialized");
      }
      previous = configuration;
   }
   auto candidate = previous->prepare_options();
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      if (stop_requested.load(std::memory_order_acquire)) { return false; }
      if (configuration != previous || phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P configuration changed during startup preparation");
      }
      phase.store(lifecycle_phase::starting, std::memory_order_release);
      ++setup_operations;
   }
   static_assert(std::is_nothrow_move_assignable_v<forge::net::p2p::node::options>);
   prepared = std::move(candidate);
   frozen = std::move(previous);
   return true;
}

void plugin::impl::finish_setup() noexcept {
   {
      const auto lock = std::scoped_lock{configuration_mutex};
      --setup_operations;
   }
   setup_changed.notify();
}

void plugin::impl::enable_pubsub(forge::net::p2p::pubsub::options options_value) {
   const auto previous = configuration_candidate_base();
   auto candidate = std::make_shared<detail::configuration_snapshot>(*previous);
   candidate->options.limits.pubsub = std::move(options_value);
   candidate->options.capabilities.add(forge::net::p2p::capabilities::pubsub);
   candidate->pubsub_requested = true;
   commit_configuration(previous, std::move(candidate));
}

} // namespace forge::plugins::net::p2p::node

