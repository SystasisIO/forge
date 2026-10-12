#include <boost/test/unit_test.hpp>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.app.application_builder;
import forge.app.plugin_registry;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.value;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.db.store.plugin;

#include "details/secure_node_fixture.hxx"

namespace forge::tests::p2p {
namespace {
using value = forge::config::core::value;

value secret(std::string id, std::string material, std::string purpose, std::string encoding = "raw",
             bool exportable = true) {
   return value::object_type{
       {"id", std::move(id)}, {"kind", "bytes"},
       {"source", value::object_type{{"type", "value"}, {"encoding", std::move(encoding)}, {"value", std::move(material)}}},
       {"purposes", value::array_type{value{std::move(purpose)}}},
       {"operations", value::array_type{value{"get_bytes"}}}, {"allow-raw-export", exportable}};
}

forge::config::core::document document(const std::filesystem::path& path, std::string backend,
                                       const identity_fixture& identity,
                                       const secure_node_fixture::secret_policy& policy, std::string security) {
   auto config = forge::config::core::document{};
   auto entries = value::array_type{
       secret("identity/cert", identity.certificate_pem, "p2p.identity.certificate"),
       secret("identity/key", identity.private_key_pem, "p2p.identity.private-key")};
   if (policy.include) {
      entries.push_back(secret("network/psk", policy.hex, policy.purpose, "hex", policy.exportable));
   }
   config.set("plugins.crypto.secrets.secrets", std::move(entries));
   config.set("plugins.db.store.stores", value::array_type{value::object_type{
       {"name", "peers"}, {"driver", std::move(backend)}, {"path", path.string()},
       {"object", value::object_type{{"family", "objectdb"}, {"write-policy", "single-writer"}}}}});
   config.set("plugins.net.p2p.node.peer-store.store", "peers");
   config.set("plugins.net.p2p.node.identity.certificate-secret", "identity/cert");
   config.set("plugins.net.p2p.node.identity.private-key-secret", "identity/key");
   config.set("plugins.net.p2p.node.allow-insecure-test-mode", false);
   config.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   config.set("plugins.net.p2p.node.topology.mode", "static-only");
   config.set("plugins.net.p2p.node.tcp.security", std::move(security));
   config.set("plugins.net.p2p.node.path.policy", "direct-only");
   config.set("plugins.net.p2p.node.relay.client-enabled", false);
   config.set("plugins.net.p2p.node.relay.server-enabled", false);
   config.set("plugins.net.p2p.node.relay.auto-discovery-enabled", false);
   if (policy.private_profile) { config.set("plugins.net.p2p.node.private-network.psk-secret", "network/psk"); }
   return config;
}
} // namespace

secure_node_fixture::directory::directory()
    : path(std::filesystem::temp_directory_path() /
           ("forge-private-plugin-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
   if (!std::filesystem::create_directory(path)) { throw std::runtime_error{"secure fixture directory collision"}; }
}

secure_node_fixture::directory::~directory() {
   auto error = std::error_code{};
   std::filesystem::remove_all(path, error);
   if (error) { BOOST_ERROR("secure fixture directory cleanup: " << error.message()); }
}

std::vector<std::string> secure_node_fixture::backends() {
   auto result = std::vector<std::string>{};
#if FORGE_HAS_MDBX
   result.emplace_back("mdbx");
#endif
#if FORGE_HAS_ROCKSDB
   result.emplace_back("rocksdb");
#endif
   return result;
}

secure_node_fixture::secure_node_fixture(const std::string& backend)
    : secure_node_fixture(backend, secret_policy{}) {}

secure_node_fixture::secure_node_fixture(const std::string& backend, secret_policy policy, std::string security,
                                         forge::config::core::document overrides)
    : identity(make_identity_fixture("private-plugin")) {
   auto node_descriptor = forge::plugins::net::p2p::node::plugin::descriptor();
   auto factory = std::move(node_descriptor.factory);
   node_descriptor.factory = [this, factory = std::move(factory)] {
      auto node = factory();
      _node = static_cast<forge::plugins::net::p2p::node::plugin*>(node.get());
      return node;
   };
   auto builder = forge::app::application_builder{};
   builder.name("private-plugin-test")
       .runtime(forge::asio::runtime_options{.worker_threads = 2})
       .plugin(forge::plugins::db::store::plugin::descriptor())
       .plugin(forge::plugins::crypto::secrets::plugin::descriptor())
       .plugin(std::move(node_descriptor));
   shell = std::move(builder).build();
   config = forge::config::core::merge({document(_directory.path / "peers", backend, identity, policy,
                                               std::move(security)), overrides});
   shell->configure(config);
}

void secure_node_fixture::start() { run(shell->startup(), "official application startup"); }

void secure_node_fixture::configure_node(const forge::config::core::document& candidate) {
   if (!_node) { throw std::logic_error{"official node factory has not run"}; }
   run(_node->configure({candidate, "plugins.net.p2p.node"}), "direct official node configuration");
   config = candidate;
}

void secure_node_fixture::request_stop_node() noexcept {
   if (_node) { _node->request_stop(); }
}

boost::asio::awaitable<void> secure_node_fixture::shutdown_node() {
   if (!_node) { throw std::logic_error{"official node factory has not run"}; }
   return _node->shutdown();
}

secure_node_fixture::~secure_node_fixture() {
   shell->request_stop();
   auto shutdown_failure = std::exception_ptr{};
   auto scheduler_failure = std::exception_ptr{};
   // Providers remain owned until node shutdown and the unconditional scheduler join finish.
   try { run(shell->shutdown(), "reverse plugin shutdown"); }
   catch (...) { shutdown_failure = std::current_exception(); }
   try { run(shell->scheduler().shutdown(), "unconditional scheduler join"); }
   catch (...) { scheduler_failure = std::current_exception(); }
   if (shutdown_failure) { BOOST_ERROR("secure fixture plugin shutdown failed"); }
   if (scheduler_failure) { BOOST_ERROR("secure fixture scheduler join failed"); }
}

} // namespace forge::tests::p2p
