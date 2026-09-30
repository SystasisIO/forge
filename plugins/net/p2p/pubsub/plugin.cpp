module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.pubsub.plugin;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.pubsub;
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
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.p2p.pubsub");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto config = decode_config(view);
   validate_config(config);
   impl_->settings = std::move(config);
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   impl_->source = context.apis()
                      .get<forge::plugins::net::p2p::node::pubsub_source>(
                         {.id = {"forge.plugins.net.p2p.node.pubsub_source"}, .major = 1, .min_revision = 0})
                      .shared();
   impl_->source->enable(core_options_for(impl_->settings));
   impl_->initialized = true;
   impl_->stopping = false;
   co_return;
}

boost::asio::awaitable<void> plugin::startup() {
   co_return;
}

void plugin::request_stop() noexcept {
   impl_->stopping = true;
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();
   std::vector<forge::net::p2p::pubsub::topic> topics;
   {
      auto lock = std::scoped_lock{impl_->mutex};
      topics.reserve(impl_->topics.size());
      for (const auto& [topic, _] : impl_->topics) {
         topics.push_back(forge::net::p2p::pubsub::topic{.value = topic});
      }
      impl_->topics.clear();
   }
   if (impl_->source) {
      for (auto& topic : topics) {
         try {
            co_await impl_->source->async_leave_topic(std::move(topic));
         } catch (...) {
            forge::exceptions::capture_and_log("P2P PubSub unsubscribe during shutdown failed");
         }
      }
   }
   impl_->initialized = false;
   impl_->source = nullptr;
   co_return;
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
