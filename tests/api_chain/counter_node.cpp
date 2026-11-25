module;

#include <boost/asio/awaitable.hpp>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

module test.api_chain.counter_client;

import package.chain_api_component.p2p_runtime;
import forge.app.application_builder;
import forge.asio.blocking;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.resolver.plugin;
import forge.config.core.value;

namespace demo {
using app = forge::app::application_shell;

namespace fixture = package_chain_api_component;
std::unique_ptr<app> start_client(forge::config::core::document config, forge::app::plugin_descriptor delivery,
                                  std::function<void(forge::app::application_context&)> signing,
                                  std::string p2p_bootstrap) {
   forge::app::application_builder builder;
   builder.name("chain-counter-client")
       .runtime({.worker_threads = 4})
       .compute({.worker_threads = 2})
       .plugin(database_plugin())

       .plugin(std::move(delivery))
       .provide([signing = std::move(signing)](forge::app::application_context& ctx) {
          provide_counter(ctx);
          signing(ctx);
       });
   if (!p2p_bootstrap.empty()) {
      builder.plugin(fixture::p2p_secrets_descriptor())
          .plugin(forge::plugins::net::p2p::node::plugin::descriptor())
          .plugin(forge::plugins::net::p2p::resolver::plugin::descriptor());
      auto network = fixture::p2p_config(fixture::test_peer(0x52));
      network.set("plugins.net.p2p.node.bootstrap",
                  forge::config::core::value::array_type{forge::config::core::value{std::move(p2p_bootstrap)}});
      config = forge::config::core::merge({std::move(network), std::move(config)});
   }
   auto client = std::move(builder).build();
   client->configure(config);
   forge::asio::blocking::run(client->runtime(), client->startup());
   return client;
}

std::string endpoint(app& server) {
   using node = forge::plugins::net::p2p::node::api;
   return server.apis().get<node>(node::ref())->local_endpoint().value().to_string();
}
} // namespace demo
