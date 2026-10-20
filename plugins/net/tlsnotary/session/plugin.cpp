module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.plugins.net.tlsnotary.session.plugin;

import forge.api.core.registry;
import forge.app.plugin_lifecycle;
import forge.asio.runtime;
import forge.asio.compute;
import forge.config.core.decode;
import forge.net.tlsnotary.engine;
import forge.plugins.net.tlsnotary.session.api;
import forge.plugins.net.tlsnotary.session.config;
import forge.plugins.net.tlsnotary.session.exceptions;
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::tlsnotary::session {

plugin::plugin() : _impl{std::make_shared<impl>(lifecycle_handle())} {}
plugin::~plugin() = default;
forge::app::plugin_id plugin::id() const {
   return {.value = "forge.plugins.net.tlsnotary.session"};
}

std::string plugin::version() const {
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.tlsnotary.session");
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   auto decoded = forge::config::core::decode<config>(view.source(), view.section());
   if (!decoded.ok() || !std::filesystem::path{decoded.value.worker_path}.is_absolute())
      FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                            "TLSNotary requires a trusted absolute worker path and bounded limits");
   const auto lock = std::scoped_lock{_impl->mutex};
   _impl->settings = std::move(decoded.value);
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(_impl));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   if (!context.has_compute())
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "TLSNotary requires caller-owned compute workers");
   const auto generation = _impl->lifecycle.snapshot().generation;
   if (!_impl->lifecycle.try_admit(generation, [&] {
          const auto lock = std::scoped_lock{_impl->mutex};
          _impl->runtime = &context.scheduler().runtime_context();
          _impl->compute = context.compute();
       }))
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "TLSNotary initialization stopped");
   co_return;
}

boost::asio::awaitable<void> plugin::on_startup() {
   auto owner = std::shared_ptr<forge::net::tlsnotary::engine>{};
   const auto generation = _impl->lifecycle.snapshot().generation;
   {
      const auto lock = std::scoped_lock{_impl->mutex};
      if (!_impl->runtime)
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "TLSNotary runtime is unavailable");
      owner = std::make_shared<forge::net::tlsnotary::engine>(
          *_impl->runtime, _impl->compute,
          forge::net::tlsnotary::engine_options{.worker_path = _impl->settings.worker_path,
                                                .max_active = _impl->settings.max_active,
                                                .max_queued = _impl->settings.max_queued});
   }

   if (!_impl->lifecycle.try_admit(generation, [&] {
          const auto lock = std::scoped_lock{_impl->mutex};
          _impl->engine = owner;
       })) {
      co_await owner->shutdown();
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "TLSNotary startup stopped");
   }

   co_return;
}

void plugin::on_request_stop() noexcept {
   auto owner = std::shared_ptr<forge::net::tlsnotary::engine>{};
   {
      const auto lock = std::scoped_lock{_impl->mutex};
      owner = _impl->engine;
   }

   if (owner)
      owner->request_stop();
}

boost::asio::awaitable<void> plugin::on_shutdown() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   auto owner = std::shared_ptr<forge::net::tlsnotary::engine>{};
   {
      const auto lock = std::scoped_lock{_impl->mutex};
      owner = _impl->engine;
   }

   if (owner)
      co_await owner->shutdown();
   {
      const auto lock = std::scoped_lock{_impl->mutex};
      _impl->engine.reset();
      _impl->runtime = nullptr;
      _impl->compute = {};
   }
}

forge::app::plugin_descriptor plugin::descriptor() {
   return {.id = {.value = "forge.plugins.net.tlsnotary.session"},
           .factory = [] { return std::make_unique<plugin>(); }};
}
} // namespace forge::plugins::net::tlsnotary::session
