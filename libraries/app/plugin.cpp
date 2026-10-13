module;

#include "details/lifecycle_log.hxx"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/system/system_error.hpp>

#include <exception>
#include <optional>
#include <utility>

module forge.app.plugin;

import forge.config.core.component;
import forge.app.exceptions;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;

namespace forge::app {

namespace {

void log_lifecycle(const plugin& owner, const char* operation, bool beginning = false,
                   std::exception_ptr error = {}, bool live = true) noexcept {
   try {
      const auto id = owner.id();
      const auto state = owner.lifecycle().snapshot();
      const auto name = std::string_view{operation};
      if (!beginning && !error && name == "startup" && state.phase != lifecycle_phase::started) {
         return;
      }
      detail::emit_lifecycle_log({.logger = "forge.app.plugin",
                                 .event = error ? "plugin.operation.failed"
                                     : beginning ? "plugin.operation.begin"
                                     : name == "request_stop" ? "plugin.stop.requested" : "plugin.operation.complete",
                                 .operation = operation,
                                 .level = !beginning && (name == "startup" || name == "shutdown" ||
                                                        (name == "request_stop" && live))
                                     ? detail::lifecycle_log_level::info : detail::lifecycle_log_level::debug,
                                 .phase = static_cast<int>(state.phase),
                                 .generation = state.generation,
                                 .epoch = state.epoch,
                                 .stop_requested = state.stop_requested,
                                 .error = error,
                                 .canceled_event = "plugin.operation.canceled",
                                 .id = id.value});
   } catch (...) {
      // Logging, virtual identity and sinks cannot veto terminal publication.
   }
}

} // namespace

plugin::plugin() = default;

plugin::~plugin() {
   lifecycle_.abandon_owner();
}

lifecycle_observer plugin::lifecycle() const noexcept {
   return lifecycle_.observer();
}

plugin_lifecycle plugin::lifecycle_handle() const noexcept {
   return lifecycle_;
}

std::optional<config::core::component_descriptor> plugin::describe_config() const {
   return std::nullopt;
}

boost::asio::awaitable<void> plugin::configure(config::core::component_view view) {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::configure);
   if (!operation) {
      co_return;
   }
   log_lifecycle(*this, "configure", true);
   try {
      co_await on_configure(std::move(view));
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "configure", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "configure", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::provide);
   if (!operation) {
      co_return;
   }
   log_lifecycle(*this, "provide", true);
   try {
      co_await on_provide(provider);
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "provide", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "provide", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> plugin::initialize(plugin_context& context) {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::initialize);
   if (!operation) {
      co_return;
   }
   log_lifecycle(*this, "initialize", true);
   try {
      co_await on_initialize(context);
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "initialize", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "initialize", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> plugin::after_initialize() {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::after_initialize);
   if (!operation) {
      co_return;
   }
   log_lifecycle(*this, "after_initialize", true);
   try {
      co_await on_after_initialize();
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "after_initialize", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "after_initialize", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> plugin::startup() {
   // Direct consumers need the same cross-plugin preparation barrier as an
   // application runtime. Ensure it before claiming startup to avoid self-join.
   co_await after_initialize();
   auto operation = co_await lifecycle_.begin(lifecycle_operation::startup);
   if (!operation) {
      co_return;
   }
   log_lifecycle(*this, "startup", true);
   try {
      co_await on_startup();
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "startup", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "startup", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

void plugin::request_stop() noexcept {
   const auto before = lifecycle_.snapshot();
   if (!lifecycle_.request_stop()) {
      return;
   }
   on_request_stop();
   lifecycle_.complete_stop();
   log_lifecycle(*this, "request_stop", false, {}, before.phase != lifecycle_phase::created &&
                 before.phase != lifecycle_phase::configured);
}

boost::asio::awaitable<void> plugin::shutdown() {
   request_stop();
   auto operation = co_await lifecycle_.begin(lifecycle_operation::shutdown);
   if (!operation) {
      co_return;
   }
   // The accepted cleanup is shared by all callers. Canceling any one caller
   // cannot abandon resources or cancel the owner's cleanup hook.
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
   log_lifecycle(*this, "shutdown", true);
   try {
      co_await on_shutdown();
   } catch (...) {
      const auto error = std::current_exception();
      operation.complete(error);
      log_lifecycle(*this, "shutdown", false, error);
      throw;
   }
   const auto error = operation.complete();
   log_lifecycle(*this, "shutdown", false, error);
   if (error) {
      std::rethrow_exception(error);
   }
}

boost::asio::awaitable<void> plugin::on_configure(config::core::component_view) {
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider&) {
   co_return;
}

boost::asio::awaitable<void> plugin::on_after_initialize() {
   co_return;
}

void plugin::on_request_stop() noexcept {}

bool valid_plugin_id(const plugin_id& id) noexcept {
   return !id.value.empty();
}

} // namespace forge::app
