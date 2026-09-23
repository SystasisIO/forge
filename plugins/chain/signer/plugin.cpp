module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.chain.signer.plugin;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.asio.notification;
import forge.chain.api.finality_signer;
import forge.chain.api.block_signer;
import forge.chain.api.transaction_signer;
import forge.chain.protocol.block_signing;
import forge.config.core.decode;
import forge.plugins.chain.signer.exceptions;
import forge.plugins.chain.signer.types;

#include "details/config.hxx"
#include "details/block_api_impl.hxx"
#include "details/finality_api_impl.hxx"
#include "details/plugin_impl.hxx"
#include "details/transaction_api_impl.hxx"

namespace forge::plugins::chain::signer {

plugin::plugin(plugin_options value) : impl_{std::make_shared<impl>(std::move(value))} {}

plugin::~plugin() = default;

forge::app::plugin_descriptor descriptor(plugin_options value) {
   return forge::app::plugin_descriptor{
       .id = {.value = "forge.plugins.chain.signer"},
       .dependencies = {},
       .enabled_by_default = false,
       .factory = [options = std::move(value)] { return std::make_unique<plugin>(options); },
   };
}

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.chain.signer"};
}

std::string plugin::version() const {
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.chain.signer");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   impl_->set_config(decode_config(view));
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<forge::chain::api::transaction_signer>(std::make_shared<transaction_api_impl>(impl_));
   provider.install<forge::chain::api::block_signer>(std::make_shared<block_api_impl>(impl_));
   provider.install<forge::chain::api::finality_signer>(std::make_shared<finality_api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context&) {
   impl_->initialize();
   co_return;
}

boost::asio::awaitable<void> plugin::startup() {
   if (impl_->stopping()) {
      FORGE_THROW_EXCEPTION(forge::plugins::chain::signer::exceptions::invalid_lifecycle,
                            "Chain signer plugin was stopped before startup");
   }
   co_return;
}

void plugin::request_stop() noexcept {
   impl_->request_stop();
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   co_await impl_->wait_for_drain();
}

} // namespace forge::plugins::chain::signer
