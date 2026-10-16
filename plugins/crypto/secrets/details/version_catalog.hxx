#pragma once

#include "loaded_secret.hxx"

namespace forge::plugins::crypto::secrets {

struct version_catalog {
   enum class mutation_kind : std::uint8_t {
      create,
      revoke,
   };

   struct head {
      std::uint64_t latest_version = 0;
      std::uint64_t revision = 0;
   };

   struct entry {
      version_ref ref;
      std::shared_ptr<const loaded_secret> secret;
      version_state state = version_state::available;
   };

   struct receipt {
      mutation_kind kind = mutation_kind::create;
      std::array<std::uint8_t, 32> fingerprint{};
      version_mutation_result result;
   };

   std::shared_ptr<const forge::crypto::core::secret_bytes> receipt_key;
   std::map<std::string, head> heads;
   std::map<std::pair<std::string, std::uint64_t>, entry> versions;
   std::map<std::string, receipt> receipts;
   std::uint64_t material_bytes = 0;
   std::uint64_t encoded_bytes = 0;
};

BOOST_DESCRIBE_ENUM(version_catalog::mutation_kind, create, revoke)
BOOST_DESCRIBE_STRUCT(version_catalog::head, (), (latest_version, revision))
BOOST_DESCRIBE_STRUCT(version_catalog::receipt, (), (kind, fingerprint, result))

} // namespace forge::plugins::crypto::secrets
