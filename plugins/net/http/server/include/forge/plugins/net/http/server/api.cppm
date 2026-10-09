module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <memory>
#include <utility>

export module forge.plugins.net.http.server.api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.dispatcher;
import forge.api.core.binding;
import forge.api.http.binding;
import forge.net.http.assets;
import forge.plugins.net.http.server.middleware;
import forge.plugins.net.http.server.types;
export import forge.plugins.net.http.server.routes;

export namespace forge::plugins::net::http::server {

class api : public forge::api::core::contract<api, forge::api::core::surface::local> {
 public:
   virtual ~api() = default;

   virtual boost::asio::awaitable<void> use(middleware_descriptor descriptor) = 0;
   virtual boost::asio::awaitable<void> mount_assets(forge::net::http::asset_mount value) = 0;
   virtual boost::asio::awaitable<void> mount_routes(route_mount value);
   virtual boost::asio::awaitable<void> reload_tls() = 0;

   template <typename Interface> boost::asio::awaitable<void> publish(publish_options options = {}) {
      co_await publish(std::make_unique<typed_binding_spec<Interface>>(), std::move(options));
   }

 protected:
   class binding_spec {
    public:
      virtual ~binding_spec() = default;
      [[nodiscard]] virtual forge::api::http::binding_plan build(const forge::api::core::registry& registry) const = 0;
   };

 private:
   template <typename Interface> class typed_binding_spec final : public binding_spec {
    public:
      [[nodiscard]] forge::api::http::binding_plan build(const forge::api::core::registry& registry) const override {
         auto plan = forge::api::core::binding().serve(registry).build();
         return forge::api::http::binding().use(std::move(plan)).bind<Interface>().build();
      }
   };

   [[nodiscard]] virtual const forge::api::core::registry& registry() const = 0;
   virtual boost::asio::awaitable<void> publish(std::unique_ptr<binding_spec> binding, publish_options options) = 0;
};

} // namespace forge::plugins::net::http::server

FORGE_EXPORT_API(::forge::plugins::net::http::server::api, FORGE_API_CONTRACT("forge.plugins.net.http.server", 2, 1))
