#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.registry;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.blocking;
import forge.config.core.document;
import forge.config.core.value;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.swarm.node.api;
import forge.plugins.swarm.node.plugin;

namespace swarm_plugin_test {

namespace sn = forge::plugins::swarm::node;
namespace np = forge::plugins::net::p2p::node;
namespace secrets = forge::plugins::crypto::secrets;

class identity_api final : public secrets::api {
 public:
   explicit identity_api(std::string name) : _identity{forge::tests::p2p::make_identity_fixture(std::move(name))} {}
   boost::asio::awaitable<secrets::snapshot> status(secrets::query) override {
      co_return secrets::snapshot{.configured_secrets = 2};
   }
   boost::asio::awaitable<secrets::get_result> get_bytes(secrets::get_request request) override {
      const auto* source = request.secret_id == "swarm/certificate" ? &_identity.certificate_pem :
         request.secret_id == "swarm/private-key" ? &_identity.private_key_pem : nullptr;
      if (!source) throw std::invalid_argument{"unknown Swarm test secret"};
      co_return secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = {source->begin(), source->end()}};
   }
   boost::asio::awaitable<secrets::derive_result> derive_hkdf_sha256(secrets::derive_request) override {
      throw std::logic_error{"test identity cannot derive"}; co_return secrets::derive_result{};
   }
   boost::asio::awaitable<secrets::aead_encrypt_result> encrypt_aes_gcm(secrets::aead_encrypt_request) override {
      throw std::logic_error{"test identity cannot encrypt"}; co_return secrets::aead_encrypt_result{};
   }
   boost::asio::awaitable<secrets::aead_decrypt_result> decrypt_aes_gcm(secrets::aead_decrypt_request) override {
      throw std::logic_error{"test identity cannot decrypt"}; co_return secrets::aead_decrypt_result{};
   }
 private:
   forge::tests::p2p::identity_fixture _identity;
};

class dependency final : public forge::app::plugin {
 public:
   dependency(std::string id, std::shared_ptr<identity_api> identity = {}) : _id{std::move(id)}, _identity{std::move(identity)} {}
   forge::app::plugin_id id() const override { return {.value = _id}; }
   std::string version() const override { return "test"; }
 protected:
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override {
      if (_identity) provider.install<secrets::api>(_identity);
      co_return;
   }
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context&) override { co_return; }
   boost::asio::awaitable<void> on_startup() override { co_return; }
   boost::asio::awaitable<void> on_shutdown() override { co_return; }
 private:
   std::string _id;
   std::shared_ptr<identity_api> _identity;
};

// Node, DHT, API publication and authenticated transport are official. Only
// identity provisioning and the unused (file-mode) DB dependency slot are fake.
class application final : public forge::app::application_shell {
 public:
   explicit application(std::string name, sn::plugin_options options = {})
      : forge::app::application_shell({.name = name, .runtime = {.worker_threads = 2},
           .compute = forge::asio::compute::pool::options{.worker_threads = 2}}),
        _identity{std::make_shared<identity_api>(std::move(name))}, _options{std::move(options)} {}
   forge::api::core::handle<sn::api> swarm() {
      return apis().get<sn::api>({.id = {"forge.plugins.swarm.node"}, .major = 1});
   }
   forge::api::core::handle<np::api> node() {
      return apis().get<np::api>({.id = {"forge.plugins.net.p2p.node"}, .major = 3});
   }
   forge::api::core::handle<np::dht_api> dht() {
      return apis().get<np::dht_api>({.id = {"forge.plugins.net.p2p.node.dht"}, .major = 1});
   }
   sn::plugin& swarm_owner() { return *_swarm; }
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin({.id = {"forge.plugins.db.store"},
         .factory = [] { return std::make_unique<dependency>("forge.plugins.db.store"); }});
      registry.register_plugin({.id = {"forge.plugins.crypto.secrets"},
         .factory = [identity = _identity] { return std::make_unique<dependency>("forge.plugins.crypto.secrets", identity); }});
      registry.register_plugin(np::plugin::descriptor());
      auto descriptor = sn::plugin::descriptor(_options);
      descriptor.factory = [this] {
         auto result = std::make_unique<sn::plugin>(_options);
         _swarm = result.get();
         return result;
      };
      registry.register_plugin(std::move(descriptor));
   }
 private:
   std::shared_ptr<identity_api> _identity;
   sn::plugin_options _options;
   sn::plugin* _swarm = nullptr;
};

inline forge::config::core::document configuration() {
   using value = forge::config::core::value;
   auto result = forge::config::core::document{};
   result.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   result.set("plugins.net.p2p.node.identity.certificate-secret", "swarm/certificate");
   result.set("plugins.net.p2p.node.identity.private-key-secret", "swarm/private-key");
   result.set("plugins.net.p2p.node.topology.mode", "static-only");
   result.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   result.set("plugins.net.p2p.node.dht.profiles", value::array_type{value{value::object_type{
      {"kind", "amino-v1"}, {"mode", "server"}, {"protocol", "/ipfs/kad/1.0.0"},
      {"peers", true}, {"providers", true}, {"values", true}}}});
   return result;
}

inline sn::plugin_options free_options() {
   auto options = sn::plugin_options{};
   options.initial_config.free_admission = true;
   return options;
}

inline std::vector<forge::multiformats::multiaddr> addresses(application& app) {
   auto result = std::vector<forge::multiformats::multiaddr>{};
   for (const auto& endpoint : app.node()->local_endpoints()) result.push_back(endpoint.to_multiaddr());
   return result;
}

} // namespace swarm_plugin_test
