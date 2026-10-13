module;

#include "details/lifecycle_log.hxx"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.app.application;

import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin_lifecycle;

namespace forge::app {
namespace {

void publish_lifecycle_event(plugin_context& context, event_severity severity, const plugin& owner,
                             const char* transition, std::exception_ptr failure = {}) noexcept {
   try {
      const auto id = owner.id();
      context.events().publish(severity, "app.plugin." + id.value + "." + transition,
                               failure ? "lifecycle operation failed" : transition);
   } catch (...) {
      // History is best effort; lifecycle and cleanup never depend on it.
   }
}

} // namespace

application_runtime::application_runtime(plugin_context& context, std::vector<std::unique_ptr<plugin>> plugins,
                                         diagnostics_store* diagnostics)
    : application_runtime{context, std::move(plugins), plugin_lifecycle{}, diagnostics} {
   owns_lifecycle_ = true;
   if (diagnostics) {
      diagnostics->bind_application(lifecycle());
   }
}

application_runtime::application_runtime(plugin_context& context, std::vector<std::unique_ptr<plugin>> plugins,
                                         plugin_lifecycle lifecycle, diagnostics_store* diagnostics)
    : context_{&context}, plugins_{std::move(plugins)}, lifecycle_{std::move(lifecycle)} {
   if (diagnostics) {
      for (const auto& value : plugins_) {
         diagnostics->bind_plugin(value->id().value, value->version(), value->lifecycle());
      }
   }
}

application_runtime::~application_runtime() {
   request_stop();
   if (owns_lifecycle_) {
      lifecycle_.abandon_owner();
   }
}

config::core::component_registry application_runtime::describe_config() const {
   auto registry = config::core::component_registry{};
   for (const auto& value : plugins_) {
      if (auto descriptor = value->describe_config()) {
         registry.add(std::move(*descriptor));
      }
   }
   return registry;
}

boost::asio::awaitable<void> application_runtime::configure_plugins(const config::core::document& document) {
   for (auto& value : plugins_) {
      auto section = value->id().value;
      if (auto descriptor = value->describe_config()) {
         section = descriptor->section;
      }
      co_await value->configure(config::core::component_view{document, std::move(section)});
   }
}

boost::asio::awaitable<void> application_runtime::configure(const config::core::document& document) {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::configure);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(lifecycle_.snapshot(), "configure", true);
   auto failure = std::exception_ptr{};
   try {
      co_await configure_plugins(document);
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(lifecycle_.snapshot(), "configure", false, failure);
   if (failure) {
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await rollback(failure);
   }
}

boost::asio::awaitable<void> application_runtime::provide_plugins(forge::api::core::provider& provider) {
   for (auto& value : plugins_) {
      try {
         publish_lifecycle_event(*context_, event_severity::info, *value, "providing");
         co_await value->provide(provider);
         publish_lifecycle_event(*context_, event_severity::info, *value, "provided");
      } catch (...) {
         publish_lifecycle_event(*context_, event_severity::error, *value, "failed", std::current_exception());
         throw;
      }
   }
}

boost::asio::awaitable<void> application_runtime::provide(forge::api::core::provider& provider) {
   auto operation = co_await lifecycle_.begin(lifecycle_operation::provide);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(lifecycle_.snapshot(), "provide", true);
   auto failure = std::exception_ptr{};
   try {
      co_await provide_plugins(provider);
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(lifecycle_.snapshot(), "provide", false, failure);
   if (failure) {
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await rollback(failure);
   }
}

boost::asio::awaitable<void> application_runtime::initialize_plugins() {
   for (auto& value : plugins_) {
      try {
         publish_lifecycle_event(*context_, event_severity::info, *value, "initializing");
         co_await value->initialize(*context_);
      } catch (...) {
         publish_lifecycle_event(*context_, event_severity::error, *value, "failed", std::current_exception());
         throw;
      }
   }
   for (auto& value : plugins_) {
      try {
         co_await value->after_initialize();
         publish_lifecycle_event(*context_, event_severity::info, *value, "initialized");
      } catch (...) {
         publish_lifecycle_event(*context_, event_severity::error, *value, "failed", std::current_exception());
         throw;
      }
   }
}

boost::asio::awaitable<void> application_runtime::initialize() {
   auto operation = co_await lifecycle_.begin_joined(lifecycle_operation::initialize);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(lifecycle_.snapshot(), "initialize", true);
   auto failure = std::exception_ptr{};
   try {
      co_await initialize_plugins();
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(lifecycle_.snapshot(), "initialize", false, failure);
   if (failure) {
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await rollback(failure);
   }
}

boost::asio::awaitable<void> application_runtime::startup_plugins() {
   for (auto& value : plugins_) {
      try {
         publish_lifecycle_event(*context_, event_severity::info, *value, "starting");
         co_await value->startup();
         publish_lifecycle_event(*context_, event_severity::info, *value, "started");
      } catch (...) {
         publish_lifecycle_event(*context_, event_severity::error, *value, "failed", std::current_exception());
         throw;
      }
   }
}

boost::asio::awaitable<void> application_runtime::startup() {
   if (!lifecycle_.snapshot().completed(lifecycle_operation::initialize)) {
      co_await initialize();
   }
   auto operation = co_await lifecycle_.begin(lifecycle_operation::startup);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(lifecycle_.snapshot(), "startup", true);
   auto failure = std::exception_ptr{};
   try {
      co_await startup_plugins();
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(lifecycle_.snapshot(), "startup", false, failure);
   if (failure) {
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await rollback(failure);
   }
}

boost::asio::awaitable<void> application_runtime::rollback(std::exception_ptr failure) {
   // The caller disables cancellation before entering this child coroutine.
   try {
      co_await shutdown();
   } catch (...) {
      std::throw_with_nested(exceptions::shutdown_failed{"application rollback failed"});
   }
   std::rethrow_exception(failure);
}

boost::asio::awaitable<void> application_runtime::shutdown_plugins() {
   for (auto index = plugins_.size(); index > 0; --index) {
      auto& value = plugins_[index - 1];
      if (value->lifecycle().snapshot().phase == lifecycle_phase::stopped) {
         continue;
      }
      try {
         publish_lifecycle_event(*context_, event_severity::info, *value, "stopping");
         co_await value->shutdown();
         publish_lifecycle_event(*context_, event_severity::info, *value, "stopped");
      } catch (...) {
         publish_lifecycle_event(*context_, event_severity::error, *value, "failed", std::current_exception());
         // A failed dependent still owns its dependencies. Retain the reverse
         // dependency barrier and retry it before tearing those dependencies down.
         throw;
      }
   }
}

boost::asio::awaitable<void> application_runtime::shutdown() {
   request_stop();
   auto operation = co_await lifecycle_.begin(lifecycle_operation::shutdown);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(lifecycle_.snapshot(), "shutdown", true);
   auto failure = std::exception_ptr{};
   try {
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      co_await shutdown_plugins();
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(lifecycle_.snapshot(), "shutdown", false, failure);
   if (failure) {
      std::rethrow_exception(failure);
   }
}

void application_runtime::stop_plugins() noexcept {
   for (auto& value : plugins_) {
      value->request_stop();
   }
}

void application_runtime::request_stop() noexcept {
   const auto before = lifecycle_.snapshot();
   if (lifecycle_.request_stop()) {
      stop_plugins();
      lifecycle_.complete_stop();
      detail::log_application_stop(lifecycle_.snapshot(), before.phase != lifecycle_phase::created &&
                                  before.phase != lifecycle_phase::configured);
   }
}

lifecycle_phase application_runtime::state() const noexcept {
   return lifecycle_.snapshot().phase;
}

lifecycle_observer application_runtime::lifecycle() const noexcept {
   return lifecycle_.observer();
}

std::size_t application_runtime::plugin_count() const noexcept {
   return plugins_.size();
}

} // namespace forge::app
