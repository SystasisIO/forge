module;

#include "details/lifecycle_log.hxx"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

module forge.app.application_shell;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.compute;
import forge.asio.task;
import forge.api.core.registry;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.app.application;
import forge.app.application_environment;
import forge.app.diagnostics;
import forge.app.events;
import forge.app.exceptions;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.app.plugin_registry;
import forge.app.connect_context;
import forge.app.service_registry;
import forge.schema.value_kind;

#include "details/application_shell_impl.hxx"

namespace forge::app {
namespace {

void publish_application_event(event_journal& events, event_severity severity, const std::string& name,
                               const char* transition) noexcept {
   try {
      events.publish(severity, "app." + name + "." + transition, transition);
   } catch (...) {
      // History cannot interrupt lifecycle, stop admission or cleanup.
   }
}

[[nodiscard]] std::vector<plugin_config> enabled_config_for_all_plugins(const plugin_registry& registry) {
   auto out = std::vector<plugin_config>{};
   for (const auto& descriptor : registry.descriptors()) {
      out.push_back(plugin_config{
         .id = descriptor.id,
         .enabled = true,
      });
   }
   return out;
}

[[nodiscard]] std::string plugin_selection_key(const plugin_id& id) {
   constexpr auto official_prefix = std::string_view{"forge.plugins."};
   if (id.value.starts_with(official_prefix)) {
      return id.value.substr(official_prefix.size());
   }
   return id.value;
}

[[nodiscard]] forge::config::core::component_descriptor plugin_selection_descriptor(const plugin_registry& registry) {
   auto descriptor = forge::config::core::component_descriptor{.section = "plugins"};
   for (const auto& plugin : registry.descriptors()) {
      descriptor.fields.push_back(forge::config::core::field_descriptor{
         .name = plugin_selection_key(plugin.id) + ".enabled",
         .kind = forge::schema::value_kind::boolean,
         .has_default = true,
         .default_value = plugin.enabled_by_default,
         .description = "Enable plugin " + plugin.id.value,
      });
   }
   return descriptor;
}

[[nodiscard]] std::vector<plugin_config> plugin_selection_from_document(const plugin_registry& registry,
                                                                        const forge::config::core::document& document) {
   auto out = std::vector<plugin_config>{};
   for (const auto& descriptor : registry.descriptors()) {
      auto enabled = descriptor.enabled_by_default;
      const auto path = "plugins." + plugin_selection_key(descriptor.id) + ".enabled";
      if (const auto* configured = document.try_get(path)) {
         if (const auto* value = std::get_if<bool>(&configured->storage)) {
            enabled = *value;
         } else if (const auto* text = std::get_if<std::string>(&configured->storage)) {
            auto parsed = false;
            if (!forge::config::core::parse_bool_text(*text, parsed)) {
               FORGE_THROW_EXCEPTION(exceptions::config_failed, path + ": plugin enabled flag must be boolean",
                                     forge::exceptions::ctx("path", path));
            }
            enabled = parsed;
         } else {
            FORGE_THROW_EXCEPTION(exceptions::config_failed, path + ": plugin enabled flag must be boolean",
                                  forge::exceptions::ctx("path", path));
         }
      }
      out.push_back(plugin_config{
         .id = descriptor.id,
         .enabled = enabled,
      });
   }
   return out;
}

} // namespace

application_context::application_context(forge::asio::runtime& runtime, forge::asio::task::scheduler& scheduler,
                                         forge::api::core::registry& apis, service_view services,
                                         event_journal& events, const diagnostics_store& diagnostics,
                                         application_environment environment, forge::asio::compute::executor compute)
    : runtime_{&runtime}, scheduler_{&scheduler}, compute_{std::move(compute)}, apis_{&apis}, services_{services},
      environment_{std::move(environment)}, events_{&events}, diagnostics_{&diagnostics} {}

application_context::application_context(forge::asio::runtime& runtime, forge::asio::task::scheduler& scheduler,
                                         forge::api::core::registry& apis, event_journal& events,
                                         const diagnostics_store& diagnostics, application_environment environment,
                                         forge::asio::compute::executor compute)
    : application_context{runtime, scheduler, apis, service_view{}, events, diagnostics,
                          std::move(environment), std::move(compute)} {}

forge::asio::runtime& application_context::runtime() noexcept {
   return *runtime_;
}

forge::asio::task::scheduler& application_context::scheduler() noexcept {
   return *scheduler_;
}

bool application_context::has_compute() const noexcept {
   return compute_.valid();
}

forge::asio::compute::executor application_context::compute() const {
   if (!has_compute()) {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::invalid_state, "application compute pool is not configured");
   }
   return compute_;
}

