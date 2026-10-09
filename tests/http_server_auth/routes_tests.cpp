#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>
#include <type_traits>

import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.api.core.registry;
import forge.config.core.document;
import forge.net.http.client;
import forge.net.http.base_url;
import forge.net.http.types;
import forge.net.http.assets;
import forge.net.http.route_context;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.types;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.http.server.plugin;

namespace forge::test::http_routes {

class legacy_api final : public forge::plugins::net::http::server::api {
 public:
   boost::asio::awaitable<void> use(forge::plugins::net::http::server::middleware_descriptor) override { co_return; }
   boost::asio::awaitable<void> mount_assets(forge::net::http::asset_mount) override { co_return; }
   boost::asio::awaitable<void> reload_tls() override { co_return; }

 private:
   const forge::api::core::registry& registry() const override { return _registry; }
   boost::asio::awaitable<void> publish(std::unique_ptr<binding_spec>,
       forge::plugins::net::http::server::publish_options) override { co_return; }
   forge::api::core::registry _registry;
};

static_assert(!std::is_abstract_v<legacy_api>);

forge::plugins::net::http::server::route_mount mount()
{
   return {.id = "test.application.routes", .reserved_paths = {"/native"},
       .apply = [](forge::net::http::router& router)
       {
          router.get("/native", [](forge::net::http::route_context& context) -> boost::asio::awaitable<forge::net::http::response>
          { co_return forge::net::http::make_text_response(context.request, forge::net::http::status::ok, "native-mounted"); });
       }};
}

} // namespace forge::test::http_routes

BOOST_AUTO_TEST_CASE(http_application_route_mount_has_compatible_default_and_closed_lifecycle)
{
   auto io = boost::asio::io_context{};
   auto reserved = boost::asio::ip::tcp::acceptor{io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
   const auto port = reserved.local_endpoint().port();
   reserved.close();
   auto builder = forge::app::application_builder{};
   builder.name("http-route-publication-test").plugin(forge::plugins::net::http::server::descriptor());
   auto app = std::move(builder).build();
   auto config = forge::config::core::document{};
   config.set("plugins.net.http.server.port", std::uint64_t{port});
   app->configure(config);
   forge::asio::blocking::run(app->runtime(), app->initialize());
   const auto api = app->apis().get<forge::plugins::net::http::server::api>(forge::plugins::net::http::server::api::ref());
   auto legacy = forge::test::http_routes::legacy_api{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), legacy.mount_routes(forge::test::http_routes::mount())),
       forge::plugins::net::http::server::exceptions::unsupported_route_mount);
   forge::asio::blocking::run(app->runtime(), api->mount_routes(forge::test::http_routes::mount()));
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), api->mount_routes(forge::test::http_routes::mount())),
       forge::plugins::net::http::server::exceptions::invalid_config);
   forge::asio::blocking::run(app->runtime(), app->startup());
   auto client = forge::net::http::client{app->runtime(), forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   const auto response = forge::asio::blocking::run(app->runtime(), client.async_get("/native"));
   BOOST_TEST(response.result_int() == 200U);
   BOOST_TEST(response.body() == "native-mounted");
   BOOST_CHECK_THROW(forge::asio::blocking::run(app->runtime(), api->mount_routes(forge::test::http_routes::mount())),
       forge::plugins::net::http::server::exceptions::publication_closed);
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}
