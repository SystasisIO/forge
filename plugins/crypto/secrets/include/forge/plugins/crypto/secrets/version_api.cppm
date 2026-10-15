module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.plugins.crypto.secrets.version_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.plugins.crypto.secrets.version_types;

export namespace forge::plugins::crypto::secrets {

class version_api : public forge::api::core::contract<version_api, forge::api::core::surface::local> {
 public:
   virtual ~version_api() = default;
   virtual boost::asio::awaitable<version_get_result> get_bytes(version_get_request value) = 0;
   virtual boost::asio::awaitable<version_derive_result> derive_hkdf_sha256(version_derive_request value) = 0;
   virtual boost::asio::awaitable<version_aead_encrypt_result> encrypt_aes_gcm(version_aead_encrypt_request value) = 0;
   virtual boost::asio::awaitable<version_aead_decrypt_result> decrypt_aes_gcm(version_aead_decrypt_request value) = 0;
};

} // namespace forge::plugins::crypto::secrets

FORGE_EXPORT_API(
    ::forge::plugins::crypto::secrets::version_api, FORGE_API_CONTRACT("forge.plugins.crypto.secrets.version", 1, 0),
    FORGE_API_METHOD_TYPED(get_bytes, ::forge::plugins::crypto::secrets::version_get_request,
                           ::forge::plugins::crypto::secrets::version_get_result),
    FORGE_API_METHOD_TYPED(derive_hkdf_sha256, ::forge::plugins::crypto::secrets::version_derive_request,
                           ::forge::plugins::crypto::secrets::version_derive_result),
    FORGE_API_METHOD_TYPED(encrypt_aes_gcm, ::forge::plugins::crypto::secrets::version_aead_encrypt_request,
                           ::forge::plugins::crypto::secrets::version_aead_encrypt_result),
    FORGE_API_METHOD_TYPED(decrypt_aes_gcm, ::forge::plugins::crypto::secrets::version_aead_decrypt_request,
                           ::forge::plugins::crypto::secrets::version_aead_decrypt_result))