forge::api::core::installer application_context::apis() noexcept {
   return forge::api::core::installer{*apis_};
}

forge::api::core::view application_context::api_view() const noexcept {
   return forge::api::core::view{*apis_};
}

const application_environment& application_context::environment() const noexcept {
   return environment_;
}

event_journal& application_context::events() noexcept {
   return *events_;
}

const diagnostics_store& application_context::diagnostics() const noexcept {
   return *diagnostics_;
}

configure_context::configure_context(const forge::config::core::document& document) : document_{&document} {}

const forge::config::core::document& configure_context::document() const noexcept {
   return *document_;
}

forge::config::core::component_view configure_context::view(std::string section) const {
   return forge::config::core::component_view{*document_, std::move(section)};
}

application_shell::application_shell(application_shell_options options)
    : impl_{std::make_unique<impl>(std::move(options))} {}

application_shell::~application_shell() {
   request_stop();
   impl_->lifecycle.abandon_owner();
}

void application_shell::on_describe_config(forge::config::core::component_registry&) const {}

boost::asio::awaitable<void> application_shell::on_configure(configure_context&) {
   co_return;
}

void application_shell::on_register_plugins(plugin_registry&) {}

boost::asio::awaitable<void> application_shell::on_provide(application_context&) {
   co_return;
}

boost::asio::awaitable<void> application_shell::on_connect(connect_context&) {
   co_return;
}

boost::asio::awaitable<void> application_shell::on_after_initialize(const application_context&) {
   co_return;
}

int application_shell::on_run_foreground() {
   return 0;
}

void application_shell::ensure_plugins_registered() {
   if (impl_->plugins_registered) {
      return;
   }
   on_register_plugins(impl_->registry);
   impl_->plugins_registered = true;
}

void application_shell::instantiate_plugins(const forge::config::core::document& document,
                                             std::uint64_t generation) {
   ensure_plugins_registered();
   impl_->plugin_context_value = std::make_unique<plugin_context>(
      impl_->scheduler, impl_->apis, impl_->services.view(), impl_->events, &impl_->diagnostics,
      impl_->options.environment,
      impl_->compute_pool == nullptr ? forge::asio::compute::executor{} : impl_->compute_pool->get_executor());
   impl_->diagnostics.clear_plugins();
   auto runtime = std::shared_ptr<application_runtime>{new application_runtime{
      *impl_->plugin_context_value,
      impl_->registry.instantiate_enabled(plugin_selection_from_document(impl_->registry, document)),
      impl_->lifecycle, &impl_->diagnostics}};
   if (!impl_->lifecycle.try_admit(generation, [&] {
      std::atomic_store_explicit(&impl_->plugin_runtime, runtime, std::memory_order_release);
   })) {
      runtime->stop_plugins();
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "application stopped before plugin admission");
   }
}

forge::config::core::component_registry application_shell::collect_config() {
   ensure_plugins_registered();
   auto registry = forge::config::core::component_registry{};
   on_describe_config(registry);
   if (!impl_->registry.descriptors().empty()) {
      registry.add(plugin_selection_descriptor(impl_->registry));
   }
   // Schema discovery is not a live runtime and must not replace its observers.
   auto plugins = impl_->registry.instantiate_enabled(enabled_config_for_all_plugins(impl_->registry));
   for (const auto& value : plugins) {
      if (auto descriptor = value->describe_config()) {
         registry.add(std::move(*descriptor));
      }
   }
   return registry;
}

