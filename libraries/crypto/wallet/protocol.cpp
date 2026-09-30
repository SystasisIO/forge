module;

#include <forge/raw/serialization.hpp>

module forge.crypto.wallet.protocol;

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.value;

FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::wallet_request)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::password_request)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::timeout_request)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::key_request)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::import_request)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::wallet_status)
FORGE_IMPLEMENT_SERIALIZATION(forge::crypto::wallet::key_info)
