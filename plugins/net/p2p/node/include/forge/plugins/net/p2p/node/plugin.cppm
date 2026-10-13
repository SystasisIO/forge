module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.node.plugin;

import forge.plugins.net.p2p.node.config;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;

namespace forge::plugins::net::p2p::node::detail {
struct pubsub_service;
}

export namespace forge::plugins::net::p2p::node {

class plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] static forge::app::plugin_descriptor descriptor();

   plugin();
   ~plugin() override;

   plugin(const plugin&) = delete;
   plugin& operator=(const plugin&) = delete;

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;
   [[nodiscard]] std::optional<forge::config::core::component_descriptor> describe_config() const override;
   boost::asio::awaitable<void> configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> after_initialize() override;
   boost::asio::awaitable<void> startup() override;
   void request_stop() noexcept override;
   boost::asio::awaitable<void> shutdown() override;

 private:
   struct impl;
   class api_impl;
   class dht_api_impl;
   class configuration_api_impl;
   class protocol_api_impl;
   class diagnostics_api_impl;
   class events_api_impl;
   class pubsub_api_impl;

   friend void apply_config(impl&, const config&);
   friend struct detail::pubsub_service;

   std::shared_ptr<impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
