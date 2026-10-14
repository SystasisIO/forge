#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

import forge.app.application_builder;
import forge.app.application_environment;
import forge.app.application_shell;
import forge.app.daemon;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.dispatcher;
import forge.config.core.component;
import forge.schema.object;

namespace service_example {

struct service_config {
   std::uint16_t batch_size = 16;
};

struct status_config {
   std::uint16_t batch_limit = 32;
};

struct service_settings {
   std::uint16_t batch_size = 16;
   std::filesystem::path data_path;
   std::string profile;
};

class status_api : public forge::api::core::contract<status_api, forge::api::core::surface::local> {
 public:
   virtual ~status_api() = default;
   [[nodiscard]] virtual std::uint16_t batch_limit() const noexcept = 0;
};

class status_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] static forge::app::plugin_descriptor descriptor();
   forge::app::plugin_id id() const override;
   std::string version() const override;
   std::optional<forge::config::core::component_descriptor> describe_config() const override;

 protected:
   boost::asio::awaitable<void> on_configure(forge::config::core::component_view view) override;
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   class api_impl;
   std::uint16_t batch_limit_ = 32;
};

class status_plugin::api_impl final : public status_api {
 public:
   explicit api_impl(std::uint16_t batch_limit);
   std::uint16_t batch_limit() const noexcept override;

 private:
   const std::uint16_t batch_limit_;
};

std::unique_ptr<forge::app::application_shell> make_application(const forge::app::daemon_context& context);
forge::app::daemon_options daemon_options();

BOOST_DESCRIBE_STRUCT(service_config, (), (batch_size))
BOOST_DESCRIBE_STRUCT(status_config, (), (batch_limit))

} // namespace service_example

template <> struct forge::schema::rules<service_example::service_config> {
   static forge::schema::object_schema<service_example::service_config> define() {
      auto schema = forge::schema::object<service_example::service_config>();
      schema.field<&service_example::service_config::batch_size>("batch-size").default_value(16).range(1, 64);
      return schema;
   }
};

template <> struct forge::schema::rules<service_example::status_config> {
   static forge::schema::object_schema<service_example::status_config> define() {
      auto schema = forge::schema::object<service_example::status_config>();
      schema.field<&service_example::status_config::batch_limit>("batch-limit").default_value(32).range(1, 64);
      return schema;
   }
};

FORGE_API(::service_example::status_api, FORGE_API_CONTRACT("service.status", 1, 0))
