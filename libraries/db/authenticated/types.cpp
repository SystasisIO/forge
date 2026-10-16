module;

#include <forge/raw/serialization.hpp>

module forge.db.authenticated.types;

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;

FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::db::authenticated::point_proof)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::db::authenticated::range_proof)
FORGE_IMPLEMENT_SERIALIZATION_PACK(forge::db::authenticated::checkpoint_record)
