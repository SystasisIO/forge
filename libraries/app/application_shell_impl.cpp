module;

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

module forge.app.application_shell;

import forge.app.application;
import forge.app.diagnostics;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.app.plugin_registry;
import forge.app.service_registry;
import forge.asio.runtime;
import forge.asio.compute;
import forge.asio.task;
import forge.api.core.registry;
import forge.config.core.document;

#include "details/application_shell_impl.hxx"

namespace forge::app {

application_shell::impl::impl(application_shell_options input)
    : options{std::move(input)}, runtime{options.runtime}, scheduler{runtime, options.scheduler},
      compute_pool{make_compute_pool(options.compute)},
      context{runtime, scheduler, apis, services.view(), events, diagnostics, options.environment,
              compute_pool == nullptr ? forge::asio::compute::executor{} : compute_pool->get_executor()} {
   diagnostics.bind_application(lifecycle.observer());
}

std::unique_ptr<forge::asio::compute::pool> application_shell::impl::make_compute_pool(
    const std::optional<forge::asio::compute::pool::options>& options) {
   if (!options) {
      return nullptr;
   }
   return std::make_unique<forge::asio::compute::pool>(*options);
}

boost::asio::awaitable<void> application_shell::impl::stop_execution() {
   // Plugin cleanup has succeeded. Cancel pending application tasks before
   // draining compute so delayed work cannot run during that drain.
   scheduler.request_stop();
   if (compute_pool != nullptr) {
      co_await compute_pool->shutdown();
   }
   services.clear();
   co_await scheduler.shutdown();
}

} // namespace forge::app
