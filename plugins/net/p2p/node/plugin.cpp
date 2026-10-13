module;

#include <forge/exceptions/macros.hpp>
#include <forge/db/object/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <stop_token>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.api.transport.exceptions;
import forge.api.transport.options;
import forge.api.transport.client;
import forge.api.transport.connection;
import forge.api.transport.server;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.asio.notification;
import forge.asio.gate;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.decode;
import forge.crypto.core.types;
import forge.crypto.core.secret_bytes;
import forge.db.object.index;
import forge.db.object.object;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.identify;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.rendezvous;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.hole_punch;
import forge.net.p2p.protocol;
import forge.net.p2p.message;
import forge.net.p2p.scoring;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.node;
import forge.net.pnet.protector;
import forge.net.p2p.topology;
import forge.multiformats.multiaddr;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.events_api;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.ipns;
import forge.net.p2p.provider_registration;
import forge.chrono.timestamp;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/configuration_policy.hxx"
#include "details/api_impl.hxx"
#include "details/dht_api_impl.hxx"
#include "details/configuration_api_impl.hxx"
#include "details/protocol_api_impl.hxx"
#include "details/object_dht_record_store_adapter.hxx"
#include "details/object_peer_state_adapter.hxx"
#include "details/p2p_state_schema.hxx"
#include "details/plugin_diagnostics_api_impl.hxx"
#include "details/plugin_events_api_impl.hxx"
#include "details/plugin_impl.hxx"
#include "details/pubsub_service.hxx"
#include "details/protocol_registry.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

void clear_text(std::string& value) noexcept {
   forge::crypto::core::secure_erase(value);
}

} // namespace

plugin::plugin() : impl_{std::make_shared<impl>()} {}
plugin::~plugin() { request_stop(); }

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"};
}

std::string plugin::version() const {
   return "8.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.p2p.node");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   const auto config = decode_config(view);
   apply_config(*impl_, config);
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   provider.install<dht_api>(std::make_shared<dht_api_impl>(impl_));
   provider.install<configuration_api>(std::make_shared<configuration_api_impl>(impl_));
   provider.install<protocol_api>(std::make_shared<protocol_api_impl>(impl_));
   provider.install<diagnostics_api>(std::make_shared<diagnostics_api_impl>(impl_));
   provider.install<events_api>(std::make_shared<events_api_impl>(impl_));
   provider.install<pubsub_api>(impl_->pubsub->interface());
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   const auto previous = impl_->configuration_candidate_base();
   auto* runtime = &context.scheduler().runtime_context();
   auto* stores = static_cast<forge::plugins::db::store::api*>(nullptr);
   auto* secrets = static_cast<forge::plugins::crypto::secrets::api*>(nullptr);
   if (!previous->peer_store_name.empty()) {
      stores = context.apis()
                   .get<forge::plugins::db::store::api>({.id = {"forge.plugins.db.store"}, .major = 2, .min_revision = 0})
                   .operator->();
   }
   if (!previous->certificate_secret.empty() || previous->declarative.private_network) {
      secrets = context.apis()
                    .get<forge::plugins::crypto::secrets::api>(
                        {.id = {"forge.plugins.crypto.secrets"}, .major = 1, .min_revision = 0})
                    .operator->();
   }
   {
      const auto lock = std::scoped_lock{impl_->configuration_mutex};
      if (impl_->configuration != previous || impl_->providers_bound ||
          impl_->stop_requested.load(std::memory_order_acquire) ||
          impl_->phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P configuration changed during dependency binding");
      }
      detail::bind_protocol_registry_executor(impl_->protocols, runtime->context().get_executor());
      impl_->runtime = runtime;
      impl_->stores = stores;
      impl_->secrets = secrets;
      impl_->providers_bound = true;
   }
   co_return;
}

boost::asio::awaitable<void> plugin::after_initialize() {
   const auto state = impl_;
   auto active = false;
   auto joined = boost::scope::scope_exit{[&] { if (active) { state->finish_setup(); } }};
   const auto previous = state->configuration_candidate_base();
   auto name = previous->peer_store_name;
   {
      const auto lock = std::scoped_lock{state->configuration_mutex};
      if (state->configuration != previous || !state->providers_bound || state->bound_peer_store_name ||
          state->stop_requested.load(std::memory_order_acquire) ||
          state->phase.load(std::memory_order_relaxed) != lifecycle_phase::idle) {
         FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P peer store binding changed or is frozen");
      }
      state->bound_peer_store_name.emplace(std::move(name));
      ++state->setup_operations;
      active = true;
   }
   if (previous->peer_store_name.empty()) {
      if (!previous->options.allow_insecure_test_mode) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P peer store is not configured");
      }
      co_return;
   }
   auto store = std::make_shared<forge::plugins::db::store::store_handle>(
       co_await state->stores->store(previous->peer_store_name));
   detail::p2p_state_schema::register_objects(*store);
   state->peer_state_store = std::move(store);
}

