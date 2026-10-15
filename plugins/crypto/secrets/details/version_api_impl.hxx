#pragma once

namespace forge::plugins::crypto::secrets {

class plugin::version_api_impl final : public version_api {
 public:
   explicit version_api_impl(std::shared_ptr<impl> state);
   boost::asio::awaitable<version_get_result> get_bytes(version_get_request value) override;
   boost::asio::awaitable<version_derive_result> derive_hkdf_sha256(version_derive_request value) override;
   boost::asio::awaitable<version_aead_encrypt_result> encrypt_aes_gcm(version_aead_encrypt_request value) override;
   boost::asio::awaitable<version_aead_decrypt_result> decrypt_aes_gcm(version_aead_decrypt_request value) override;

 private:
   std::shared_ptr<impl> state_;
};

} // namespace forge::plugins::crypto::secrets
