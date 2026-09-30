module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module package.chain_api_component.p2p_runtime;

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.app.application;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.blocking;
import forge.config.core.value;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.plugin;
import forge.plugins.net.p2p.resolver.types;

namespace package_chain_api_component {

namespace crypto_secrets = forge::plugins::crypto::secrets;

namespace {

inline constexpr auto certificate = std::string_view{R"(-----BEGIN CERTIFICATE-----
MIICpDCCAYwCCQCJjaEDxrQqBzANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAkx
MjcuMC4wLjEwHhcNMjYwNDI5MDgwMTMzWhcNMjYwNDMwMDgwMTMzWjAUMRIwEAYD
VQQDDAkxMjcuMC4wLjEwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDy
sbPH/R4QUz725sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TX
gl9tHkNpKmI92s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+
x7MRWXfKYd/ArGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOS
lI/lDqIjZxo7jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuv
M+mTj6eO4UQ42w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXj
nPOZzBinLRTDnE59HbDZAgMBAAEwDQYJKoZIhvcNAQELBQADggEBAHSOUQTEDgjC
uwza9ayfThJTs43j+TziWHLlowqCiHt/ipRNFEW7L0ibTnbMdQBFGfaLkTAhc5Rd
6O6x+9o76pgEYxEg0rDkgNXmprNmS+nL7Are+iiF6R+X8dts3MQgtONPApAXE96P
/n5K4GDQTd3WCI37hkmJA6rmwziFDTlwqtKWts39g8PqAbXac27rVR/iD0gWdOws
qiaoGj/0WW9qcgjYGdCc0/CbbnyiWbi48VVf0yyfm7wgcz90byaKIQchHdb/qjyU
wy7nfU5TJ5MKQ5yeqPTWmPYZZp9TKa5VD6wZD/IH7jH3GdJ/fSyroVLZktVnmxJa
dmG/9wwivwQ=
-----END CERTIFICATE-----
)"};

inline constexpr auto private_key = std::string_view{R"(-----BEGIN PRIVATE KEY-----
MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQDysbPH/R4QUz72
5sY376knXjSDCA+O5+Udwqfl4qaXHTAooWfplVY/WFRCnnMV6+TXgl9tHkNpKmI9
2s4O/LuJ5xnCCPX8k5i70gSnaGpClYSx+0gix8QgddDDsbLbIU/+x7MRWXfKYd/A
rGNelPMadlvmcoEhumVUAwjYSV26GhNAmUacJlho3ltyujYSGFOSlI/lDqIjZxo7
jbAGMMpiyu1omQ5nxjTm+bfOTcksBRMQP8mDz0vYXHXirA+xDfuvM+mTj6eO4UQ4
2w+iVLqhSPEhfLURmR4NULtPmq9hT7d1wS/Ys9q4Hj/j+kcXRCXjnPOZzBinLRTD
nE59HbDZAgMBAAECggEBAIWVjHhy+V5RA+JRCh/12ayirNLG2BF30OP9pf7iL4IT
/dMPbKvkmDGLw+1bW8tgKXj5+N6N/trfCm4zhqI3OF7ihooH9qYM88/F/OvMjFiU
BhMVVhJW1LxtPPjKUcFN58M8VnMhRM9v6gIaoSOJZvpU1abVtgBDocyJUxAB6gYp
i7MzoRwHGsL5mW/luE5H92/S8NNwLWBDA7DIGfrTZ6POf92h5I5W3CuTcqR5FICz
3pfU3i443yZmsmkc9duH2gZ9cb9j4pRtNLbbsGmRVrBlgnkVFk8JWbikc8MpLeKO
VKP7A2NvxJIrc7oFYrf4hbw8P70YL7S9B3W3yBPPzJECgYEA+Y3nG8CtvVTE/Keo
qb5Rljlnj9DEffrylLyYUYfSSNR4Olc2WCPBiz0rPCDdO0VGeXAwqLf2VP7IEyAx
kvrnqhzHWMhiLv+k4tIVyKCwpuofN0JsoUCi7CwRf+H2Pg+t6ewLV116THKsd41H
IRElWyEvZsmbbhlLrsxUtfFZWnUCgYEA+PZwXUn+cb8kRmfG959gMawTtcfvnBUX
sIn7LQl/ZWUIiLMWCaS3FbqkiGjaEYo6om1invYNJNA9zp/ECauSDp58NICCL0ie
L7z26sEa6Ocg2VdR4ezpN3cM6dyAKfTFGb9V6qjyqNIPCE4eey6ZJ+CU/mpEfSDu
+RGMzfdDCFUCgYEA5FRUn0zk6jU0YyMXq+9pgLSXL7vI/Kdt6m7AQuCto1tbga2o
GG7mt/pIo6RCJufUemoO62AeL1hKQU2UbjHJYxkfv/jf9LaM68dijQWRe7b8xres
4sFcEBCmFkbt4YzBCCWjntT1gBrv+Ba4fOXOMxoi374Yy1yzpYRpAWuI4L0CgYAn
u1SlXrivuHx2i/tR62pzou2mVhkkRK16LBsczeY57UzWXBZJRbM+UYIOjwU2RWQk
JebWTZg9ZspmXlLv5CS0FpDl5BhiqWktXy/cuSKtRq2UYf4cWy3A/0vdSqZdi8Wk
3Uc94uaPEK77eVQd/orMtWexzo3NlmLs9uMMv8g/3QKBgQCbik0UoJkkqNRMmWG8
dKQzj58eRI8fmKdJlWNfj2QMspd2vXMbsWYgAbFbU1QcVs1n8PxNydM+cfy77w8q
NWMlYP7rUFQ3ekYWqrRlshZdJ/h24PALd1nPCvhc4C9dvn+zW3BLVez1lBuFO8n8
0YkgmTgW7Ieibqnf4DqYp//nkw==
-----END PRIVATE KEY-----
)"};

} // namespace

p2p_dependency::p2p_dependency(std::string id) : id_{std::move(id)} {}

forge::app::plugin_id p2p_dependency::id() const {
   return forge::app::plugin_id{.value = id_};
}

std::string p2p_dependency::version() const {
   return "test";
}

boost::asio::awaitable<void> p2p_dependency::initialize(forge::app::plugin_context&) {
   co_return;
}

boost::asio::awaitable<void> p2p_dependency::startup() {
   co_return;
}

boost::asio::awaitable<void> p2p_dependency::shutdown() {
   co_return;
}

boost::asio::awaitable<crypto_secrets::snapshot> p2p_secrets_api::status(crypto_secrets::query) {
   co_return crypto_secrets::snapshot{.configured_secrets = 2};
}

boost::asio::awaitable<crypto_secrets::get_result> p2p_secrets_api::get_bytes(crypto_secrets::get_request request) {
   const auto* material = request.secret_id == "p2p/test-certificate"   ? &certificate
                          : request.secret_id == "p2p/test-private-key" ? &private_key
                                                                        : nullptr;
   if (material == nullptr) {
      throw std::runtime_error{"unknown P2P package-test secret"};
   }
   auto bytes = decltype(crypto_secrets::get_result{}.bytes){};
   bytes.reserve(material->size());
   for (const auto value : *material) {
      bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
   }
   co_return crypto_secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = std::move(bytes)};
}

