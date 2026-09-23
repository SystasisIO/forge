module;

#include <forge/raw/serialization.hpp>

module forge.chain.protocol.block_signing;

import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.value;

FORGE_IMPLEMENT_SERIALIZATION(forge::chain::protocol::block_sign_request)
