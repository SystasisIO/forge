module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/scope/scope_exit.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

module forge.plugins.log.otlp.plugin;

import forge.api.core.registry;
import forge.asio.notification;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.log.record;
import forge.log.logger;
import forge.crypto.core.types;
import forge.net.http.client;
import forge.net.http.types;
import forge.otlp.crash;
import forge.otlp.log_exporter;
import forge.otlp.log_sink;
import forge.crypto.core.secret_bytes;
import forge.plugins.crypto.secrets.api;
import forge.plugins.log.otlp.api;
import forge.plugins.log.otlp.exceptions;
import forge.plugins.log.otlp.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::log::otlp {

plugin::plugin() : impl_{std::make_shared<impl>(lifecycle_handle())} {}

plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.log.otlp"};
}

std::string plugin::version() const {
   return "1.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.log.otlp");
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   auto settings = decode_config(view);
   const auto generation = impl_->lifecycle.snapshot().generation;
   if (!impl_->lifecycle.try_admit(generation, [&] { std::swap(impl_->settings, settings); })) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "OTLP configuration was stopped");
   }
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   const auto generation = impl_->lifecycle.snapshot().generation;
   auto* runtime = &context.scheduler().runtime_context();
   auto resolve_headers = decltype(impl_->resolve_headers){};
   if (impl_->settings.export_enabled) {
      auto secrets = std::shared_ptr<forge::plugins::crypto::secrets::api>{};
      const auto needs_secrets =
          std::ranges::any_of(impl_->settings.headers, [](const header& value) { return value.secret_id.has_value(); });
      try {
         if (needs_secrets) {
            secrets = context.apis()
                          .get<forge::plugins::crypto::secrets::api>(
                              {.id = {"forge.plugins.crypto.secrets"}, .major = 1, .min_revision = 0})
                          .shared();
         }
         resolve_headers = [secrets = std::move(secrets)](const std::vector<header>& headers)
             -> boost::asio::awaitable<std::vector<std::pair<std::string, forge::crypto::core::secret_bytes>>> {
            auto resolved = std::vector<std::pair<std::string, forge::crypto::core::secret_bytes>>{};
            resolved.reserve(headers.size());
            for (const auto& header : headers) {
               if (!header.secret_id) {
                  auto bytes = forge::crypto::core::bytes{header.value.begin(), header.value.end()};
                  resolved.emplace_back(header.name, forge::crypto::core::secret_bytes{std::move(bytes)});
                  continue;
               }
               try {
                  auto material =
                      co_await secrets->get_bytes({.secret_id = *header.secret_id, .purpose = *header.purpose});
                  auto clear_material =
                      boost::scope::scope_exit{[&material] { forge::crypto::core::secure_erase(material.bytes); }};
                  const auto text =
                      std::string_view{reinterpret_cast<const char*>(material.bytes.data()), material.bytes.size()};
                  validate_header_value(header.name, text);
                  resolved.emplace_back(header.name, forge::crypto::core::secret_bytes{std::move(material.bytes)});
               } catch (...) {
                  FORGE_THROW_EXCEPTION(exceptions::startup_failed, "failed to resolve OTLP header secret",
                                        forge::exceptions::ctx("header", header.name),
                                        forge::exceptions::ctx("secret_id", *header.secret_id),
                                        forge::exceptions::ctx("purpose", *header.purpose));
               }
            }
            co_return resolved;
         };
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::startup_failed, "OTLP secret headers require the local Crypto Secrets API");
      }
   }
   if (!impl_->lifecycle.try_admit(generation, [&] {
      impl_->runtime = runtime;
      impl_->resolve_headers.swap(resolve_headers);
   })) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "OTLP initialization was stopped");
   }
   co_return;
}

