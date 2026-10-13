module;

#include <boost/asio/awaitable.hpp>

#include <optional>
#include <string>
#include <compare>

export module forge.app.plugin;

export import forge.app.plugin_lifecycle;

import forge.config.core.component;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.app.plugin_context;

export namespace forge::app {

struct plugin_id {
   std::string value;

   [[nodiscard]] friend bool operator==(const plugin_id&, const plugin_id&) = default;
   [[nodiscard]] friend auto operator<=>(const plugin_id&, const plugin_id&) = default;
};

struct plugin_config {
   plugin_id id;
   bool enabled = true;
};

class plugin {
 public:
   plugin();
   // Complete async shutdown before destruction; the destructor only closes
   // admission and wakes observers, and cannot dispatch derived cleanup hooks.
   virtual ~plugin();

   plugin(const plugin&) = delete;
   plugin& operator=(const plugin&) = delete;

   [[nodiscard]] virtual plugin_id id() const = 0;
   [[nodiscard]] virtual std::string version() const = 0;

   [[nodiscard]] virtual std::optional<config::core::component_descriptor> describe_config() const;
   [[nodiscard]] lifecycle_observer lifecycle() const noexcept;

   virtual boost::asio::awaitable<void> configure(config::core::component_view view) final;
   virtual boost::asio::awaitable<void> provide(forge::api::core::provider& provider) final;
   virtual boost::asio::awaitable<void> initialize(plugin_context& context) final;
   virtual boost::asio::awaitable<void> after_initialize() final;
   virtual boost::asio::awaitable<void> startup() final;
   virtual void request_stop() noexcept final;
   virtual boost::asio::awaitable<void> shutdown() final;

 protected:
   [[nodiscard]] plugin_lifecycle lifecycle_handle() const noexcept;

   virtual boost::asio::awaitable<void> on_configure(config::core::component_view view);
   virtual boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider);
   virtual boost::asio::awaitable<void> on_initialize(plugin_context& context) = 0;
   virtual boost::asio::awaitable<void> on_after_initialize();
   virtual boost::asio::awaitable<void> on_startup() = 0;
   virtual void on_request_stop() noexcept;
   // Cleanup also covers setup/partially initialized resources. It must
   // tolerate never-initialized owners; a failed attempt may be retried.
   virtual boost::asio::awaitable<void> on_shutdown() = 0;

 private:
   plugin_lifecycle lifecycle_;
};

[[nodiscard]] bool valid_plugin_id(const plugin_id& id) noexcept;

} // namespace forge::app
