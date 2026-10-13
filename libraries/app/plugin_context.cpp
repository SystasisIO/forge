module;

#include <utility>

module forge.app.plugin_context;

import forge.asio.exceptions;

namespace forge::app {
namespace {

forge::api::core::registry& default_api_registry() {
   static auto registry = forge::api::core::registry{};
   return registry;
}

} // namespace

plugin_context::plugin_context(forge::asio::task::scheduler& scheduler, forge::api::core::registry& apis,
                               service_view services, event_journal& events,
                               const diagnostics_store* diagnostics, application_environment environment,
                               forge::asio::compute::executor compute)
    : scheduler_{&scheduler}, compute_{std::move(compute)}, apis_{&apis}, services_{services},
      events_{&events}, diagnostics_{diagnostics}, environment_{std::move(environment)} {}

plugin_context::plugin_context(forge::asio::task::scheduler& scheduler, forge::api::core::registry& apis,
                               event_journal& events, const diagnostics_store* diagnostics,
                               application_environment environment, forge::asio::compute::executor compute)
    : plugin_context{scheduler, apis, service_view{}, events, diagnostics, std::move(environment),
                     std::move(compute)} {}

plugin_context::plugin_context(forge::asio::task::scheduler& scheduler, event_journal& events,
                               const diagnostics_store* diagnostics, application_environment environment,
                               forge::asio::compute::executor compute)
    : plugin_context{scheduler, default_api_registry(), events, diagnostics, std::move(environment),
                     std::move(compute)} {}

forge::asio::task::scheduler& plugin_context::scheduler() noexcept {
   return *scheduler_;
}

bool plugin_context::has_compute() const noexcept {
   return compute_.valid();
}

forge::asio::compute::executor plugin_context::compute() const {
   if (!has_compute()) {
      throw forge::asio::exceptions::invalid_state{"application compute pool is not configured"};
   }
   return compute_;
}

forge::api::core::view plugin_context::apis() const noexcept {
   return forge::api::core::view{*apis_};
}

service_view plugin_context::services() const noexcept {
   return services_;
}

event_journal& plugin_context::events() noexcept {
   return *events_;
}

const diagnostics_store* plugin_context::diagnostics() const noexcept {
   return diagnostics_;
}

const application_environment& plugin_context::environment() const noexcept {
   return environment_;
}

} // namespace forge::app
