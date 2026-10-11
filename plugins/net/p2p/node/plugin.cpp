module;

#include <forge/exceptions/macros.hpp>
#include <forge/db/object/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <list>
#include <memory>
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
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.host_event_source;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.ipns;
import forge.net.p2p.provider_registration;
import forge.chrono.timestamp;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/config.hxx"
#include "details/api_impl.hxx"
#include "details/dht_api_impl.hxx"
#include "details/object_dht_record_store_adapter.hxx"
#include "details/object_peer_state_adapter.hxx"
#include "details/p2p_state_schema.hxx"
#include "details/plugin_diagnostics_source_adapter.hxx"
#include "details/plugin_host_event_source_adapter.hxx"
#include "details/plugin_impl.hxx"
#include "details/plugin_pubsub_source_adapter.hxx"

namespace forge::plugins::net::p2p::node {
namespace {

[[nodiscard]] std::string secret_text(forge::crypto::core::bytes& bytes) {
   auto out = std::string{bytes.begin(), bytes.end()};
   forge::crypto::core::secure_erase(bytes);
   return out;
}

void clear_bytes(forge::crypto::core::bytes& bytes) noexcept {
   forge::crypto::core::secure_erase(bytes);
}

void clear_text(std::string& value) noexcept {
   forge::crypto::core::secure_erase(value);
}

} // namespace

plugin::plugin() : impl_{std::make_shared<impl>()} {}
plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"};
}

std::string plugin::version() const {
   return "7.0.0";
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
   provider.install<diagnostics_source>(std::make_shared<diagnostics_source_adapter>(impl_));
   provider.install<host_event_source>(std::make_shared<host_event_source_adapter>(impl_));
   provider.install<pubsub_source>(std::make_shared<pubsub_source_adapter>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   impl_->runtime = &context.scheduler().runtime_context();
   detail::bind_api_publication_registry_executor(
      impl_->api_publications, impl_->runtime->context().get_executor());
   if (!impl_->peer_store_name.empty()) {
      impl_->stores =
          context.apis()
              .get<forge::plugins::db::store::api>({.id = {"forge.plugins.db.store"}, .major = 2, .min_revision = 0})
              .operator->();
   }
   if (!impl_->certificate_secret.empty()) {
      impl_->secrets = context.apis()
                           .get<forge::plugins::crypto::secrets::api>(
                               {.id = {"forge.plugins.crypto.secrets"}, .major = 1, .min_revision = 0})
                           .operator->();
   }
   co_return;
}

boost::asio::awaitable<void> plugin::after_initialize() {
   if (impl_->peer_store_name.empty()) {
      if (!impl_->options.allow_insecure_test_mode) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P peer store is not configured");
      }
      co_return;
   }
   impl_->peer_state_store =
       std::make_shared<forge::plugins::db::store::store_handle>(co_await impl_->stores->store(impl_->peer_store_name));
   detail::p2p_state_schema::register_objects(*impl_->peer_state_store);
}