boost::asio::awaitable<void> plugin::startup() {
   const auto state = impl_;
   auto active = false;
   auto joined = boost::scope::scope_exit{[&] { if (active) { state->finish_setup(); } }};
   auto options = forge::net::p2p::node::options{};
   auto frozen = std::shared_ptr<const detail::configuration_snapshot>{};
   if (!state->begin_startup(options, frozen)) { co_return; }
   active = true;
   auto private_key_cleanup = boost::scope::scope_exit{[&] {
      clear_text(options.certificate_pem);
      clear_text(options.private_key_pem);
   }};
   if (!frozen->certificate_secret.empty()) {
      auto certificate_result = co_await state->secrets->get_bytes(
          {.secret_id = frozen->certificate_secret, .purpose = "p2p.identity.certificate"});
      auto certificate = forge::crypto::core::secret_bytes{std::move(certificate_result.bytes)};
      if (state->stop_requested.load(std::memory_order_acquire)) { co_return; }
      auto private_key_result = co_await state->secrets->get_bytes(
          {.secret_id = frozen->private_key_secret, .purpose = "p2p.identity.private-key"});
      auto private_key = forge::crypto::core::secret_bytes{std::move(private_key_result.bytes)};
      if (state->stop_requested.load(std::memory_order_acquire)) { co_return; }
      options.certificate_pem = std::string{certificate.span().begin(), certificate.span().end()};
      options.private_key_pem = std::string{private_key.span().begin(), private_key.span().end()};
   }
   if (frozen->declarative.private_network) {
      auto result = co_await state->secrets->get_bytes({
          .secret_id = frozen->declarative.private_network->psk_secret, .purpose = "p2p.private-network.psk"});
      auto material = forge::crypto::core::secret_bytes{std::move(result.bytes)};
      if (state->stop_requested.load(std::memory_order_acquire)) { co_return; }
      if (material.size() != forge::net::pnet::pre_shared_key_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P private-network PSK secret must decode to exactly 32 bytes");
      }
      options.private_network->protector = std::make_shared<const forge::net::pnet::protector>(
          forge::net::pnet::pre_shared_key{material.span()});
   }
   if (state->stop_requested.load(std::memory_order_acquire)) { co_return; }
   if (state->peer_state_store) {
      state->peer_state = co_await object_peer_state_adapter::async_open(
          state->stores, *state->peer_state_store, options.peer_state, frozen->reset_incompatible_peer_state);
      options.peer_state.persistence = state->peer_state;

      auto open_failure = std::exception_ptr{};
      try {
         for (const auto& profile : options.dht_profiles) {
            auto& records = options.dht_records.try_emplace(
                profile.protocol, forge::net::p2p::dht::record_store::options_for(profile)).first->second;
            auto persistence = co_await object_dht_record_store_adapter::async_open(
                state->stores, *state->peer_state_store, profile.protocol, records);
            records.persistence = persistence;
            state->dht_state.emplace_back(profile.protocol, std::move(persistence));
         }
      } catch (...) {
         open_failure = std::current_exception();
      }
      if (open_failure) {
         auto cleanup_failure = std::exception_ptr{};
         for (auto& [_, persistence] : state->dht_state) {
            try {
               co_await persistence->async_close();
            } catch (...) {
               if (!cleanup_failure) {
                  cleanup_failure = std::current_exception();
               }
            }
         }
         state->dht_state.clear();
         options.dht_records.clear();
         try {
            co_await state->peer_state->async_close();
         } catch (...) {
            if (!cleanup_failure) {
               cleanup_failure = std::current_exception();
            }
         }
         state->peer_state.reset();
         options.peer_state.persistence.reset();
         static_cast<void>(cleanup_failure);
         std::rethrow_exception(open_failure);
      }
   }

   if (state->stop_requested.load(std::memory_order_acquire)) {
      co_return;
   }

   auto node = state->ensure_node(std::move(options));
   if (state->stop_requested.load(std::memory_order_acquire)) {
      node->stop();
      co_return;
   }
   auto startup_failure = std::exception_ptr{};
   try {
      static_cast<void>(co_await node->async_start());
   } catch (...) {
      startup_failure = std::current_exception();
   }
   if (startup_failure) {
      if (!state->stop_requested.load(std::memory_order_acquire)) {
         std::rethrow_exception(startup_failure);
      }
      co_await node->async_stop();
      co_return;
   }
   if (state->stop_requested.load(std::memory_order_acquire)) {
      node->request_stop();
      co_await node->async_stop();
      co_return;
   }
   state->pubsub->initialize(frozen->pubsub, node);
   state->mark_started();
}

