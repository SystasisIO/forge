module;

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <optional>
#include <string>

export module forge.plugins.chain.outbox.plugin;

export import forge.plugins.chain.outbox.types;

import forge.api.core.registry;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;

export namespace forge::plugins::chain::outbox {

class plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] static forge::app::plugin_descriptor descriptor(plugin_options options);

   explicit plugin(plugin_options options);
   ~plugin() override;

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;
   [[nodiscard]] std::optional<forge::config::core::component_descriptor> describe_config() const override;

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_after_initialize() override;
   boost::asio::awaitable<void> on_startup() override;
   void on_request_stop() noexcept override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   struct impl;
   class api_impl;
   std::shared_ptr<impl> _impl;
};

} // namespace forge::plugins::chain::outbox
