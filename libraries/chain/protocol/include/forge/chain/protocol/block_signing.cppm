module;

#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>

#include <vector>

export module forge.chain.protocol.block_signing;

export import forge.chain.protocol.block;
import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.described;
import forge.variant.value;

export namespace forge::chain::protocol {

struct block_sign_request {
   chain_id chain;
   block_header header;
   std::vector<public_key> keys;
};

BOOST_DESCRIBE_STRUCT(block_sign_request, (), (chain, header, keys))

} // namespace forge::chain::protocol

FORGE_DECLARE_SERIALIZATION(forge::chain::protocol::block_sign_request)