boost::asio::awaitable<crypto_secrets::derive_result>
p2p_secrets_api::derive_hkdf_sha256(crypto_secrets::derive_request) {
   throw std::logic_error{"P2P package-test secrets do not implement derivation"};
   co_return crypto_secrets::derive_result{};
}

boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
p2p_secrets_api::encrypt_aes_gcm(crypto_secrets::aead_encrypt_request) {
   throw std::logic_error{"P2P package-test secrets do not implement encryption"};
   co_return crypto_secrets::aead_encrypt_result{};
}

boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
p2p_secrets_api::decrypt_aes_gcm(crypto_secrets::aead_decrypt_request) {
   throw std::logic_error{"P2P package-test secrets do not implement decryption"};
   co_return crypto_secrets::aead_decrypt_result{};
}

p2p_secrets_plugin::p2p_secrets_plugin() : p2p_dependency{"forge.plugins.crypto.secrets"} {}

boost::asio::awaitable<void> p2p_secrets_plugin::provide(forge::api::core::provider& provider) {
   provider.install<crypto_secrets::api>(std::make_shared<p2p_secrets_api>());
   co_return;
}

void register_p2p_stack(forge::app::plugin_registry& registry) {
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.db.store"},
       .factory = [] { return std::make_unique<p2p_dependency>("forge.plugins.db.store"); },
   });
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "forge.plugins.crypto.secrets"},
       .factory = [] { return std::make_unique<p2p_secrets_plugin>(); },
   });
   registry.register_plugin(forge::plugins::net::p2p::node::descriptor());
}

