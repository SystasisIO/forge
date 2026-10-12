#pragma once

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.registry;
import forge.app.application_shell;
import forge.asio.runtime;
import forge.config.core.document;
import forge.plugins.net.p2p.node.plugin;

namespace forge::tests::p2p {

class secure_node_fixture final {
 private:
   struct directory {
      const std::filesystem::path path;
      directory();
      ~directory();
   };
   directory _directory;
   forge::plugins::net::p2p::node::plugin* _node = nullptr;

 public:
   struct secret_policy {
      std::string hex = std::string(64, '1');
      std::string purpose = "p2p.private-network.psk";
      bool exportable = true;
      bool include = true;
      bool private_profile = true;
   };

   const identity_fixture identity;
   std::unique_ptr<forge::app::application_shell> shell;
   forge::config::core::document config;

   explicit secure_node_fixture(const std::string& backend);
   secure_node_fixture(const std::string& backend, secret_policy policy, std::string security = "noise",
                       forge::config::core::document overrides = {});
   ~secure_node_fixture();
   secure_node_fixture(const secure_node_fixture&) = delete;
   secure_node_fixture& operator=(const secure_node_fixture&) = delete;

   [[nodiscard]] static std::vector<std::string> backends();
   void start();
   // Invoke the official plugin directly, committing the fixture document only
   // after acceptance. This is not application_shell's post-init config guard.
   void configure_node(const forge::config::core::document& candidate);
   void request_stop_node() noexcept;
   [[nodiscard]] boost::asio::awaitable<void> shutdown_node();

   template<class T> static T join(std::future<T>& result, std::string_view phase) {
      if (result.wait_for(std::chrono::seconds{10}) != std::future_status::ready) {
         std::cerr << "secure node fixture exceeded its owned join budget: " << phase << '\n';
         std::terminate();
      }
      return result.get();
   }
   template<class T> T run(boost::asio::awaitable<T> operation, std::string_view phase = "operation") {
      auto result = boost::asio::co_spawn(shell->runtime().context(), std::move(operation), boost::asio::use_future);
      return join(result, phase);
   }
   template<class T> auto api() { return shell->apis().get<T>(T::ref()).shared(); }
};

} // namespace forge::tests::p2p
