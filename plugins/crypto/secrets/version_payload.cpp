module;

#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;
import forge.crypto.core.constant_time;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.version_types;
import forge.raw.raw;

#include "details/version_payload.hxx"

namespace forge::plugins::crypto::secrets {
namespace {

[[noreturn]] void invalid_payload() {
   FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret store payload is invalid");
}

} // namespace

version_payload::~version_payload() {
   forge::crypto::core::secure_erase(receipt_key);
   for (auto& material : materials) {
      forge::crypto::core::secure_erase(material);
   }
}

forge::crypto::core::secret_bytes version_payload::encode(const version_catalog& value, std::uint64_t maximum) {
   auto payload = version_payload{};
   payload.receipt_key = value.receipt_key->copy();
   payload.heads.reserve(value.heads.size());
   payload.versions.reserve(value.versions.size());
   payload.materials.reserve(value.versions.size());
   payload.receipts.reserve(value.receipts.size());
   for (const auto& head : value.heads) {
      payload.heads.push_back(head);
   }
   for (const auto& [key, entry] : value.versions) {
      const auto& secret = *entry.secret;
      payload.versions.push_back(version_payload::entry{
          .ref = entry.ref,
          .kind = secret.kind,
          .state = entry.state,
          .policy =
              {
                  .purposes = secret.purposes,
                  .operations = secret.operations,
                  .allow_raw_export = secret.allow_raw_export,
                  .max_plaintext_bytes = secret.max_plaintext_bytes,
                  .max_ciphertext_bytes = secret.max_ciphertext_bytes,
                  .max_aad_bytes = secret.max_aad_bytes,
              },
      });
      payload.materials.emplace_back(secret.material.copy());
   }
   for (const auto& receipt : value.receipts) {
      payload.receipts.push_back(receipt);
   }
   return pack_exact(payload, maximum);
}

forge::crypto::core::secret_bytes version_payload::pack_exact(const version_payload& value, std::uint64_t maximum) {
   auto counter = forge::datastream<std::size_t>{};
   forge::raw::pack(counter, value);
   const auto size = counter.tellp();
   if (size > maximum) {
      FORGE_THROW_EXCEPTION(exceptions::capacity_exceeded, "Managed secret store payload capacity is exhausted");
   }
   auto output = forge::crypto::core::bytes(size);
   auto wipe = std::unique_ptr<forge::crypto::core::bytes,
                               decltype([](auto* bytes) { forge::crypto::core::secure_erase(*bytes); })>{&output};
   auto stream = forge::datastream<std::uint8_t*>{output.data(), output.size()};
   try {
      forge::raw::pack(stream, value);
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::capacity_exceeded, "Managed secret store payload capacity is exhausted");
   }
   if (stream.tellp() != size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret encoding is inconsistent");
   }
   return forge::crypto::core::secret_bytes{std::move(output)};
}

std::shared_ptr<version_catalog> version_payload::decode(const forge::crypto::core::secret_bytes& value,
                                                         const version_store_config& settings) {
   if (value.size() > settings.max_store_plaintext_bytes) {
      invalid_payload();
   }
   auto payload = version_payload{};
   try {
      forge::raw::unpack_exact(
          value.span(), payload,
          forge::raw::unpack_limits{
              .max_container_elements = static_cast<std::uint32_t>(
                  (std::max)({settings.max_versions * 2, settings.max_secret_ids, std::uint64_t{32}})),
              .max_total_container_elements =
                  static_cast<std::uint32_t>(settings.max_versions * 40 + settings.max_secret_ids + 32),
              .max_bytes = static_cast<std::uint32_t>((std::max)(settings.max_material_bytes, std::uint64_t{256})),
              .first_container_elements = static_cast<std::uint32_t>(
                  (std::max)({settings.max_versions * 2, settings.max_secret_ids, std::uint64_t{32}})),
          });
   } catch (const std::exception&) {
      invalid_payload();
   }
   if (payload.schema != "forge.plugins.crypto.secrets.versions" || payload.schema_version != 1 ||
       payload.receipt_key.size() != 32 || payload.heads.size() > settings.max_secret_ids ||
       payload.versions.size() > settings.max_versions || payload.materials.size() != payload.versions.size() ||
       payload.receipts.size() > settings.max_versions * 2) {
      invalid_payload();
   }
   // Raw's legacy enum codec may narrow an encoded integer. Require the exact
   // canonical typed representation before material enters the live catalog.
   const auto canonical = [&] {
      try {
         return pack_exact(payload, settings.max_store_plaintext_bytes);
      } catch (const std::exception&) {
         invalid_payload();
      }
   }();
   if (!forge::crypto::core::constant_time_equal(value.span(), canonical.span())) {
      invalid_payload();
   }
   auto catalog = std::make_shared<version_catalog>();
   catalog->receipt_key = std::make_shared<forge::crypto::core::secret_bytes>(std::move(payload.receipt_key));
   catalog->encoded_bytes = value.size();
   for (auto& head : payload.heads) {
      if (!catalog->heads.emplace(std::move(head)).second) {
         invalid_payload();
      }
   }
   for (auto index = std::size_t{}; index < payload.versions.size(); ++index) {
      auto& value = payload.versions[index];
      auto& material = payload.materials[index];
      if (material.empty() || material.size() > settings.max_material_bytes ||
          material.size() > settings.max_total_material_bytes - catalog->material_bytes) {
         invalid_payload();
      }
      catalog->material_bytes += material.size();
      auto resource = std::make_shared<loaded_secret>(loaded_secret{
          .id = value.ref.secret_id,
          .kind = value.kind,
          .material = forge::crypto::core::secret_bytes{std::move(material)},
          .purposes = std::move(value.policy.purposes),
          .operations = std::move(value.policy.operations),
          .allow_raw_export = value.policy.allow_raw_export,
          .max_plaintext_bytes = value.policy.max_plaintext_bytes,
          .max_ciphertext_bytes = value.policy.max_ciphertext_bytes,
          .max_aad_bytes = value.policy.max_aad_bytes,
      });
      auto key = std::pair{value.ref.secret_id, value.ref.version};
      if (!catalog->versions
               .emplace(std::move(key),
                        version_catalog::entry{
                            .ref = std::move(value.ref),
                            .secret = std::move(resource),
                            .state = value.state,
                        })
               .second) {
         invalid_payload();
      }
   }
   for (auto& receipt : payload.receipts) {
      if (!catalog->receipts.emplace(std::move(receipt)).second) {
         invalid_payload();
      }
   }
   return catalog;
}

} // namespace forge::plugins::crypto::secrets
