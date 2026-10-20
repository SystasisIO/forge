module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.db.store.api;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.api.core.binding;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.dht;
import forge.multiformats.multiaddr;
import forge.swarm.storage.catalog;
import forge.swarm.runtime.engine;
#include "details/plugin_impl.hxx"
#include "details/catalog_access.hxx"
#include "details/capability_context.hxx"
#include "details/peer_capabilities.hxx"

namespace forge::plugins::swarm::node {

plugin::impl::impl(forge::app::plugin_lifecycle lifecycle, plugin_options options)
   : lifecycle{std::move(lifecycle)}, options{std::move(options)}, settings{this->options.initial_config} {
   validate_config(settings);
}

std::shared_ptr<forge::swarm::runtime::engine> plugin::impl::require_engine() const {
   auto result = std::shared_ptr<forge::swarm::runtime::engine>{};
   const auto state = lifecycle.snapshot();
   if (!lifecycle.try_admit(state.generation, forge::app::lifecycle_phase::started, [&] {
         const auto lock = std::scoped_lock{mutex}; result = engine;
      }) || !result)
      FORGE_THROW_EXCEPTION(exceptions::not_running, "Swarm plugin is not running");
   return result;
}
std::shared_ptr<forge::swarm::storage::catalog> plugin::impl::require_catalog() const {
   auto result = std::shared_ptr<forge::swarm::storage::catalog>{};
   const auto state = lifecycle.snapshot();
   if (!lifecycle.try_admit(state.generation, forge::app::lifecycle_phase::started, [&] {
         const auto lock = std::scoped_lock{mutex}; result = catalog;
      }))
      FORGE_THROW_EXCEPTION(exceptions::not_running, "Swarm plugin is not running");
   if (!result) FORGE_THROW_EXCEPTION(exceptions::metadata_unavailable, "Swarm file mode has no named metadata catalog");
   return result;
}
std::shared_ptr<peer_capabilities> plugin::impl::require_capabilities() const {
   auto result = std::shared_ptr<peer_capabilities>{};
   const auto state = lifecycle.snapshot();
   if (!lifecycle.try_admit(state.generation, forge::app::lifecycle_phase::started, [&] {
         const auto lock = std::scoped_lock{mutex}; result = capabilities;
      }) || !result)
      FORGE_THROW_EXCEPTION(exceptions::not_running, "Swarm routes are unavailable before startup or after stop");
   return result;
}

void plugin::impl::request_stop() noexcept {
   stop.request_stop();
   auto engine = std::shared_ptr<forge::swarm::runtime::engine>{};
   auto publication = std::shared_ptr<forge::api::p2p::publication>{};
   {
      const auto lock = std::scoped_lock{mutex};
      engine = this->engine;
      publication = this->publication;
   }
   if (publication) publication->close();
   if (engine) engine->cancel();
}

boost::asio::awaitable<void> plugin::impl::start(std::shared_ptr<impl> owner) {
   const auto generation = owner->lifecycle.snapshot().generation;
   auto failure = std::exception_ptr{};
   try {
      auto policy = owner->options.admission;
      if (owner->settings.free_admission && policy)
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm free configuration conflicts with injected admission");
      if (owner->settings.free_admission) policy = std::make_shared<forge::swarm::runtime::free_admission>();
      if (!policy) FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm requires explicit free or product admission");
      if (owner->stop.stop_requested()) FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm startup stopped");
      if (owner->settings.metadata.mode == "object") {
         auto access = std::make_shared<catalog_access>(owner->objects, owner->database);
         auto catalog = co_await forge::swarm::storage::catalog::async_open(std::move(access),
            {.cpu = owner->cpu, .database = owner->options.database,
             .max_inflight = owner->settings.metadata.max_inflight}, owner->stop.get_token());
         const auto lock = std::scoped_lock{owner->mutex};
         owner->catalog = std::move(catalog);
      }
      if (owner->stop.stop_requested()) FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm startup stopped");
      auto context = std::make_shared<const capability_context>(capability_context{
         owner->p2p, owner->dht, {owner->settings.network.profile},
         {owner->settings.network.application_protocol}, owner->stop.get_token()});
      auto capabilities = std::make_shared<peer_capabilities>(std::move(context));
      auto engine = std::make_shared<forge::swarm::runtime::engine>(*owner->scheduler, owner->cpu,
         std::move(policy), capabilities, owner->options.engine_options);
      auto exports = std::make_shared<forge::api::core::registry>();
      exports->install<forge::swarm::api::peer>(engine);
      auto publication = std::make_shared<forge::api::p2p::publication>();
      {
         const auto lock = std::scoped_lock{owner->mutex};
         owner->engine = engine;
         owner->capabilities = std::move(capabilities);
         owner->exports = exports;
      }
      if (owner->stop.stop_requested()) FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm startup stopped");
      auto plan = forge::api::core::binding().serve(*exports)
         .export_api<forge::swarm::api::peer>({.id = {.value = "forge.swarm.peer"}, .major = 1}).build();
      auto transport = forge::api::transport::options{};
      transport.deadline = std::chrono::milliseconds::zero();
      transport.max_inflight = owner->options.engine_options.global.calls;
      transport.max_item_size = forge::swarm::protocol::max_wire_message_size;
      transport.max_frame_size = forge::swarm::protocol::max_wire_message_size + 4096U;
      *publication = owner->p2p->publish_api(std::move(plan), {owner->settings.network.application_protocol}, transport);
      {
         const auto lock = std::scoped_lock{owner->mutex};
         owner->publication = publication;
      }
      // A stop concurrent with synchronous publish may have seen no publication.
      if (owner->stop.stop_requested()) publication->close();
      // Capture this exact engine, not the facade or a mutable current generation.
      auto callback = std::make_unique<std::stop_callback<std::function<void()>>>(
         publication->stop_token(), [engine] { engine->cancel(); });
      {
         const auto lock = std::scoped_lock{owner->mutex};
         owner->publication_stop = std::move(callback);
      }
      if (owner->stop.stop_requested() || !publication->active() || !owner->lifecycle.try_admit(
            generation, forge::app::lifecycle_phase::starting, [] {}))
         FORGE_THROW_EXCEPTION(exceptions::canceled, "Swarm startup lost lifecycle admission");
   } catch (...) { failure = std::current_exception(); }
   if (failure) {
      owner->request_stop();
      auto cleanup = std::exception_ptr{};
      try { co_await close(owner); } catch (...) { cleanup = std::current_exception(); }
      if (cleanup)
         FORGE_THROW_EXCEPTION_WITH_DATA(exceptions::operation_failed,
            (exceptions::failures{failure, cleanup}), "Swarm startup and rollback failed");
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> plugin::impl::close(std::shared_ptr<impl> owner) {
   owner->request_stop();
   auto engine = std::shared_ptr<forge::swarm::runtime::engine>{};
   auto catalog = std::shared_ptr<forge::swarm::storage::catalog>{};
   auto publication = std::shared_ptr<forge::api::p2p::publication>{};
   {
      const auto lock = std::scoped_lock{owner->mutex};
      engine = owner->engine; catalog = owner->catalog; publication = owner->publication;
   }
   auto primary = std::exception_ptr{};
   auto secondary = std::exception_ptr{};
   const auto retain = [&] {
      if (!primary) primary = std::current_exception();
      else if (!secondary) secondary = std::current_exception();
   };
   if (publication) { try { co_await publication->async_close(); } catch (...) { retain(); } }
   if (engine) { try { co_await engine->async_close(); } catch (...) { retain(); } }
   // Engine checkpoint/lease drain always precedes logical catalog close.
   if (catalog) { try { co_await catalog->async_close(); } catch (...) { retain(); } }
   if (secondary)
      FORGE_THROW_EXCEPTION_WITH_DATA(exceptions::operation_failed,
         (exceptions::failures{primary, secondary}), "Swarm shutdown has multiple failures");
   if (primary) std::rethrow_exception(primary);
   auto callback = std::unique_ptr<std::stop_callback<std::function<void()>>>{};
   auto retained = std::tuple{std::shared_ptr<forge::api::core::registry>{}, std::shared_ptr<peer_capabilities>{},
      forge::plugins::db::store::object_handle{}, std::shared_ptr<forge::plugins::db::store::api>{},
      std::shared_ptr<forge::plugins::net::p2p::node::dht_api>{}, std::shared_ptr<forge::plugins::net::p2p::node::api>{}};
   {
      const auto lock = std::scoped_lock{owner->mutex};
      callback = std::move(owner->publication_stop);
      owner->publication.reset(); owner->engine.reset(); owner->catalog.reset();
      retained = std::tuple{std::move(owner->exports), std::move(owner->capabilities), std::move(owner->objects),
         std::move(owner->database), std::move(owner->dht), std::move(owner->p2p)};
      owner->scheduler = nullptr; owner->cpu = {};
   }
   callback.reset();
}

} // namespace forge::plugins::swarm::node
