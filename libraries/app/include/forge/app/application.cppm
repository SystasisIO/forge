module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <exception>
#include <memory>
#include <vector>

export module forge.app.application;

import forge.config.core.component;
import forge.config.core.document;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.app.diagnostics;
import forge.app.plugin_context;
import forge.app.plugin;
import forge.app.plugin_lifecycle;
import forge.app.application_shell;

export namespace forge::app {

class application_runtime {
 public:
   application_runtime(plugin_context& context, std::vector<std::unique_ptr<plugin>> plugins,
                       diagnostics_store* diagnostics = nullptr);
   ~application_runtime();

   application_runtime(const application_runtime&) = delete;
   application_runtime& operator=(const application_runtime&) = delete;

   [[nodiscard]] config::core::component_registry describe_config() const;
   boost::asio::awaitable<void> configure(const config::core::document& document);
   boost::asio::awaitable<void> provide(forge::api::core::provider& provider);
   boost::asio::awaitable<void> initialize();
   boost::asio::awaitable<void> startup();
   boost::asio::awaitable<void> shutdown();
   void request_stop() noexcept;

   [[nodiscard]] lifecycle_phase state() const noexcept;
   [[nodiscard]] lifecycle_observer lifecycle() const noexcept;
   [[nodiscard]] std::size_t plugin_count() const noexcept;

 private:
   friend class application_shell;

   application_runtime(plugin_context& context, std::vector<std::unique_ptr<plugin>> plugins,
                       plugin_lifecycle lifecycle, diagnostics_store* diagnostics);

   // The shell already owns the shared application operation while calling
   // these stages. Plugin entrypoints still guard each individual hook.
   boost::asio::awaitable<void> configure_plugins(const config::core::document& document);
   boost::asio::awaitable<void> provide_plugins(forge::api::core::provider& provider);
   boost::asio::awaitable<void> initialize_plugins();
   boost::asio::awaitable<void> startup_plugins();
   boost::asio::awaitable<void> shutdown_plugins();
   boost::asio::awaitable<void> rollback(std::exception_ptr failure);
   void stop_plugins() noexcept;

   plugin_context* context_ = nullptr;
   std::vector<std::unique_ptr<plugin>> plugins_;
   plugin_lifecycle lifecycle_;
   bool owns_lifecycle_ = false;
};

} // namespace forge::app