boost::asio::awaitable<void> plugin::startup() {
   auto routes = impl_->begin_startup();
   if (!routes) {
      co_return;
   }
   auto private_key_cleanup = boost::scope::scope_exit{[this] { clear_text(impl_->options.private_key_pem); }};
   if (!impl_->certificate_secret.empty()) {
      auto certificate = co_await impl_->secrets->get_bytes(
          {.secret_id = impl_->certificate_secret, .purpose = "p2p.identity.certificate"});
      try {
         auto private_key = co_await impl_->secrets->get_bytes(
             {.secret_id = impl_->private_key_secret, .purpose = "p2p.identity.private-key"});
         try {
            impl_->options.certificate_pem = secret_text(certificate.bytes);
            impl_->options.private_key_pem = secret_text(private_key.bytes);
         } catch (...) {
            clear_bytes(private_key.bytes);
            clear_text(impl_->options.certificate_pem);
            clear_text(impl_->options.private_key_pem);
            throw;
         }
      } catch (...) {
         clear_bytes(certificate.bytes);
         throw;
      }
   }
   if (impl_->peer_state_store) {
      impl_->peer_state = co_await object_peer_state_adapter::async_open(
          impl_->stores, *impl_->peer_state_store, impl_->options.peer_state, impl_->reset_incompatible_peer_state);
      impl_->options.peer_state.persistence = impl_->peer_state;

      auto open_failure = std::exception_ptr{};
      try {
         for (const auto& profile : impl_->options.dht_profiles) {
            auto persistence = co_await object_dht_record_store_adapter::async_open(
                impl_->stores, *impl_->peer_state_store, profile.protocol,
                forge::net::p2p::dht::record_store::options{
                    .max_providers_per_key = profile.limits.replication,
                    .max_record_bytes = profile.limits.max_record_size,
                });
            impl_->options.dht_record_persistence.emplace(profile.protocol, persistence);
            impl_->dht_state.emplace_back(profile.protocol, std::move(persistence));
         }
      } catch (...) {
         open_failure = std::current_exception();
      }
      if (open_failure) {
         auto cleanup_failure = std::exception_ptr{};
         for (auto& [_, persistence] : impl_->dht_state) {
            try {
               co_await persistence->async_close();
            } catch (...) {
               if (!cleanup_failure) {
                  cleanup_failure = std::current_exception();
               }
            }
         }
         impl_->dht_state.clear();
         impl_->options.dht_record_persistence.clear();
         try {
            co_await impl_->peer_state->async_close();
         } catch (...) {
            if (!cleanup_failure) {
               cleanup_failure = std::current_exception();
            }
         }
         impl_->peer_state.reset();
         impl_->options.peer_state.persistence.reset();
         static_cast<void>(cleanup_failure);
         std::rethrow_exception(open_failure);
      }
   }

   if (impl_->stop_requested.load(std::memory_order_acquire)) {
      co_return;
   }

   auto node = impl_->ensure_node(*routes);
   if (impl_->stop_requested.load(std::memory_order_acquire)) {
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
      if (!impl_->stop_requested.load(std::memory_order_acquire)) {
         std::rethrow_exception(startup_failure);
      }
      co_await node->async_stop();
      co_return;
   }
   if (impl_->stop_requested.load(std::memory_order_acquire)) {
      node->request_stop();
      co_await node->async_stop();
      co_return;
   }
   impl_->mark_started();
}

void plugin::request_stop() noexcept {
   impl_->stop_requested.store(true, std::memory_order_release);
   impl_->phase.store(lifecycle_phase::stopping, std::memory_order_release);
   detail::request_close_api_publication_registry(impl_->api_publications);
   if (auto node = impl_->node_snapshot()) {
      try {
         node->request_stop();
      } catch (...) {
         // shutdown() retries and reports a synchronous initiation failure.
      }
   }
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();
   auto failure = std::exception_ptr{};
   try {
      co_await detail::async_close_api_publication_registry(impl_->api_publications);
   } catch (...) {
      failure = std::current_exception();
   }
   auto node = impl_->node_snapshot();
   auto peer_state_closed = !node && !impl_->peer_state && impl_->dht_state.empty();
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
      for (auto& [_, persistence] : impl_->dht_state) {
         try {
            co_await persistence->async_close();
         } catch (...) {
            peer_state_closed = false;
            if (!failure) {
               failure = std::current_exception();
            }
         }
      }
      if (impl_->peer_state) {
         try {
            co_await impl_->peer_state->async_close();
         } catch (...) {
            peer_state_closed = false;
            if (!failure) {
               failure = std::current_exception();
            }
         }
      }
   }
   if (peer_state_closed) {
      auto empty = std::shared_ptr<forge::net::p2p::node>{};
      std::atomic_compare_exchange_strong_explicit(&impl_->node, &node, empty, std::memory_order_acq_rel,
                                                   std::memory_order_acquire);
      impl_->peer_state.reset();
      impl_->options.peer_state.persistence.reset();
      impl_->dht_state.clear();
      impl_->options.dht_record_persistence.clear();
      impl_->peer_state_store.reset();
      clear_text(impl_->options.certificate_pem);
      clear_text(impl_->options.private_key_pem);
      impl_->stores = nullptr;
      impl_->secrets = nullptr;
   }
   if (peer_state_closed) {
      impl_->mark_stopped();
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
}

forge::app::plugin_descriptor descriptor() {
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
