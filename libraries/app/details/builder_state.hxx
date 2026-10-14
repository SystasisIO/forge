#pragma once

namespace forge::app {

struct builder_state {
   application_shell_options options;
   std::vector<plugin_descriptor> plugins;
   std::vector<forge::config::core::component_descriptor> config_descriptors;
   std::vector<std::function<boost::asio::awaitable<void>(configure_context&)>> configure_callbacks;
   std::vector<std::function<boost::asio::awaitable<void>(application_context&)>> provide_callbacks;
   std::vector<std::function<boost::asio::awaitable<void>(connect_context&)>> connect_callbacks;
   std::vector<std::function<boost::asio::awaitable<void>(const application_context&)>>
      after_initialize_callbacks;
   std::function<int(application_shell&)> foreground;
};

} // namespace forge::app
