module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <exception>

module forge.plugins.db.store.plugin;

import forge.api.core.binding;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.db.blob.store;
import forge.db.core.driver;
import forge.db.object.store;
import forge.db.revision.store;
import forge.exceptions;
import forge.plugins.db.store.exceptions;

#include "details/api_impl.hxx"
#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::db::store {

plugin::plugin() : impl_{std::make_shared<impl>(lifecycle_handle())} {}
plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.db.store"};
}

std::string plugin::version() const {
   return "2.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.db.store");
}

boost::asio::awaitable<void> plugin::on_configure(forge::config::core::component_view view) {
   impl_->configure(detail::decode_config(view));
   co_return;
}

boost::asio::awaitable<void> plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::on_initialize(forge::app::plugin_context& context) {
   static_cast<void>(context);
   impl_->initialize();
   co_return;
}

boost::asio::awaitable<void> plugin::on_after_initialize() {
   try {
      co_await impl_->open();
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const exceptions::duplicate_store&) {
      throw;
   } catch (const exceptions::stopped&) {
      throw;
   } catch (const exceptions::initialize_failed&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::initialize_failed, "db store plugin initialization failed",
                            forge::exceptions::ctx("error", error.what()));
   }
   co_return;
}

boost::asio::awaitable<void> plugin::on_startup() {
   try {
      impl_->start();
   } catch (const exceptions::stopped&) {
      throw;
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const exceptions::duplicate_store&) {
      throw;
   } catch (const exceptions::startup_failed&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::startup_failed, "db store plugin startup failed",
                            forge::exceptions::ctx("error", error.what()));
   }
   co_return;
}

void plugin::on_request_stop() noexcept {
   impl_->request_stop();
}

boost::asio::awaitable<void> plugin::on_shutdown() {
   co_await impl_->close();
}

forge::app::plugin_descriptor plugin::descriptor() {
   return forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "forge.plugins.db.store"},
      .factory = [] {
         return std::make_unique<plugin>();
      },
   };
}

} // namespace forge::plugins::db::store