boost::asio::awaitable<void> plugin::on_after_initialize() {
   const auto configure_route = [](const logger_route& route) {
      auto logger = forge::logger::get(route.name);
      logger.set_name(route.name);
      logger.set_enabled(route.enabled);
      logger.set_log_level(parse_log_level(route.level));
      forge::logger::update(route.name, logger);
   };
   for (const auto& route : impl_->settings.loggers) {
      configure_route(route);
   }
   if (!impl_->settings.export_enabled) {
      co_return;
   }
   if (impl_->runtime == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "OTLP logs plugin was not initialized with a runtime");
   }

   try {
      auto rollback = boost::scope::scope_exit{[this] {
         impl_->crash_guard = forge::otlp::crash_guard{};
         impl_->detach_sink();
         impl_->sink.reset();
         impl_->clear_exporter();
      }};
      auto options = make_exporter_options(impl_->settings);
      if (impl_->resolve_headers) {
         auto secure_headers = std::make_shared<std::vector<std::pair<std::string, forge::crypto::core::secret_bytes>>>(
             co_await impl_->resolve_headers(impl_->settings.headers));
         auto materialized = std::vector<forge::net::http::header_entry>{};
         materialized.reserve(secure_headers->size());
         auto clear_materialized = boost::scope::scope_exit{[&materialized] {
            for (auto& header : materialized) {
               forge::crypto::core::secure_erase(header.text);
            }
         }};
         for (const auto& [name, value] : *secure_headers) {
            const auto bytes = value.span();
            materialized.push_back(
                {.name = name, .text = std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()}});
         }
         auto existing =
             std::vector<forge::net::http::header_entry>{{.name = "Content-Type", .text = "application/json"}};
         if (!options.user_agent.empty()) {
            existing.push_back({.name = "User-Agent", .text = options.user_agent});
         }
         forge::net::http::validate_provider_headers(materialized, existing, options.http_client.max_provider_headers,
                                                     options.http_client.max_provider_header_bytes);
         options.http_client.header_provider = [secure_headers =
                                                    std::move(secure_headers)](const forge::net::http::request&) {
            auto result = std::vector<forge::net::http::header_entry>{};
            result.reserve(secure_headers->size());
            for (const auto& [name, value] : *secure_headers) {
               const auto bytes = value.span();
               result.push_back(
                   {.name = name, .text = std::string{reinterpret_cast<const char*>(bytes.data()), bytes.size()}});
            }
            return result;
         };
      }
      auto exporter = std::make_shared<forge::otlp::log_exporter>(*impl_->runtime, std::move(options));
      impl_->sink = std::make_shared<forge::otlp::log_sink>(exporter);
      const auto attach_export_sink = [this](const logger_route& route) {
         if (!route.export_logs) {
            return;
         }
         auto logger = forge::logger::get(route.name);
         impl_->attached_loggers.push_back(attached_logger{.name = route.name, .logger = logger});
         logger.add_sink(impl_->sink);
         forge::logger::update(route.name, logger);
      };
      for (const auto& route : impl_->settings.loggers) {
         attach_export_sink(route);
      }
      if (impl_->settings.crash_spool.enabled) {
         const auto crash_options = make_crash_spool_options(impl_->settings);
         impl_->crash_guard = forge::otlp::install_crash_capture(crash_options);
         if (impl_->settings.crash_spool.resend_on_startup) {
            co_await forge::otlp::async_resend_crashes(*exporter, crash_options);
         }
      }
      impl_->publish_exporter(std::move(exporter));
      rollback.set_active(false);
   } catch (const exceptions::startup_failed&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "failed to start OTLP logs exporter",
                            forge::exceptions::ctx("error", error.what()));
   }
}

boost::asio::awaitable<void> plugin::on_startup() {
   // Exporter setup belongs to the framework's after_initialize barrier.
   co_return;
}

void plugin::on_request_stop() noexcept {
   // Common lifecycle closes management admission. Logger sinks remain attached
   // until reverse shutdown reaches OTLP so dependent cleanup can still log.
}

boost::asio::awaitable<void> plugin::on_shutdown() {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto state = impl_;
   auto exporter = state->take_exporter();
   state->crash_guard = forge::otlp::crash_guard{};
   auto error = std::exception_ptr{};
   if (exporter) {
      try {
         co_await exporter->async_shutdown();
      } catch (...) {
         error = std::current_exception();
      }
   }
   state->detach_sink();
   state->sink.reset();
   state->runtime = nullptr;
   state->resolve_headers = {};
   co_await state->wait_for_calls();
   exporter.reset();
   if (error) {
      std::rethrow_exception(error);
   }
}

forge::app::plugin_descriptor plugin::descriptor() {
   return forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.log.otlp"},
       .factory = [] { return std::make_unique<plugin>(); },
   };
}

} // namespace forge::plugins::log::otlp
