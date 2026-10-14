#include "readme_example.hxx"

#include <algorithm>
#include <utility>

import forge.app.connect_context;
import forge.config.core.decode;

namespace service_example {

forge::app::plugin_descriptor status_plugin::descriptor() {
   return {
      .id = {"status"},
      .factory = [] { return std::make_unique<status_plugin>(); },
   };
}

forge::app::plugin_id status_plugin::id() const {
   return {"status"};
}

std::string status_plugin::version() const {
   return "1";
}

std::optional<forge::config::core::component_descriptor> status_plugin::describe_config() const {
   return forge::config::core::describe_component<status_config>("status");
}

boost::asio::awaitable<void> status_plugin::on_configure(forge::config::core::component_view view) {
   batch_limit_ = view.get_or<std::uint16_t>("batch-limit", 32);
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_provide(forge::api::core::provider& provider) {
   provider.install<status_api>(std::make_shared<api_impl>(batch_limit_));
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_initialize(forge::app::plugin_context&) {
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_startup() {
   co_return;
}

boost::asio::awaitable<void> status_plugin::on_shutdown() {
   co_return;
}

status_plugin::api_impl::api_impl(std::uint16_t batch_limit) : batch_limit_{batch_limit} {}

std::uint16_t status_plugin::api_impl::batch_limit() const noexcept {
   return batch_limit_;
}

std::unique_ptr<forge::app::application_shell> make_application(const forge::app::daemon_context& context) {
   auto settings = std::make_shared<service_settings>();
   settings->data_path = context.environment.resolve_data_path("service");
   settings->profile = context.environment.profile();

   auto builder = forge::app::application_builder{};
   builder.name(context.shell.name)
      .environment(context.environment)
      .runtime(context.shell.runtime)
      .scheduler(context.shell.scheduler)
      .config<service_config>("service", [settings](const service_config& config) {
         settings->batch_size = config.batch_size;
      })
      .connect([settings](forge::app::connect_context& connect) {
         const auto status = connect.api<status_api>();
         settings->batch_size = std::min(settings->batch_size, status->batch_limit());
         connect.publish(settings);
      })
      .plugin(status_plugin::descriptor());
   if (context.shell.compute) {
      builder.compute(*context.shell.compute);
   }
   return std::move(builder).build();
}

forge::app::daemon_options daemon_options() {
   return {
      .name = "service",
      .display_name = "Service daemon",
      .default_data_dir_name = "service",
      .env_prefix = "SERVICE",
   };
}

} // namespace service_example
