module;

#include <boost/describe.hpp>
#include <cstdint>
#include <string>
#include <vector>

export module forge.plugins.crypto.wallet.config;

export import forge.crypto.wallet.protocol;
import forge.schema.enums;
import forge.schema.object;

export namespace forge::plugins::crypto::wallet {

struct permission {
   std::string fingerprint;
   std::vector<std::string> wallets;
   std::vector<forge::crypto::wallet::operation> operations;
};

struct startup_wallet {
   std::string name;
   bool open = false;
   std::uint32_t timeout_seconds = 300;
   std::string unlock_file;
};

struct config {
   std::string directory = "wallets";
   std::vector<startup_wallet> wallets;
   std::vector<permission> permissions;
   bool publish_http = false;
   std::uint32_t max_wallets = 32;
   std::uint32_t max_pending = 64;
   std::uint32_t max_request_bytes = 32768;
};

BOOST_DESCRIBE_STRUCT(permission, (), (fingerprint, wallets, operations))
BOOST_DESCRIBE_STRUCT(startup_wallet, (), (name, open, timeout_seconds, unlock_file))
BOOST_DESCRIBE_STRUCT(config, (),
                      (directory, wallets, permissions, publish_http, max_wallets, max_pending, max_request_bytes))

} // namespace forge::plugins::crypto::wallet

export template <> struct forge::schema::rules<forge::plugins::crypto::wallet::permission> {
   static auto define() {
      using type = forge::plugins::crypto::wallet::permission;
      auto schema = forge::schema::object<type>();
      schema.field<&type::fingerprint>("fingerprint").required().non_empty();
      schema.field<&type::wallets>("wallets").min_items(1).max_items(256).each_non_empty();
      schema.field<&type::operations>("operations").min_items(1).max_items(12);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::wallet::startup_wallet> {
   static auto define() {
      using type = forge::plugins::crypto::wallet::startup_wallet;
      auto schema = forge::schema::object<type>();
      schema.field<&type::name>("name").required().non_empty();
      schema.field<&type::open>("open").default_value(false);
      schema.field<&type::timeout_seconds>("timeout-seconds").default_value(std::uint32_t{300}).range(0, 86400);
      schema.field<&type::unlock_file>("unlock-file").default_value("");
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::crypto::wallet::config> {
   static auto define() {
      using type = forge::plugins::crypto::wallet::config;
      auto schema = forge::schema::object<type>();
      schema.field<&type::directory>("directory").default_value("wallets").non_empty();
      schema.field<&type::wallets>("wallets")
          .items<forge::plugins::crypto::wallet::startup_wallet>()
          .unique_by<&forge::plugins::crypto::wallet::startup_wallet::name>()
          .max_items(256);
      schema.field<&type::permissions>("permissions")
          .items<forge::plugins::crypto::wallet::permission>()
          .max_items(1024);
      schema.field<&type::publish_http>("publish-http").default_value(false);
      schema.field<&type::max_wallets>("max-wallets").default_value(std::uint32_t{32}).range(1, 256);
      schema.field<&type::max_pending>("max-pending").default_value(std::uint32_t{64}).range(1, 4096);
      schema.field<&type::max_request_bytes>("max-request-bytes")
          .default_value(std::uint32_t{32768})
          .range(1024, 1048576);
      return schema;
   }
};
