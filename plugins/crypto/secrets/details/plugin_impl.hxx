#pragma once

#include "loaded_secret.hxx"

namespace forge::plugins::crypto::secrets {

class version_store;

struct plugin::impl {
   explicit impl(forge::app::plugin_lifecycle lifecycle);
   const forge::app::plugin_lifecycle lifecycle;
   const std::uint64_t process;
   [[nodiscard]] std::shared_ptr<const std::map<std::string, loaded_secret>> require_active() const;
   [[nodiscard]] snapshot status(query value) const;
   [[nodiscard]] get_result get_bytes(get_request value) const;
   [[nodiscard]] derive_result derive_hkdf_sha256(derive_request value) const;
   [[nodiscard]] aead_encrypt_result encrypt_aes_gcm(aead_encrypt_request value) const;
   [[nodiscard]] aead_decrypt_result decrypt_aes_gcm(aead_decrypt_request value) const;
   [[nodiscard]] std::shared_ptr<version_store> configured_versions(forge::config::core::component_view view,
                                                                    const config& legacy) const;
   [[nodiscard]] std::shared_ptr<version_store> versions_store() const;
   boost::asio::awaitable<void> initialize_versions(forge::app::plugin_context& context);
   void stop_versions() noexcept;
   boost::asio::awaitable<void> shutdown_versions();

   std::shared_ptr<const std::map<std::string, loaded_secret>> secrets;
   std::shared_ptr<version_store> versions;
};

} // namespace forge::plugins::crypto::secrets
