module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/error.hpp>
#include <boost/system/system_error.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <functional>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>
#include <vector>

module forge.plugins.chain.outbox.plugin;

import forge.api.chain.outbox;
import forge.api.chain.runtime;
import forge.api.core.exceptions;
import forge.asio.exceptions;
import forge.asio.notification;
import forge.asio.task;
import forge.chain.protocol.info;
import forge.chain.transaction.types;
import forge.chain.api.verified_client_factory;
import forge.chain.transaction.builder;
import forge.config.core.decode;
import forge.config.core.document;
import forge.exceptions;

import forge.net.http.base_url;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::chain::outbox {

plugin::plugin(plugin_options options) : _impl{std::make_shared<impl>(std::move(options))} {}
plugin::~plugin() = default;

forge::app::plugin_descriptor plugin::descriptor(plugin_options options) {
   auto descriptor = forge::app::plugin_descriptor{
       .id = {.value = "forge.plugins.chain.outbox"},
       .dependencies = {{.value = "forge.plugins.db.store"}},
   };
   if (options.p2p) {
      descriptor.dependencies.push_back({.value = "forge.plugins.net.p2p.resolver"});
   }
   descriptor.factory = [options = std::move(options)] { return std::make_unique<plugin>(options); };
   return descriptor;
}

forge::app::plugin_id plugin::id() const { return {.value = "forge.plugins.chain.outbox"}; }
std::string plugin::version() const { return "1.0.0"; }

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   auto result = forge::config::core::describe_component<config>("plugins.chain.outbox");
   const auto defaults = forge::config::core::encode(
       config{.store = _impl->options.store, .http = _impl->options.http}, result.section);
   for (auto& field : result.fields) {
      if ((field.name == "store" && !_impl->options.store.empty()) ||
          (field.name == "http" && !_impl->options.http.empty())) {
         if (const auto* value = defaults.try_get(result.section + "." + field.name)) {
            field.has_default = true;
            field.default_value = *value;
         }
      }
   }
   return result;
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   auto document = forge::config::core::merge({
       forge::config::core::encode(config{.store = _impl->options.store, .http = _impl->options.http}, view.section()),
       view.source()});
   auto decoded = forge::config::core::decode<config>(document, view.section());
   if (!decoded.ok()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Invalid chain outbox configuration");
   }
   if (!_impl->options.verification.finality) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "Chain outbox requires a finality verifier");
   }
   if (!_impl->options.verification.projections) {
      _impl->options.verification.projections = std::make_shared<forge::chain::api::projection_verifier>();
   }
   _impl->options.store = std::move(decoded.value.store);
   _impl->configure_http(std::move(decoded.value.http));
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<forge::api::chain::runtime>(std::make_shared<api_impl>(_impl));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   _impl->apis = context.apis();
   _impl->scheduler = &context.scheduler();
   co_return;
}

boost::asio::awaitable<void> plugin::on_after_initialize() {
   _impl->resolve_signer();
   co_await _impl->open(_impl->connect());
}

boost::asio::awaitable<void> plugin::on_startup() {
   auto state = _impl;
   state->worker = state->scheduler->submit(forge::asio::task::awaitable{
       .name = "chain.outbox",
       .work = [state](forge::asio::task::context& task) { return state->recover(task); },
   });
   if (!state->worker.accepted()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Chain outbox recovery task was rejected");
   }
   state->running = true;
   co_return;
}

void plugin::on_request_stop() noexcept {
   _impl->running = false;
   _impl->stop.request_stop();
}

boost::asio::awaitable<void> plugin::on_shutdown() {
   co_await _impl->drain();
   if (_impl->worker.valid()) {
      try {
         co_await _impl->worker.wait();
      } catch (const forge::asio::exceptions::canceled&) {
         if (!_impl->stop.stop_requested()) {
            throw;
         }
      } catch (const boost::system::system_error& error) {
         if (!_impl->stop.stop_requested() || error.code() != boost::asio::error::operation_aborted) {
            throw;
         }
      }
   }
}

} // namespace forge::plugins::chain::outbox
