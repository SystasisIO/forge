module;

#if !defined(FORGE_CONTRACT_GUEST)
#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>
#endif

export module forge.chain.protocol.system;

export import :value;

#if !defined(FORGE_CONTRACT_GUEST)
import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;

export namespace forge::chain::protocol {
BOOST_DESCRIBE_STRUCT(newaccount, (), (creator, name, owner, active))
BOOST_DESCRIBE_STRUCT(setcode, (), (account, vmtype, vmversion, code))
BOOST_DESCRIBE_STRUCT(setabi, (), (account, abi))
BOOST_DESCRIBE_STRUCT(updateauth, (), (account, permission, parent, auth))
BOOST_DESCRIBE_STRUCT(deleteauth, (), (account, permission))
BOOST_DESCRIBE_STRUCT(linkauth, (), (account, code, type, requirement))
BOOST_DESCRIBE_STRUCT(unlinkauth, (), (account, code, type))
BOOST_DESCRIBE_STRUCT(canceldelay, (), (canceling_auth, trx_id))
BOOST_DESCRIBE_STRUCT(onerror, (), (sender_id, sent_trx))
BOOST_DESCRIBE_STRUCT(claimrewards, (), (owner))
} // namespace forge::chain::protocol

FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::newaccount)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::setcode)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::setabi)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::updateauth)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::deleteauth)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::linkauth)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::unlinkauth)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::canceldelay)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::onerror)
FORGE_DECLARE_SERIALIZATION_PACK(forge::chain::protocol::claimrewards)
#endif
