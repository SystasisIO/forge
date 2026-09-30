module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <memory>
#include <utility>
#include <vector>

export module forge.plugins.net.p2p.resolver.managed_api;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.handle;
import forge.api.core.types;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.resolver.types;

export namespace forge::plugins::net::p2p::resolver {

class managed_api : public forge::api::core::contract<managed_api> {
 public:
   virtual ~managed_api() = default;

   template <typename Interface>
   boost::asio::awaitable<forge::api::core::handle<Interface>>
   remote(std::vector<forge::net::p2p::peer_id> ordered_peers, managed_remote_options options = {}) {
      static_assert(forge::api::core::remote_interface<Interface>,
                    "Interface must opt in to forge::api::core::surface::remote");
      auto descriptor = Interface::describe();
      auto requested = forge::api::core::api_ref{
          .id = descriptor.id,
          .major = descriptor.version.major,
          .min_revision = descriptor.version.revision,
      };
      auto invoker = co_await open_managed_remote(std::move(ordered_peers), requested, descriptor, options);
      co_return forge::api::core::handle<Interface>{
          std::make_shared<forge::api::core::proxy<Interface>>(std::move(invoker), std::move(requested))};
   }

 private:
   virtual boost::asio::awaitable<std::shared_ptr<forge::api::core::remote_invoker>>
   open_managed_remote(std::vector<forge::net::p2p::peer_id> ordered_peers, forge::api::core::api_ref requested,
                       forge::api::core::descriptor descriptor, managed_remote_options options) = 0;
};

} // namespace forge::plugins::net::p2p::resolver

FORGE_EXPORT_API(::forge::plugins::net::p2p::resolver::managed_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.resolver.managed", 1, 0))
