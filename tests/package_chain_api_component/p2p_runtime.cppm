module;

#include <boost/asio/awaitable.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

export module package.chain_api_component.p2p_runtime;

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.app.application;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.runtime;
import forge.config.core.document;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.crypto.secrets.api;
import forge.plugins.crypto.secrets.types;

namespace package_chain_api_component {

namespace crypto_secrets = forge::plugins::crypto::secrets;

class p2p_dependency : public forge::app::plugin {
 public:
   explicit p2p_dependency(std::string id);

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   std::string id_;
};

class p2p_secrets_api final : public crypto_secrets::api {
 public:
   boost::asio::awaitable<crypto_secrets::snapshot> status(crypto_secrets::query query) override;
   boost::asio::awaitable<crypto_secrets::get_result> get_bytes(crypto_secrets::get_request request) override;
   boost::asio::awaitable<crypto_secrets::derive_result>
   derive_hkdf_sha256(crypto_secrets::derive_request request) override;
   boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
   encrypt_aes_gcm(crypto_secrets::aead_encrypt_request request) override;
   boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
   decrypt_aes_gcm(crypto_secrets::aead_decrypt_request request) override;
};

class p2p_secrets_plugin final : public p2p_dependency {
 public:
   p2p_secrets_plugin();

 protected:
   boost::asio::awaitable<void> on_provide(forge::api::core::provider& provider) override;
};

void register_p2p_stack(forge::app::plugin_registry& registry);

} // namespace package_chain_api_component

export namespace package_chain_api_component {

inline constexpr auto chain_api_protocol = std::string_view{"/spine/chain/api/1"};
inline constexpr auto chain_api_max_frame_size = std::uint32_t{512U * 1024U};

struct p2p_publication_callbacks {
   std::function<boost::asio::awaitable<void>(forge::app::application_context&)> install;
   std::function<forge::api::core::binding_plan(forge::app::plugin_context&)> binding;
};

class chain_api_publisher final : public forge::app::plugin {
 public:
   explicit chain_api_publisher(std::shared_ptr<const p2p_publication_callbacks> callbacks);

   [[nodiscard]] forge::app::plugin_id id() const override;
   [[nodiscard]] std::string version() const override;

 protected:
   boost::asio::awaitable<void> on_initialize(forge::app::plugin_context& context) override;
   boost::asio::awaitable<void> on_startup() override;
   boost::asio::awaitable<void> on_shutdown() override;

 private:
   std::shared_ptr<const p2p_publication_callbacks> callbacks_;
   forge::api::p2p::publication publication_;
};

class p2p_server_application final : public forge::app::application_shell {
 public:
   explicit p2p_server_application(p2p_publication_callbacks callbacks);

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override;
   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override;

 private:
   std::shared_ptr<const p2p_publication_callbacks> callbacks_;
};

class p2p_client_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override;
};

[[nodiscard]] forge::net::p2p::peer_id test_peer(std::uint8_t seed);
[[nodiscard]] forge::config::core::document p2p_config(const forge::net::p2p::peer_id& peer);
void shutdown_after_failure(forge::app::application_shell& application, bool started) noexcept;

} // namespace package_chain_api_component
