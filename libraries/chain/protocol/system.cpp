module;

#include <forge/raw/serialization.hpp>

#include <new>

module forge.chain.protocol.system;

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::newaccount)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::setcode)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::setabi)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::updateauth)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::deleteauth)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::linkauth)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::unlinkauth)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::canceldelay)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::onerror)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::chain::protocol::claimrewards)
