module;

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <string>

export module forge.plugins.crypto.secrets.plugin;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.plugins.crypto.secrets.types;

export namespace forge::plugins::crypto::secrets {

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

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   void on_request_stop() noexcept override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   struct impl;
   class api_impl;
   class version_api_impl;
   class version_write_api_impl;
   class version_metadata_api_impl;

   friend void apply_config(impl&, forge::config::core::component_view);

   std::shared_ptr<impl> impl_;
};

} // namespace forge::plugins::crypto::secrets
