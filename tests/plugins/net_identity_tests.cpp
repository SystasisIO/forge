#include <boost/test/unit_test.hpp>

#include <string>
#include <string_view>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.config.core.document;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.http.server.exceptions;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.resolver.plugin;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.diagnostics.plugin;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.pubsub.plugin;
import forge.plugins.net.p2p.pubsub.exceptions;

namespace {

template <typename Plugin, typename Error> void check_identity(std::string_view leaf) {
   auto plugin = Plugin{};
   const auto section = "plugins.net." + std::string{leaf};
   BOOST_TEST(plugin.id().value == "forge." + section);
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor);
   BOOST_TEST(descriptor->section == section);
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   for (const auto mixed : {false, true}) {
      forge::config::core::document settings;
      settings.set("plugins." + std::string{leaf} + ".enabled", false);
      if (mixed) {
         settings.set(section + ".enabled", true);
      }
      BOOST_CHECK_EXCEPTION(
          forge::asio::blocking::run(runtime, plugin.configure({settings, section})), Error, [](const Error& error) {
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
   check_identity<net::p2p::diagnostics::plugin, net::p2p::diagnostics::exceptions::invalid_config>("p2p.diagnostics");
   check_identity<net::p2p::pubsub::plugin, net::p2p::pubsub::exceptions::invalid_config>("p2p.pubsub");
}
