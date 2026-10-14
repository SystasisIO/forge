module;

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

export module forge.app.application_shell;

export import forge.app.connect_context;
export import forge.app.service_registry;

import forge.asio.runtime;
import forge.asio.compute;
import forge.asio.task;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.app.application_environment;
import forge.app.diagnostics;
import forge.app.events;
import forge.app.plugin_lifecycle;
import forge.app.plugin_registry;

export namespace forge::app {

struct application_shell_options {
   std::string name = "forge-app";
   application_environment environment{};
   forge::asio::runtime_options runtime{};
   forge::asio::task::scheduler::options scheduler{};
   std::optional<forge::asio::compute::pool::options> compute;
};

class application_context {
 public:
   application_context(forge::asio::runtime& runtime, forge::asio::task::scheduler& scheduler,
                       forge::api::core::registry& apis, service_view services, event_journal& events,
                       const diagnostics_store& diagnostics, application_environment environment = {},
                       forge::asio::compute::executor compute = {});
   application_context(forge::asio::runtime& runtime, forge::asio::task::scheduler& scheduler,
                       forge::api::core::registry& apis, event_journal& events,
                       const diagnostics_store& diagnostics, application_environment environment = {},
                       forge::asio::compute::executor compute = {});

   [[nodiscard]] forge::asio::runtime& runtime() noexcept;
   [[nodiscard]] forge::asio::task::scheduler& scheduler() noexcept;
   [[nodiscard]] bool has_compute() const noexcept;
   [[nodiscard]] forge::asio::compute::executor compute() const;
   [[nodiscard]] forge::api::core::installer apis() noexcept;
   [[nodiscard]] forge::api::core::view api_view() const noexcept;
   template <typename Service> [[nodiscard]] std::shared_ptr<Service> service() const {
      return services_.get<Service>();
   }
   [[nodiscard]] const application_environment& environment() const noexcept;
   [[nodiscard]] event_journal& events() noexcept;
   [[nodiscard]] const diagnostics_store& diagnostics() const noexcept;

 private:
   forge::asio::runtime* runtime_ = nullptr;
   forge::asio::task::scheduler* scheduler_ = nullptr;
   forge::asio::compute::executor compute_;
   forge::api::core::registry* apis_ = nullptr;
   service_view services_;
   const application_environment environment_;
   event_journal* events_ = nullptr;
   const diagnostics_store* diagnostics_ = nullptr;
};

class configure_context {
 public:
   explicit configure_context(const forge::config::core::document& document);

   [[nodiscard]] const forge::config::core::document& document() const noexcept;
   [[nodiscard]] forge::config::core::component_view view(std::string section) const;

 private:
   const forge::config::core::document* document_ = nullptr;
};

class application_shell {
 public:
   explicit application_shell(application_shell_options options = {});
   // Complete async shutdown before destruction; destruction only closes
   // admission and marks the observer's owner as gone.
   virtual ~application_shell();

   application_shell(const application_shell&) = delete;
   application_shell& operator=(const application_shell&) = delete;

   [[nodiscard]] forge::config::core::component_registry describe_config();
   void configure(const forge::config::core::document& document);
   boost::asio::awaitable<void> async_configure(forge::config::core::document document);
   boost::asio::awaitable<void> initialize();
   boost::asio::awaitable<void> startup();
   // The accepted owner joins cleanup despite cancellation; joiners may cancel
   // their wait. Caller cancellation state/throw policy and lazy entry are preserved.
   boost::asio::awaitable<void> shutdown();
   void request_stop() noexcept;

   [[nodiscard]] int run();
   [[nodiscard]] lifecycle_phase state() const noexcept;
   [[nodiscard]] lifecycle_observer lifecycle() const noexcept;
   [[nodiscard]] const application_environment& environment() const noexcept;
   [[nodiscard]] forge::asio::runtime& runtime() noexcept;
   [[nodiscard]] forge::asio::task::scheduler& scheduler() noexcept;
   [[nodiscard]] bool has_compute() const noexcept;
   [[nodiscard]] forge::asio::compute::executor compute() const;
   [[nodiscard]] forge::api::core::registry& apis() noexcept;
   [[nodiscard]] service_view services() const noexcept;
   [[nodiscard]] event_journal& events() noexcept;
   [[nodiscard]] const diagnostics_store& diagnostics() const noexcept;

 protected:
   virtual void on_describe_config(forge::config::core::component_registry& registry) const;
   virtual boost::asio::awaitable<void> on_configure(configure_context& context);
   virtual void on_register_plugins(plugin_registry& registry);
   virtual boost::asio::awaitable<void> on_provide(application_context& context);
   virtual boost::asio::awaitable<void> on_connect(connect_context& context);
   virtual boost::asio::awaitable<void> on_after_initialize(const application_context& context);
   virtual int on_run_foreground();

 private:
   void ensure_plugins_registered();
   void instantiate_plugins(const forge::config::core::document& document, std::uint64_t generation);
   [[nodiscard]] forge::config::core::component_registry collect_config();
   [[nodiscard]] forge::config::core::document make_effective_config(const forge::config::core::document& document);
   boost::asio::awaitable<void> apply_effective_config(forge::config::core::document document,
                                                     std::uint64_t generation);
   boost::asio::awaitable<void> rollback(std::exception_ptr failure);
   boost::asio::awaitable<void> shutdown_owned(std::stop_token stop = {});

   struct impl;
   std::unique_ptr<impl> impl_;
};

} // namespace forge::app