forge::config::core::document application_shell::make_effective_config(const forge::config::core::document& document) {
   auto registry = collect_config();
   auto effective = forge::config::core::merge({forge::config::core::defaults_for(registry), document});
   const auto diagnostics = forge::config::core::validate_ingestion(effective, registry);
   if (!diagnostics.empty()) {
      const auto& error = diagnostics.front();
      FORGE_THROW_EXCEPTION(exceptions::config_failed, error.path + ": " + error.message,
                            forge::exceptions::ctx("path", error.path));
   }
   return effective;
}

boost::asio::awaitable<void> application_shell::apply_effective_config(forge::config::core::document document,
                                                                      std::uint64_t generation) {
   impl_->effective_config = std::move(document);
   auto context = configure_context{impl_->effective_config};
   co_await on_configure(context);
   instantiate_plugins(impl_->effective_config, generation);
   co_await std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)
       ->configure_plugins(impl_->effective_config);
}

forge::config::core::component_registry application_shell::describe_config() {
   return collect_config();
}

void application_shell::configure(const forge::config::core::document& document) {
   forge::asio::blocking::run(impl_->runtime, async_configure(document));
}

boost::asio::awaitable<void> application_shell::async_configure(forge::config::core::document document) {
   auto operation = co_await impl_->lifecycle.begin(lifecycle_operation::configure);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(impl_->lifecycle.snapshot(), "configure", true);
   auto failure = std::exception_ptr{};
   try {
      co_await apply_effective_config(make_effective_config(document), operation.generation());
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(impl_->lifecycle.snapshot(), "configure", false, failure);
   if (failure) {
      publish_application_event(impl_->events, event_severity::error, impl_->options.name, "configure_failed");
      const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      try {
         co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
      } catch (...) {
         failure = std::current_exception();
      }
      co_await boost::asio::this_coro::throw_if_cancelled(throws);
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> application_shell::initialize() {
   if (!impl_->lifecycle.snapshot().completed(lifecycle_operation::configure)) {
      auto operation = co_await impl_->lifecycle.begin_joined(lifecycle_operation::configure);
      if (operation) {
         detail::log_application_operation(impl_->lifecycle.snapshot(), "configure", true);
         auto failure = std::exception_ptr{};
         try {
            co_await apply_effective_config(make_effective_config(forge::config::core::document{}),
                                            operation.generation());
         } catch (...) {
            failure = std::current_exception();
         }
         failure = operation.complete(failure);
         detail::log_application_operation(impl_->lifecycle.snapshot(), "configure", false, failure);
         if (failure) {
            publish_application_event(impl_->events, event_severity::error, impl_->options.name, "configure_failed");
            const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
            co_await boost::asio::this_coro::throw_if_cancelled(false);
            try {
               co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
                   boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
            } catch (...) {
               failure = std::current_exception();
            }
            co_await boost::asio::this_coro::throw_if_cancelled(throws);
            std::rethrow_exception(failure);
         }
      }
   }
   if (!impl_->lifecycle.snapshot().completed(lifecycle_operation::provide)) {
      auto operation = co_await impl_->lifecycle.begin_joined(lifecycle_operation::provide);
      if (operation) {
         detail::log_application_operation(impl_->lifecycle.snapshot(), "provide", true);
         auto failure = std::exception_ptr{};
         try {
            co_await on_provide(impl_->context);
            auto provider = impl_->context.apis();
            co_await std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)
                ->provide_plugins(provider);
         } catch (...) {
            failure = std::current_exception();
         }
         failure = operation.complete(failure);
         detail::log_application_operation(impl_->lifecycle.snapshot(), "provide", false, failure);
         if (failure) {
            publish_application_event(impl_->events, event_severity::error, impl_->options.name, "provide_failed");
            const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
            co_await boost::asio::this_coro::throw_if_cancelled(false);
            try {
               co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
                   boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
            } catch (...) {
               failure = std::current_exception();
            }
            co_await boost::asio::this_coro::throw_if_cancelled(throws);
            std::rethrow_exception(failure);
         }
      }
   }
   if (!impl_->lifecycle.snapshot().completed(lifecycle_operation::initialize)) {
      auto operation = co_await impl_->lifecycle.begin_joined(lifecycle_operation::initialize);
      if (operation) {
         detail::log_application_operation(impl_->lifecycle.snapshot(), "initialize", true);
         auto failure = std::exception_ptr{};
         try {
            publish_application_event(impl_->events, event_severity::info, impl_->options.name, "initializing");
            co_await std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)
                ->initialize_plugins();
            auto connect = connect_context{impl_->context.api_view(), impl_->services};
            co_await on_connect(connect);
            impl_->services.close();
         } catch (...) {
            failure = std::current_exception();
         }
         failure = operation.complete(failure);
         detail::log_application_operation(impl_->lifecycle.snapshot(), "initialize", false, failure);
         if (failure) {
            publish_application_event(impl_->events, event_severity::error, impl_->options.name, "initialize_failed");
            const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
            co_await boost::asio::this_coro::throw_if_cancelled(false);
            try {
               co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
                   boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
            } catch (...) {
               failure = std::current_exception();
            }
            co_await boost::asio::this_coro::throw_if_cancelled(throws);
            std::rethrow_exception(failure);
         }
      }
   }
   auto operation = co_await impl_->lifecycle.begin(lifecycle_operation::after_initialize);
   if (operation) {
      detail::log_application_operation(impl_->lifecycle.snapshot(), "after_initialize", true);
      auto failure = std::exception_ptr{};
      try {
         co_await on_after_initialize(impl_->context);
      } catch (...) {
         failure = std::current_exception();
      }
      failure = operation.complete(failure);
      detail::log_application_operation(impl_->lifecycle.snapshot(), "after_initialize", false, failure);
      if (failure) {
         publish_application_event(impl_->events, event_severity::error, impl_->options.name, "after_initialize_failed");
         const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
         co_await boost::asio::this_coro::throw_if_cancelled(false);
         try {
            co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
                boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
         } catch (...) {
            failure = std::current_exception();
         }
         co_await boost::asio::this_coro::throw_if_cancelled(throws);
         std::rethrow_exception(failure);
      }
      publish_application_event(impl_->events, event_severity::info, impl_->options.name, "initialized");
   }
}

boost::asio::awaitable<void> application_shell::startup() {
   if (!impl_->lifecycle.snapshot().completed(lifecycle_operation::after_initialize)) {
      co_await initialize();
   }
   auto operation = co_await impl_->lifecycle.begin(lifecycle_operation::startup);
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(impl_->lifecycle.snapshot(), "startup", true);
   auto failure = std::exception_ptr{};
   try {
      publish_application_event(impl_->events, event_severity::info, impl_->options.name, "starting");
      co_await std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)->startup_plugins();
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(impl_->lifecycle.snapshot(), "startup", false, failure);
   if (failure) {
      publish_application_event(impl_->events, event_severity::error, impl_->options.name, "startup_failed");
      const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
      co_await boost::asio::this_coro::throw_if_cancelled(false);
      try {
         co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, rollback(failure),
             boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
      } catch (...) {
         failure = std::current_exception();
      }
      co_await boost::asio::this_coro::throw_if_cancelled(throws);
      std::rethrow_exception(failure);
   }
   if (state() == lifecycle_phase::started) {
      publish_application_event(impl_->events, event_severity::info, impl_->options.name, "started");
   }
}

boost::asio::awaitable<void> application_shell::rollback(std::exception_ptr failure) {
   // Run on an independently spawned, uncancelled coroutine thread.
   try {
      co_await shutdown_owned();
   } catch (...) {
      std::throw_with_nested(exceptions::shutdown_failed{"application rollback failed"});
   }
   std::rethrow_exception(failure);
}

boost::asio::awaitable<void> application_shell::shutdown() {
   const auto throws = co_await boost::asio::this_coro::throw_if_cancelled();
   co_await boost::asio::this_coro::throw_if_cancelled(false);
   auto failure = std::exception_ptr{};
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   try {
      auto stop = std::stop_source{};
      if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
         static_cast<void>(stop.request_stop());
      }
      if (cancellation.slot().is_connected()) {
         cancellation.slot().assign([stop](boost::asio::cancellation_type_t type) mutable {
            if (type != boost::asio::cancellation_type::none) {
               static_cast<void>(stop.request_stop());
            }
         });
      }
      co_await boost::asio::co_spawn(co_await boost::asio::this_coro::executor, shutdown_owned(stop.get_token()),
          boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   } catch (...) {
      failure = std::current_exception();
   }
   cancellation.slot().clear();
   co_await boost::asio::this_coro::throw_if_cancelled(throws);
   if (failure) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> application_shell::shutdown_owned(std::stop_token stop) {
   request_stop();
   auto operation = co_await impl_->lifecycle.begin(lifecycle_operation::shutdown, std::move(stop));
   if (!operation) {
      co_return;
   }
   detail::log_application_operation(impl_->lifecycle.snapshot(), "shutdown", true);
   auto failure = std::exception_ptr{};
   try {
      publish_application_event(impl_->events, event_severity::info, impl_->options.name, "stopping");
      if (auto runtime = std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)) {
         co_await runtime->shutdown_plugins();
      }
      co_await impl_->stop_execution();
   } catch (...) {
      failure = std::current_exception();
   }
   failure = operation.complete(failure);
   detail::log_application_operation(impl_->lifecycle.snapshot(), "shutdown", false, failure);
   if (failure) {
      publish_application_event(impl_->events, event_severity::error, impl_->options.name, "shutdown_failed");
      std::rethrow_exception(failure);
   }
   publish_application_event(impl_->events, event_severity::info, impl_->options.name, "stopped");
}

void application_shell::request_stop() noexcept {
   const auto before = impl_->lifecycle.snapshot();
   if (impl_->lifecycle.request_stop()) {
      impl_->services.close();
      if (auto runtime = std::atomic_load_explicit(&impl_->plugin_runtime, std::memory_order_acquire)) {
         runtime->stop_plugins();
      }
      impl_->lifecycle.complete_stop();
      detail::log_application_stop(impl_->lifecycle.snapshot(), before.phase != lifecycle_phase::created &&
                                  before.phase != lifecycle_phase::configured);
   }
}

int application_shell::run() {
   return on_run_foreground();
}

lifecycle_phase application_shell::state() const noexcept {
   return impl_->lifecycle.snapshot().phase;
}

lifecycle_observer application_shell::lifecycle() const noexcept {
   return impl_->lifecycle.observer();
}

const application_environment& application_shell::environment() const noexcept {
   return impl_->options.environment;
}

forge::asio::runtime& application_shell::runtime() noexcept {
   return impl_->runtime;
}

forge::asio::task::scheduler& application_shell::scheduler() noexcept {
   return impl_->scheduler;
}

bool application_shell::has_compute() const noexcept {
   return impl_->compute_pool != nullptr;
}

forge::asio::compute::executor application_shell::compute() const {
   if (!has_compute()) {
      FORGE_THROW_EXCEPTION(forge::asio::exceptions::invalid_state, "application compute pool is not configured");
   }
   return impl_->compute_pool->get_executor();
}

forge::api::core::registry& application_shell::apis() noexcept {
   return impl_->apis;
}

service_view application_shell::services() const noexcept {
   return impl_->services.view();
}

event_journal& application_shell::events() noexcept {
   return impl_->events;
}

const diagnostics_store& application_shell::diagnostics() const noexcept {
   return impl_->diagnostics;
}

} // namespace forge::app
