module;

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <functional>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.db.store.plugin;

export import forge.plugins.db.store.api;

import forge.api.core.binding;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.config.core.component;
import forge.db.object.store;

export namespace forge::plugins::db::store {

// Trusted composition callback; the actual store is borrowed only until its
// awaitable completes. Do not retain store copies, transactions or tasks.
struct open_prerequisite {
   std::string store;
   std::vector<std::string> before;
   std::function<boost::asio::awaitable<void>(forge::db::object::store&)> check;
};

class plugin final : public forge::app::plugin {
 public:
   struct options {
      std::vector<open_prerequisite> prerequisites;
   };

   [[nodiscard]] static forge::app::plugin_descriptor descriptor(options value = {});

   explicit plugin(options value = {});
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
   boost::asio::awaitable<void> on_after_initialize() override;
   boost::asio::awaitable<void> on_startup() override;
   void on_request_stop() noexcept override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   struct impl;
   class api_impl;
   class store_handle_state_impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::plugins::db::store
