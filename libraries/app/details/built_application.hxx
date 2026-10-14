#pragma once

#include "builder_state.hxx"

namespace forge::app {

class built_application final : public application_shell {
 public:
   explicit built_application(builder_state state);

 protected:
   void on_describe_config(forge::config::core::component_registry& registry) const override;
   boost::asio::awaitable<void> on_configure(configure_context& context) override;
   void on_register_plugins(plugin_registry& registry) override;
   boost::asio::awaitable<void> on_provide(application_context& context) override;
   boost::asio::awaitable<void> on_after_initialize(const application_context& context) override;
   boost::asio::awaitable<void> on_connect(connect_context& context) override;
   int on_run_foreground() override;

 private:
   builder_state state_;
};

} // namespace forge::app
