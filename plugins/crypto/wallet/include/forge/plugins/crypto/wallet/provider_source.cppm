module;

#include <forge/api/core/macros.hpp>
#include <memory>
#include <string>

export module forge.plugins.crypto.wallet.provider_source;
import forge.api.core.binding;
import forge.crypto.signer.provider;

export namespace forge::plugins::crypto::wallet {

// Local composition only. Never publish this capability on a transport.
// Returns an existing stable provider without opening/unlocking the keystore.
class provider_source : public forge::api::core::contract<provider_source, forge::api::core::surface::local> {
 public:
   virtual ~provider_source() = default;
   virtual std::shared_ptr<forge::crypto::signer::provider> get(std::string name) = 0;
};

} // namespace forge::plugins::crypto::wallet

FORGE_EXPORT_API(::forge::plugins::crypto::wallet::provider_source,
                 FORGE_API_CONTRACT("forge.plugins.crypto.wallet.provider_source", 1, 0))
