module;

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

module forge.app.application_builder;

import forge.asio.runtime;
import forge.asio.compute;
import forge.asio.task;
import forge.config.core.key_path;
import forge.config.core.value;
import forge.config.core.document;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.migration;
import forge.app.application_shell;
import forge.app.connect_context;
import forge.app.plugin_registry;

#include "details/application_builder_impl.hxx"
#include "details/built_application.hxx"

namespace forge::app {

application_builder::application_builder() : impl_{std::make_unique<impl>()} {}

application_builder::~application_builder() = default;

application_builder::application_builder(application_builder&&) noexcept = default;

application_builder& application_builder::operator=(application_builder&&) noexcept = default;

application_builder& application_builder::name(std::string value) {
   impl_->state.options.name = std::move(value);
   return *this;
}

application_builder& application_builder::environment(application_environment value) {
   impl_->state.options.environment = std::move(value);
   return *this;
}

application_builder& application_builder::runtime(forge::asio::runtime_options value) {
   impl_->state.options.runtime = std::move(value);
   return *this;
}

application_builder& application_builder::scheduler(forge::asio::task::scheduler::options value) {
   impl_->state.options.scheduler = std::move(value);
   return *this;
}

application_builder& application_builder::compute(forge::asio::compute::pool::options value) {
   impl_->state.options.compute = std::move(value);
   return *this;
}

application_builder& application_builder::plugin(plugin_descriptor descriptor) {
   impl_->state.plugins.push_back(std::move(descriptor));
   return *this;
}

application_builder& application_builder::describe_config(forge::config::core::component_descriptor descriptor) {
   impl_->state.config_descriptors.push_back(std::move(descriptor));
   return *this;
}

application_builder& application_builder::run_foreground(std::function<int(application_shell&)> handler) {
   impl_->state.foreground = std::move(handler);
   return *this;
}

std::unique_ptr<application_shell> application_builder::build() && {
   if (!impl_) {
      throw std::logic_error{"application builder has already been moved"};
   }
   auto state = std::move(impl_->state);
   impl_.reset();
   return std::make_unique<built_application>(std::move(state));
}

std::invalid_argument application_builder::make_decode_error(const forge::config::core::decode_diagnostics& diagnostics) {
   auto message = std::ostringstream{};
   message << "application config decode failed";
   for (const auto& entry : diagnostics.entries) {
      message << "; " << entry.path << ": " << entry.message;
   }
   return std::invalid_argument{message.str()};
}

void application_builder::add_configure_callback(configure_callback callback) {
   impl_->state.configure_callbacks.push_back(std::move(callback));
}

void application_builder::add_provide_callback(provide_callback callback) {
   impl_->state.provide_callbacks.push_back(std::move(callback));
}

void application_builder::add_connect_callback(connect_callback callback) {
   impl_->state.connect_callbacks.push_back(std::move(callback));
}

void application_builder::add_after_initialize_callback(after_initialize_callback callback) {
   impl_->state.after_initialize_callbacks.push_back(std::move(callback));
}

} // namespace forge::app
