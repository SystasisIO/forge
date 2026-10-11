module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <map>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include <stop_token>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.net.p2p.pubsub;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::pubsub {

plugin::plugin() : impl_{std::make_shared<impl>()} {}
plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.p2p.pubsub"};
}

std::string plugin::version() const {
   return "2.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.p2p.pubsub");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto config = decode_config(view);
   validate_config(config);
   impl_->configure(std::move(config));
   return impl::ready_owned(impl_);
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   return impl::ready_owned(impl_);
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   auto source = context.apis()
                      .get<forge::plugins::net::p2p::node::pubsub_source>(
                         {.id = {"forge.plugins.net.p2p.node.pubsub_source"}, .major = 2, .min_revision = 0})
                      .shared();
   impl_->initialize(std::move(source));
   return impl::ready_owned(impl_);
}

boost::asio::awaitable<void> plugin::startup() {
   return impl::ready_owned(impl_);
}

void plugin::request_stop() noexcept {
   impl_->request_stop();
}

boost::asio::awaitable<void> plugin::shutdown() {
   return impl::shutdown_owned(impl_);
}

forge::app::plugin_descriptor descriptor() {
   return forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "forge.plugins.net.p2p.pubsub"},
      .dependencies = {forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"}},
      .factory = [] {
         return std::make_unique<plugin>();
      },
   };
}

} // namespace forge::plugins::net::p2p::pubsub