chain_api_publisher::chain_api_publisher(std::shared_ptr<const p2p_publication_callbacks> callbacks)
    : callbacks_{std::move(callbacks)} {}

forge::app::plugin_id chain_api_publisher::id() const {
   return forge::app::plugin_id{.value = "chain-api-publisher"};
}

std::string chain_api_publisher::version() const {
   return "1";
}

boost::asio::awaitable<void> chain_api_publisher::initialize(forge::app::plugin_context& context) {
   auto resolver = context.apis().get<forge::plugins::net::p2p::resolver::api>(
       {.id = {"forge.plugins.net.p2p.resolver"}, .major = 2, .min_revision = 0});
   publication_ = resolver->publish_api(
       callbacks_->binding(context), forge::net::p2p::protocol_id{.value = std::string{chain_api_protocol}},
       forge::plugins::net::p2p::resolver::publish_options{
           .transport = forge::api::transport::options{.max_frame_size = chain_api_max_frame_size},
       });
   co_return;
}

boost::asio::awaitable<void> chain_api_publisher::startup() {
   co_return;
}

boost::asio::awaitable<void> chain_api_publisher::shutdown() {
   co_await publication_.async_close();
}

p2p_server_application::p2p_server_application(p2p_publication_callbacks callbacks)
    : callbacks_{std::make_shared<p2p_publication_callbacks>(std::move(callbacks))} {}

void p2p_server_application::on_register_plugins(forge::app::plugin_registry& registry) {
   register_p2p_stack(registry);
   registry.register_plugin(forge::plugins::net::p2p::resolver::descriptor());
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = forge::app::plugin_id{.value = "chain-api-publisher"},
       .dependencies = {forge::app::plugin_id{.value = "forge.plugins.net.p2p.resolver"}},
       .factory = [callbacks = callbacks_] { return std::make_unique<chain_api_publisher>(callbacks); },
   });
}

boost::asio::awaitable<void> p2p_server_application::on_provide(forge::app::application_context& context) {
   co_await callbacks_->install(context);
}

void p2p_client_application::on_register_plugins(forge::app::plugin_registry& registry) {
   register_p2p_stack(registry);
   registry.register_plugin(forge::plugins::net::p2p::resolver::descriptor());
}

forge::net::p2p::peer_id test_peer(std::uint8_t seed) {
   return forge::net::p2p::make_peer_id(
       {.type = forge::net::p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, seed)});
}

forge::config::core::document p2p_config(const forge::net::p2p::peer_id& peer) {
   auto config = forge::config::core::document{};
   config.set("plugins.net.p2p.node.allow-insecure-test-mode", true);
   config.set("plugins.net.p2p.node.identity.certificate-secret", "p2p/test-certificate");
   config.set("plugins.net.p2p.node.identity.private-key-secret", "p2p/test-private-key");
   config.set("plugins.net.p2p.node.peer-id", peer.to_string());
   return config;
}

void shutdown_after_failure(forge::app::application_shell& application, bool started) noexcept {
   if (!started) {
      return;
   }
   application.request_stop();
   try {
      forge::asio::blocking::run(application.runtime(), application.shutdown());
   } catch (...) {
   }
}

} // namespace package_chain_api_component
