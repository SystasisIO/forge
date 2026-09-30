#include <memory>
#include <string>

import forge.plugins.crypto.wallet.plugin;
import forge.crypto.signer.provider;
import forge.plugins.crypto.wallet.provider_source;
import forge.api.core.types;
import forge.api.core.connection;

static_assert(forge::api::core::local_interface<forge::plugins::crypto::wallet::provider_source>);
static_assert(!forge::api::core::remote_interface<forge::plugins::crypto::wallet::provider_source>);

int main() {
   namespace wallet = forge::plugins::crypto::wallet;
   auto provider = std::make_shared<wallet::provider>("producer");
   std::shared_ptr<forge::crypto::signer::provider> signer = provider;
   auto descriptor = wallet::plugin::descriptor({.wallets = {provider}});
   return signer && descriptor.id.value == "forge.plugins.crypto.wallet" && !descriptor.enabled_by_default ? 0 : 1;
}
