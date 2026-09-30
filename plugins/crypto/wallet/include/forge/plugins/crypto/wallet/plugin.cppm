module;

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.crypto.wallet.plugin;

export import forge.plugins.crypto.wallet.config;
export import forge.plugins.crypto.wallet.provider;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.api.core.registry;
import forge.config.core.component;

export namespace forge::plugins::crypto::wallet {

struct plugin_options {
   // Optional pre-created stable providers injected into a Chain signer by the
   // application. Additional management-only wallets are bounded by max_wallets.
   std::vector<std::shared_ptr<provider>> wallets;
   config initial_config;
   // Embedding policy; YAML cannot enable management publication when false.
   bool allow_http_management = true;
};

class plugin final : public forge::app::plugin {
 public:
   explicit plugin(plugin_options options = {});
   ~plugin() override;
   static forge::app::plugin_descriptor descriptor(plugin_options options = {});
   forge::app::plugin_id id() const override;
   std::string version() const override;
   std::optional<forge::config::core::component_descriptor> describe_config() const override;
   boost::asio::awaitable<void> configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> startup() override;
   void request_stop() noexcept override;
   boost::asio::awaitable<void> shutdown() override;

 private:
   struct impl;
   class api_impl;
   class provider_source_impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::plugins::crypto::wallet
