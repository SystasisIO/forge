export module forge.app.plugin_context;

export import forge.app.application_environment;
import forge.app.diagnostics;
import forge.app.events;
export import forge.app.service_registry;
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

export namespace forge::app {

class plugin_context {
 public:
   plugin_context(forge::asio::task::scheduler& scheduler, forge::api::core::registry& apis,
                  service_view services, event_journal& events,
                  const diagnostics_store* diagnostics = nullptr, application_environment environment = {},
                  forge::asio::compute::executor compute = {});
   plugin_context(forge::asio::task::scheduler& scheduler, forge::api::core::registry& apis,
                  event_journal& events, const diagnostics_store* diagnostics = nullptr,
                  application_environment environment = {},
                  forge::asio::compute::executor compute = {});
   plugin_context(forge::asio::task::scheduler& scheduler, event_journal& events,
                  const diagnostics_store* diagnostics = nullptr, application_environment environment = {},
                  forge::asio::compute::executor compute = {});

   [[nodiscard]] forge::asio::task::scheduler& scheduler() noexcept;
   [[nodiscard]] bool has_compute() const noexcept;
   [[nodiscard]] forge::asio::compute::executor compute() const;
   [[nodiscard]] forge::api::core::view apis() const noexcept;
   [[nodiscard]] service_view services() const noexcept;
   [[nodiscard]] event_journal& events() noexcept;
   [[nodiscard]] const diagnostics_store* diagnostics() const noexcept;
   [[nodiscard]] const application_environment& environment() const noexcept;

 private:
   forge::asio::task::scheduler* scheduler_ = nullptr;
   forge::asio::compute::executor compute_;
   forge::api::core::registry* apis_ = nullptr;
   service_view services_;
   event_journal* events_ = nullptr;
   const diagnostics_store* diagnostics_ = nullptr;
   const application_environment environment_;
};

} // namespace forge::app
