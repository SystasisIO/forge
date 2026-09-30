module;

#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>

#include <cstdint>
#include <string>
#include <vector>

export module forge.crypto.wallet.protocol;

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.schema.enums;
import forge.schema.object;
import forge.variant.described;
import forge.variant.value;

export namespace forge::crypto::wallet {

enum class operation : std::uint8_t {
   create,
   open,
   list,
   status,
   list_public_keys,
   unlock,
   lock,
   lock_all,
   set_timeout,
   create_key,
   import_key,
   remove_key,
};

enum class state : std::uint8_t { locked, unlocked, fault };

// Network records only. No filesystem paths, provider handles or persisted state.
struct wallet_request {
   std::string wallet;
};
struct password_request {
   std::string wallet;
   std::string password;
};
struct timeout_request {
   std::string wallet;
   std::uint32_t seconds = 300;
};
struct key_request {
   std::string wallet;
   std::string id;
};
struct import_request {
   std::string wallet;
   std::string id;
   std::string private_key;
};
struct wallet_status {
   std::string wallet;
   state status = state::locked;
   std::uint32_t timeout_seconds = 300;
};
struct key_info {
   std::string id;
   std::string key;
};

BOOST_DESCRIBE_ENUM(operation, create, open, list, status, list_public_keys, unlock, lock, lock_all, set_timeout,
                    create_key, import_key, remove_key)
BOOST_DESCRIBE_ENUM(state, locked, unlocked, fault)
BOOST_DESCRIBE_STRUCT(wallet_request, (), (wallet))
BOOST_DESCRIBE_STRUCT(password_request, (), (wallet, password))
BOOST_DESCRIBE_STRUCT(timeout_request, (), (wallet, seconds))
BOOST_DESCRIBE_STRUCT(key_request, (), (wallet, id))
BOOST_DESCRIBE_STRUCT(import_request, (), (wallet, id, private_key))
BOOST_DESCRIBE_STRUCT(wallet_status, (), (wallet, status, timeout_seconds))
BOOST_DESCRIBE_STRUCT(key_info, (), (id, key))

} // namespace forge::crypto::wallet

FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::wallet_request)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::password_request)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::timeout_request)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::key_request)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::import_request)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::wallet_status)
FORGE_DECLARE_SERIALIZATION(forge::crypto::wallet::key_info)

export template <> struct forge::schema::rules<forge::crypto::wallet::password_request> {
   static auto define() {
      using request = forge::crypto::wallet::password_request;
      auto schema = forge::schema::object<request>();
      schema.field<&request::wallet>("wallet").required().non_empty();
      schema.field<&request::password>("password").required().non_empty().secret();
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::crypto::wallet::import_request> {
   static auto define() {
      using request = forge::crypto::wallet::import_request;
      auto schema = forge::schema::object<request>();
      schema.field<&request::wallet>("wallet").required().non_empty();
      schema.field<&request::id>("id").required().non_empty();
      schema.field<&request::private_key>("private_key").required().non_empty().secret();
      return schema;
   }
};
