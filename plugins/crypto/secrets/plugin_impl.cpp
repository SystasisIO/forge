module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.crypto.symmetric.aes;
import forge.crypto.symmetric.kdf;
import forge.crypto.core.random;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.exceptions;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.types;

#include "details/plugin_impl.hxx"
#include "details/secret_operations.hxx"

namespace forge::plugins::crypto::secrets {
plugin::impl::impl(forge::app::plugin_lifecycle value)
    : lifecycle{std::move(value)}, process{static_cast<std::uint64_t>(::getpid())},
      secrets{std::make_shared<const std::map<std::string, loaded_secret>>()} {}

std::shared_ptr<const std::map<std::string, loaded_secret>> plugin::impl::require_active() const {
   const auto state = lifecycle.snapshot();
   if (state.stop_requested || state.phase == forge::app::lifecycle_phase::created ||
       state.phase == forge::app::lifecycle_phase::configured) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Secrets plugin admission is closed");
   }
   auto current = std::shared_ptr<const std::map<std::string, loaded_secret>>{};
   if (!lifecycle.try_admit(state.generation,
                            [&] { current = std::atomic_load_explicit(&secrets, std::memory_order_acquire); })) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Secrets plugin admission is closed");
   }
   return current;
}

namespace {

template <typename SecretMap>
[[nodiscard]] const typename SecretMap::mapped_type& find_secret(const SecretMap& secrets, std::string_view id) {
   const auto found = secrets.find(std::string{id});
   if (found == secrets.end()) {
      FORGE_THROW_EXCEPTION(exceptions::secret_not_found, "secret is not configured",
                            forge::exceptions::ctx("secret_id", std::string{id}));
   }
   return found->second;
}

} // namespace

snapshot plugin::impl::status(query) const {
   const auto current = std::atomic_load_explicit(&secrets, std::memory_order_acquire);
   auto summaries = std::vector<secret_summary>{};
   summaries.reserve(current->size());
   for (const auto& [ignored, secret] : *current) {
      summaries.push_back(secret_summary{
          .id = secret.id,
          .kind = secret.kind,
          .purposes = secret.purposes,
          .operations = secret.operations,
          .allow_raw_export = secret.allow_raw_export,
      });
   }
   return snapshot{
       .configured_secrets = static_cast<std::uint64_t>(current->size()),
       .stopping = lifecycle.snapshot().stop_requested,
       .secrets = std::move(summaries),
   };
}

get_result plugin::impl::get_bytes(get_request value) const {
   const auto current = require_active();
   const auto& secret = find_secret(*current, value.secret_id);
   return secret_operations::get_bytes(secret, std::move(value));
}

derive_result plugin::impl::derive_hkdf_sha256(derive_request value) const {
   const auto current = require_active();
   const auto& secret = find_secret(*current, value.secret_id);
   return secret_operations::derive_hkdf_sha256(secret, std::move(value));
}

aead_encrypt_result plugin::impl::encrypt_aes_gcm(aead_encrypt_request value) const {
   const auto current = require_active();
   const auto& secret = find_secret(*current, value.secret_id);
   return secret_operations::encrypt_aes_gcm(secret, std::move(value));
}

aead_decrypt_result plugin::impl::decrypt_aes_gcm(aead_decrypt_request value) const {
   const auto current = require_active();
   const auto& secret = find_secret(*current, value.secret_id);
   return secret_operations::decrypt_aes_gcm(secret, std::move(value));
}

} // namespace forge::plugins::crypto::secrets
