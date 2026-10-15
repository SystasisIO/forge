#pragma once

namespace forge::plugins::crypto::secrets {

struct secret_operations {
   static get_result get_bytes(const loaded_secret& secret, get_request value);
   static derive_result derive_hkdf_sha256(const loaded_secret& secret, derive_request value);
   static aead_encrypt_result encrypt_aes_gcm(const loaded_secret& secret, aead_encrypt_request value);
   static aead_decrypt_result decrypt_aes_gcm(const loaded_secret& secret, aead_decrypt_request value);
};

} // namespace forge::plugins::crypto::secrets
