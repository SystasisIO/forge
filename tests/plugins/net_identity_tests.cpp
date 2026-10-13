#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <string>
#include <string_view>
#include <utility>

import forge.asio.runtime;
import forge.config.core.document;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.resolver.plugin;
import forge.plugins.net.p2p.resolver.exceptions;

namespace {

void run(forge::asio::runtime& runtime, boost::asio::awaitable<void> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(std::chrono::seconds{5}) != std::future_status::ready) { std::terminate(); }
   result.get();
}

template <typename Plugin, typename Error> void check_identity(std::string_view leaf) {
   const auto section = "plugins.net." + std::string{leaf};
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   for (const auto mixed : {false, true}) {
      auto plugin = Plugin{};
      auto cleanup = boost::scope::scope_exit{[&] {
         try { run(runtime, plugin.shutdown()); }
         catch (...) { BOOST_ERROR("retired-config identity owner cleanup failed"); }
      }};
      BOOST_TEST(plugin.id().value == "forge." + section);
      const auto descriptor = plugin.describe_config();
      BOOST_REQUIRE(descriptor);
      BOOST_TEST(descriptor->section == section);
      forge::config::core::document settings;
      settings.set("plugins." + std::string{leaf} + ".enabled", false);
      if (mixed) {
         settings.set(section + ".enabled", true);
      }
      BOOST_CHECK_EXCEPTION(
          run(runtime, plugin.configure({settings, section})), Error, [](const Error& error) {
             return std::string_view{error.what()}.find("retired plugin configuration") != std::string_view::npos;
          });
   }
}

} // namespace

BOOST_AUTO_TEST_CASE(network_plugin_identities_and_retired_config_are_explicit) {
   namespace net = forge::plugins::net;
   check_identity<net::http::server::plugin, net::http::server::exceptions::invalid_config>("http.server");
   check_identity<net::p2p::node::plugin, net::p2p::node::exceptions::invalid_config>("p2p.node");
   check_identity<net::p2p::resolver::plugin, net::p2p::resolver::exceptions::invalid_config>("p2p.resolver");
}

BOOST_AUTO_TEST_CASE(p2p_node_rejects_retired_standalone_pubsub_and_diagnostics_sections) {
   namespace node = forge::plugins::net::p2p::node;
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   for (const auto family : {"plugins.p2p.", "plugins.net.p2p."}) {
      for (const auto role : {"pubsub", "diagnostics"}) {
         auto plugin = node::plugin{};
         auto cleanup = boost::scope::scope_exit{[&] {
            try { run(runtime, plugin.shutdown()); }
            catch (...) { BOOST_ERROR("retired standalone section owner cleanup failed"); }
         }};
         forge::config::core::document settings;
         settings.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
         settings.set(std::string{family} + role + ".enabled", false);
         BOOST_CHECK_THROW(run(runtime,
             plugin.configure({settings, "plugins.net.p2p.node"})), node::exceptions::invalid_config);
      }
   }
}
