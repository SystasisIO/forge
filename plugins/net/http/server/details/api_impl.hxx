#pragma once

namespace forge::plugins::net::http::server {

class plugin::api_impl final : public api {
 public:
   explicit api_impl(std::shared_ptr<plugin::impl> impl);

   [[nodiscard]] const forge::api::core::registry& registry() const override;
   boost::asio::awaitable<void> publish(std::unique_ptr<binding_spec> binding, publish_options options) override;
   boost::asio::awaitable<void> use(middleware_descriptor descriptor) override;
   boost::asio::awaitable<void> mount_assets(forge::net::http::asset_mount value) override;
   boost::asio::awaitable<void> mount_routes(route_mount value) override;
   boost::asio::awaitable<void> reload_tls() override;

 private:
   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::http::server
