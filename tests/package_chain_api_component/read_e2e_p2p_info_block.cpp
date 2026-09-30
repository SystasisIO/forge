module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <coroutine>
#include <memory>
#include <thread>
#include <utility>

module package.chain_api_component.read_e2e_p2p_info_block;

import package.chain_api_component.p2p_runtime;
import package.chain_api_component.read_e2e_p2p_info_block_client;
import package.chain_api_component.read_e2e_p2p_info_block_server;
import package.chain_api_component.test_support;
import forge.api.core.registry;
import forge.app.application;
import forge.asio.blocking;
import forge.config.core.value;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.resolver.api;

namespace package_chain_api_component {

read_p2p_info_block_responses run_p2p_info_block_e2e(std::shared_ptr<read_p2p_info_block_fixture> state) {
   const auto server_peer = test_peer(0x41);
   auto server_config = p2p_config(server_peer);
   server_config.set("plugins.net.p2p.node.listen", forge::config::core::value::array_type{
                                                    forge::config::core::value{"/ip4/127.0.0.1/udp/0/quic-v1"},
                                                });
   auto server = p2p_server_application{make_p2p_info_block_publication(std::move(state))};
   auto client = p2p_client_application{};
   auto server_started = false;
   auto client_started = false;
   try {
      server.configure(server_config);
      forge::asio::blocking::run(server.runtime(), server.startup());
      server_started = true;
      auto server_node = server.apis().get<forge::plugins::net::p2p::node::api>(
          {.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0});
      const auto server_endpoint = server_node->local_endpoint();
      require(server_endpoint.has_value(), "P2P chain API server did not publish a local endpoint");

      auto client_config = p2p_config(test_peer(0x42));
      client_config.set("plugins.net.p2p.node.bootstrap", forge::config::core::value::array_type{
                                                          forge::config::core::value{server_endpoint->to_string()},
                                                      });
      client.configure(client_config);
      forge::asio::blocking::run(client.runtime(), client.startup());
      client_started = true;

      auto resolver = client.apis().get<forge::plugins::net::p2p::resolver::api>(
          {.id = {"forge.plugins.net.p2p.resolver"}, .major = 2, .min_revision = 0});
      const auto remote_apis = forge::asio::blocking::run(client.runtime(), resolver->peer_apis(server_peer));
      require(remote_apis.size() == 2, "P2P resolver did not advertise info/block APIs");
      for (const auto& api : remote_apis) {
         require(api.protocol == chain_api_protocol, "P2P resolver advertised a chain API on the wrong protocol");
         require(api.max_frame_size == chain_api_max_frame_size,
                 "P2P resolver advertised the wrong chain API frame limit");
      }
      const auto resolution = forge::asio::blocking::run(
          client.runtime(),
          resolver->resolve(server_peer, {.id = {"forge.chain.api.info"}, .major = 2, .min_revision = 0}));
      require(resolution.api.protocol == chain_api_protocol, "P2P resolver selected the wrong chain API protocol");
      auto node = client.apis().get<forge::plugins::net::p2p::node::api>(
          {.id = {"forge.plugins.net.p2p.node"}, .major = 2, .min_revision = 0});
      auto connection = forge::asio::blocking::run(
          client.runtime(),
          node->open_api_connection(server_peer, forge::net::p2p::protocol_id{.value = resolution.api.protocol}));
      const auto responses =
          forge::asio::blocking::run(client.runtime(), run_p2p_info_block_client(std::move(connection)));

      auto stop_thread = std::thread{[&client] { client.request_stop(); }};
      stop_thread.join();
      const auto shutdown_started = std::chrono::steady_clock::now();
      forge::asio::blocking::run(client.runtime(), client.shutdown());
      const auto shutdown_elapsed = std::chrono::steady_clock::now() - shutdown_started;
      client_started = false;
      require(shutdown_elapsed < std::chrono::seconds{2}, "P2P resolver client shutdown did not cancel promptly");
      require(client.state() == forge::app::application_state::stopped,
              "P2P resolver client did not reach stopped state");
      server.request_stop();
      forge::asio::blocking::run(server.runtime(), server.shutdown());
      server_started = false;
      require(server.state() == forge::app::application_state::stopped,
              "P2P chain API server did not reach stopped state");
      return responses;
   } catch (...) {
      shutdown_after_failure(client, client_started);
      shutdown_after_failure(server, server_started);
      throw;
   }
}

} // namespace package_chain_api_component
