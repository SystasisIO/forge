module;

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.app.application_builder;

import forge.config.core.component;
import forge.app.application_shell;
import forge.app.connect_context;
import forge.app.plugin_registry;

#include "details/built_application.hxx"

namespace forge::app {

built_application::built_application(builder_state state)
    : application_shell{std::move(state.options)}, state_{std::move(state)} {}

void built_application::on_describe_config(forge::config::core::component_registry& registry) const {
   for (auto descriptor : state_.config_descriptors) {
      registry.add(std::move(descriptor));
   }
}

boost::asio::awaitable<void> built_application::on_configure(configure_context& context) {
   for (auto& callback : state_.configure_callbacks) {
      co_await callback(context);
   }
}

void built_application::on_register_plugins(plugin_registry& registry) {
   for (auto descriptor : state_.plugins) {
      registry.register_plugin(std::move(descriptor));
   }
}

boost::asio::awaitable<void> built_application::on_provide(application_context& context) {
   for (auto& callback : state_.provide_callbacks) {
      co_await callback(context);
   }
}

boost::asio::awaitable<void> built_application::on_after_initialize(const application_context& context) {
   for (auto& callback : state_.after_initialize_callbacks) {
      co_await callback(context);
   }
}

boost::asio::awaitable<void> built_application::on_connect(connect_context& context) {
   for (auto& callback : state_.connect_callbacks) {
      co_await callback(context);
   }
}

int built_application::on_run_foreground() {
   if (state_.foreground) {
      return state_.foreground(*this);
   }
   return application_shell::on_run_foreground();
}

} // namespace forge::app
