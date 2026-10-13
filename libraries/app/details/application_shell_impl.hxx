#pragma once

namespace forge::app {

struct application_shell::impl {
   explicit impl(application_shell_options input);

   static std::unique_ptr<forge::asio::compute::pool>
   make_compute_pool(const std::optional<forge::asio::compute::pool::options>& options);
   boost::asio::awaitable<void> stop_execution();

   application_shell_options options;
   plugin_lifecycle lifecycle;
   forge::asio::runtime runtime;
   forge::asio::task::scheduler scheduler;
   std::unique_ptr<forge::asio::compute::pool> compute_pool;
   service_registry services;
   forge::api::core::registry apis;
   event_journal events;
   diagnostics_store diagnostics;
   application_context context;
   plugin_registry registry;
   std::unique_ptr<plugin_context> plugin_context_value;
   std::shared_ptr<application_runtime> plugin_runtime;
   forge::config::core::document effective_config;
   bool plugins_registered = false;
};

} // namespace forge::app
