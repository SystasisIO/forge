module;

#include <boost/asio/awaitable.hpp>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.keystore.ownership;
import forge.crypto.keystore.types;
import forge.plugins.crypto.secrets.version_api;
import forge.plugins.crypto.secrets.version_types;

#include "details/plugin_impl.hxx"
#include "details/version_store.hxx"
#include "details/version_api_impl.hxx"

namespace forge::plugins::crypto::secrets {

plugin::version_api_impl::version_api_impl(std::shared_ptr<impl> state) : state_{std::move(state)} {}

boost::asio::awaitable<version_get_result> plugin::version_api_impl::get_bytes(version_get_request value) {
   return state_->versions_store()->get(std::move(value));
}

boost::asio::awaitable<version_derive_result>
plugin::version_api_impl::derive_hkdf_sha256(version_derive_request value) {
   return state_->versions_store()->derive(std::move(value));
}

boost::asio::awaitable<version_aead_encrypt_result>
plugin::version_api_impl::encrypt_aes_gcm(version_aead_encrypt_request value) {
   return state_->versions_store()->encrypt(std::move(value));
}

boost::asio::awaitable<version_aead_decrypt_result>
plugin::version_api_impl::decrypt_aes_gcm(version_aead_decrypt_request value) {
   return state_->versions_store()->decrypt(std::move(value));
}

} // namespace forge::plugins::crypto::secrets