void plugin::request_stop() noexcept {
   impl_->request_stop();
}

boost::asio::awaitable<void> plugin::shutdown() {
   // The child owns cleanup independently of cancellation of its caller. The
   // explicit argument keeps state alive without a capturing coroutine lambda.
   co_await boost::asio::co_spawn(
       co_await boost::asio::this_coro::executor,
       [](std::shared_ptr<impl> state) -> boost::asio::awaitable<void> {
          co_await boost::asio::this_coro::throw_if_cancelled(false);
          co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
          auto candidate = std::make_shared<detail::shutdown_attempt>();
          auto attempt = std::shared_ptr<detail::shutdown_attempt>{};
          auto leader = false;
          {
             const auto lock = std::scoped_lock{state->configuration_mutex};
             if (!state->shutdown || (state->shutdown->done && !state->shutdown->terminal)) {
                state->shutdown.swap(candidate);
                leader = true;
             }
             attempt = state->shutdown;
          }
          // Retire the previous attempt (including any exception captures) unlocked.
          candidate.reset();
          if (!leader) {
             for (;;) {
                const auto epoch = attempt->completed.epoch();
                auto done = false;
                auto error = std::exception_ptr{};
                {
                   const auto lock = std::scoped_lock{state->configuration_mutex};
                   done = attempt->done;
                   error = attempt->error;
                }
                if (done) {
                   if (error) { std::rethrow_exception(error); }
                   co_return;
                }
                co_await attempt->completed.async_wait(epoch);
             }
          }
          auto failure = std::exception_ptr{};
          auto registry_closed = false;
          auto peer_state_closed = false;
          try {
             state->request_stop();
             // Setup owns every opened adapter until this joined barrier. Capture epoch
             // before the predicate so completion cannot be lost before wait registration.
             for (;;) {
                const auto epoch = state->setup_changed.epoch();
                {
                   const auto lock = std::scoped_lock{state->configuration_mutex};
                   if (state->setup_operations == 0) {
                      break;
                   }
                }
                co_await state->setup_changed.async_wait(epoch);
             }
             try {
                co_await detail::pubsub_service::shutdown_owned(state->pubsub);
             } catch (...) {
                failure = std::current_exception();
             }
             try {
                co_await detail::async_close_protocol_registry(state->protocols);
                registry_closed = true;
             } catch (...) {
                if (!failure) { failure = std::current_exception(); }
             }
             auto node = state->node_snapshot();
             peer_state_closed = !node && !state->peer_state && state->dht_state.empty();
             if (node) {
                try {
                   co_await node->async_stop();
                   peer_state_closed = true;
                } catch (...) {
                   if (!failure) {
                      failure = std::current_exception();
                   }
                }
             } else {
                peer_state_closed = true;
                for (auto& [_, persistence] : state->dht_state) {
                   try {
                      co_await persistence->async_close();
                   } catch (...) {
                      peer_state_closed = false;
                      if (!failure) {
                         failure = std::current_exception();
                      }
                   }
                }
                if (state->peer_state) {
                   try {
                      co_await state->peer_state->async_close();
                   } catch (...) {
                      peer_state_closed = false;
                      if (!failure) {
                         failure = std::current_exception();
                      }
                   }
                }
             }
             if (registry_closed && peer_state_closed) {
                auto empty = std::shared_ptr<forge::net::p2p::node>{};
                std::atomic_compare_exchange_strong_explicit(&state->node, &node, empty, std::memory_order_acq_rel,
                                                             std::memory_order_acquire);
                state->peer_state.reset();
                state->dht_state.clear();
                state->peer_state_store.reset();
                state->stores = nullptr;
                state->secrets = nullptr;
             }
             if (registry_closed && peer_state_closed) {
                state->mark_stopped();
             }
          } catch (...) {
             if (!failure) { failure = std::current_exception(); }
          }
          {
             const auto lock = std::scoped_lock{state->configuration_mutex};
             attempt->error = failure;
             attempt->terminal = registry_closed && peer_state_closed;
             attempt->done = true;
          }
          attempt->completed.notify();
          if (failure) { std::rethrow_exception(failure); }
       }(impl_),
       boost::asio::use_awaitable);
}

forge::app::plugin_descriptor plugin::descriptor() {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"},
       .dependencies =
           {
               forge::app::plugin_id{.value = "forge.plugins.db.store"},
               forge::app::plugin_id{.value = "forge.plugins.crypto.secrets"},
           },
       .factory = [] { return std::make_unique<plugin>(); },
   };
}

} // namespace forge::plugins::net::p2p::node
