#pragma once

#include "version_catalog.hxx"

namespace forge::plugins::crypto::secrets {

struct version_payload {
   struct entry {
      version_ref ref;
      secret_kind kind = secret_kind::bytes;
      version_state state = version_state::available;
      version_policy policy;
   };

   version_payload() = default;
   ~version_payload();
   version_payload(const version_payload&) = delete;
   version_payload& operator=(const version_payload&) = delete;
   version_payload(version_payload&&) noexcept = default;
   version_payload& operator=(version_payload&&) = delete;

   static forge::crypto::core::secret_bytes encode(const version_catalog& value, std::uint64_t maximum);
   static forge::crypto::core::secret_bytes pack_exact(const version_payload& value, std::uint64_t maximum);
   static std::shared_ptr<version_catalog> decode(const forge::crypto::core::secret_bytes& value,
                                                  const version_store_config& settings);

   std::string schema = "forge.plugins.crypto.secrets.versions";
   std::uint32_t schema_version = 1;
   forge::crypto::core::bytes receipt_key;
   std::vector<std::pair<std::string, version_catalog::head>> heads;
   std::vector<entry> versions;
   std::vector<forge::crypto::core::bytes> materials;
   std::vector<std::pair<std::string, version_catalog::receipt>> receipts;
};

BOOST_DESCRIBE_STRUCT(version_payload::entry, (), (ref, kind, state, policy))
BOOST_DESCRIBE_STRUCT(version_payload, (), (schema, schema_version, receipt_key, heads, versions, materials, receipts))

} // namespace forge::plugins::crypto::secrets
