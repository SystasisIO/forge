#pragma once

#include "loaded_secret.hxx"

namespace forge::plugins::crypto::secrets {

struct plugin::impl {
   explicit impl(forge::app::plugin_lifecycle lifecycle);
   const forge::app::plugin_lifecycle lifecycle;
   [[nodiscard]] std::shared_ptr<const std::map<std::string, loaded_secret>> require_active() const;
   [[nodiscard]] snapshot status(query value) const;
   [[nodiscard]] get_result get_bytes(get_request value) const;
   [[nodiscard]] derive_result derive_hkdf_sha256(derive_request value) const;
   [[nodiscard]] aead_encrypt_result encrypt_aes_gcm(aead_encrypt_request value) const;
   [[nodiscard]] aead_decrypt_result decrypt_aes_gcm(aead_decrypt_request value) const;

   std::shared_ptr<const std::map<std::string, loaded_secret>> secrets;
};

} // namespace forge::plugins::crypto::secrets
