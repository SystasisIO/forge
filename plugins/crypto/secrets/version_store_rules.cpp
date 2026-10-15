module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.crypto.secrets.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.keystore.ownership;
import forge.crypto.keystore.types;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.version_types;

#include "details/version_store.hxx"
#include "details/version_catalog.hxx"

namespace forge::plugins::crypto::secrets {

namespace {

bool allowed_by(const version_policy& value, const version_policy& ceiling) {
   return (!value.allow_raw_export || ceiling.allow_raw_export) &&
          value.max_plaintext_bytes <= ceiling.max_plaintext_bytes &&
          value.max_ciphertext_bytes <= ceiling.max_ciphertext_bytes && value.max_aad_bytes <= ceiling.max_aad_bytes &&
          std::ranges::all_of(value.purposes,
                              [&](const auto& purpose) {
                                 return std::ranges::find(ceiling.purposes, purpose) != ceiling.purposes.end();
                              }) &&
          std::ranges::all_of(value.operations, [&](auto operation) {
             return std::ranges::find(ceiling.operations, operation) != ceiling.operations.end();
          });
}

[[noreturn]] void invalid_catalog() {
   FORGE_THROW_EXCEPTION(exceptions::invalid_store, "Managed secret store state is invalid");
}

} // namespace

void version_store::validate_id(std::string_view value, std::uint64_t maximum) {
   if (value.empty() || value.size() > maximum ||
       std::ranges::any_of(value, [](unsigned char byte) { return byte < 32 || byte == 127; })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret identifier is invalid");
   }
}

void version_store::validate_ref(const version_ref& value) {
   validate_id(value.secret_id, 256);
   if (value.version == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "An explicit positive secret version is required");
   }
}

void version_store::validate_policy_shape(const version_policy& value) {
   if (value.purposes.empty() || value.purposes.size() > 32 || value.operations.empty() ||
       value.operations.size() > 4 || value.max_plaintext_bytes > aes_update_bytes_ceiling ||
       value.max_ciphertext_bytes > aes_update_bytes_ceiling || value.max_aad_bytes > aes_update_bytes_ceiling) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret policy is invalid");
   }
   auto purposes = std::set<std::string_view>{};
   auto operations = std::set<operation>{};
   for (const auto& purpose : value.purposes) {
      validate_id(purpose, 128);
      if (!purposes.emplace(purpose).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret policy has duplicate purposes");
      }
   }
   for (auto value_operation : value.operations) {
      if (value_operation < operation::get_bytes || value_operation > operation::decrypt_aes_gcm ||
          !operations.emplace(value_operation).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret policy has invalid operations");
      }
   }
   if (value.allow_raw_export && !operations.contains(operation::get_bytes)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed raw export requires the get_bytes operation");
   }
}

void version_store::validate_config() {
   if (!_settings.enabled) {
      return;
   }
   const auto path = std::filesystem::path{_settings.path};
   const auto bootstrap = std::filesystem::path{_settings.bootstrap_passphrase_file};
   if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
       !bootstrap.is_absolute() || _settings.max_secret_ids == 0 || _settings.max_secret_ids > 1024 ||
       _settings.max_versions == 0 || _settings.max_versions > 4096 || _settings.max_versions_per_secret == 0 ||
       _settings.max_versions_per_secret > 256 || _settings.max_material_bytes == 0 ||
       _settings.max_material_bytes > 1024U * 1024U || _settings.max_total_material_bytes == 0 ||
       _settings.max_total_material_bytes > 32U * 1024U * 1024U || _settings.max_store_plaintext_bytes == 0 ||
       _settings.max_store_plaintext_bytes > 64U * 1024U * 1024U || _settings.max_pending_calls == 0 ||
       _settings.max_pending_calls > 64 || _settings.max_pending_bytes == 0 ||
       _settings.max_pending_bytes > 8U * 1024U * 1024U ||
       (_settings.mode != version_open_mode::create_new && _settings.mode != version_open_mode::open_existing) ||
       _limits.max_scrypt_n < default_scrypt_n || _limits.max_scrypt_r < default_scrypt_r ||
       _limits.max_scrypt_p < default_scrypt_p || _limits.max_scrypt_memory_bytes < default_scrypt_max_memory_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Managed secret configuration is invalid");
   }
   try {
      validate_policy_shape(_settings.ceiling);
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Managed secret policy ceiling is invalid");
   }
   if (_settings.ceiling.max_plaintext_bytes == 0 || _settings.ceiling.max_ciphertext_bytes == 0 ||
       _settings.ceiling.max_aad_bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Managed secret policy ceiling requires positive limits");
   }
}

version_policy version_store::resolve_policy(version_policy value) const {
   validate_policy_shape(value);
   for (const auto& purpose : value.purposes) {
      if (std::ranges::find(_settings.ceiling.purposes, purpose) == _settings.ceiling.purposes.end()) {
         FORGE_THROW_EXCEPTION(exceptions::purpose_denied, "Managed secret purpose exceeds its configured ceiling");
      }
   }
   for (const auto operation : value.operations) {
      if (std::ranges::find(_settings.ceiling.operations, operation) == _settings.ceiling.operations.end()) {
         FORGE_THROW_EXCEPTION(exceptions::operation_denied, "Managed secret operation exceeds its configured ceiling");
      }
   }
   if (value.allow_raw_export && !_settings.ceiling.allow_raw_export) {
      FORGE_THROW_EXCEPTION(exceptions::operation_denied, "Managed raw export exceeds its configured ceiling");
   }
   value.max_plaintext_bytes =
       value.max_plaintext_bytes ? value.max_plaintext_bytes : _settings.ceiling.max_plaintext_bytes;
   value.max_ciphertext_bytes =
       value.max_ciphertext_bytes ? value.max_ciphertext_bytes : _settings.ceiling.max_ciphertext_bytes;
   value.max_aad_bytes = value.max_aad_bytes ? value.max_aad_bytes : _settings.ceiling.max_aad_bytes;
   if (!allowed_by(value, _settings.ceiling)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret limits exceed their configured ceiling");
   }
   std::ranges::sort(value.purposes);
   std::ranges::sort(value.operations);
   return value;
}

version_policy version_store::policy_of(const loaded_secret& value) {
   return {
       .purposes = value.purposes,
       .operations = value.operations,
       .allow_raw_export = value.allow_raw_export,
       .max_plaintext_bytes = value.max_plaintext_bytes,
       .max_ciphertext_bytes = value.max_ciphertext_bytes,
       .max_aad_bytes = value.max_aad_bytes,
   };
}

void version_store::require_capacity(const version_catalog& value, std::uint64_t encoded_size) const {
   auto available = std::uint64_t{};
   for (const auto& [key, entry] : value.versions) {
      if (entry.state == version_state::available) {
         ++available;
      }
   }
   constexpr auto receipt_reserve = std::uint64_t{1024};
   constexpr auto prefix_reserve = std::uint64_t{5};
   if (value.heads.size() > _settings.max_secret_ids || value.versions.size() > _settings.max_versions ||
       value.receipts.size() > _settings.max_versions * 2 ||
       value.material_bytes > _settings.max_total_material_bytes ||
       encoded_size > _settings.max_store_plaintext_bytes ||
       prefix_reserve > _settings.max_store_plaintext_bytes - encoded_size ||
       available > (_settings.max_store_plaintext_bytes - encoded_size - prefix_reserve) / receipt_reserve) {
      FORGE_THROW_EXCEPTION(exceptions::capacity_exceeded, "Managed secret store capacity is exhausted");
   }
}

void version_store::validate_catalog(const version_catalog& value) const {
   if (!value.receipt_key || value.receipt_key->size() != 32) {
      invalid_catalog();
   }
   auto revisions = std::map<std::string, std::set<std::uint64_t>>{};
   auto creates = std::map<std::pair<std::string, std::uint64_t>, std::uint64_t>{};
   auto revokes = std::map<std::pair<std::string, std::uint64_t>, std::uint64_t>{};
   for (const auto& [id, head] : value.heads) {
      try {
         validate_id(id, 256);
      } catch (const std::exception&) {
         invalid_catalog();
      }
      if (head.latest_version == 0 || head.latest_version > _settings.max_versions_per_secret ||
          head.revision < head.latest_version || head.revision > head.latest_version * 2) {
         invalid_catalog();
      }
      auto expected = std::uint64_t{1};
      for (auto found = value.versions.lower_bound({id, 0}); found != value.versions.end() && found->first.first == id;
           ++found) {
         if (found->first.second != expected++) {
            invalid_catalog();
         }
      }
      if (expected != head.latest_version + 1) {
         invalid_catalog();
      }
   }
   for (const auto& [key, entry] : value.versions) {
      if (!value.heads.contains(key.first) || entry.ref.secret_id != key.first || entry.ref.version != key.second ||
          !entry.secret || entry.secret->id != key.first ||
          (entry.secret->kind != secret_kind::bytes && entry.secret->kind != secret_kind::symmetric_key) ||
          (entry.state != version_state::available && entry.state != version_state::revoked)) {
         invalid_catalog();
      }
      const auto policy = policy_of(*entry.secret);
      try {
         validate_ref(entry.ref);
         validate_policy_shape(policy);
      } catch (const std::exception&) {
         invalid_catalog();
      }
      if (policy.max_plaintext_bytes == 0 || policy.max_ciphertext_bytes == 0 || policy.max_aad_bytes == 0 ||
          !std::ranges::is_sorted(policy.purposes) || !std::ranges::is_sorted(policy.operations)) {
         invalid_catalog();
      }
      if (entry.state == version_state::available && !allowed_by(policy, _settings.ceiling)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "Configured secret ceiling excludes a retained available version");
      }
   }
   for (const auto& [id, receipt] : value.receipts) {
      try {
         validate_id(id, 128);
         validate_ref(receipt.result.ref);
      } catch (const std::exception&) {
         invalid_catalog();
      }
      const auto key = std::pair{receipt.result.ref.secret_id, receipt.result.ref.version};
      const auto head = value.heads.find(key.first);
      if (head == value.heads.end() || !value.versions.contains(key) || receipt.result.revision == 0 ||
          receipt.result.revision > head->second.revision ||
          !revisions[key.first].emplace(receipt.result.revision).second) {
         invalid_catalog();
      }
      if (receipt.kind == version_catalog::mutation_kind::create) {
         if (!creates.emplace(key, receipt.result.revision).second) {
            invalid_catalog();
         }
      } else if (receipt.kind == version_catalog::mutation_kind::revoke) {
         if (!revokes.emplace(key, receipt.result.revision).second) {
            invalid_catalog();
         }
      } else {
         invalid_catalog();
      }
   }
   for (const auto& [key, entry] : value.versions) {
      const auto created = creates.find(key);
      const auto revoked = revokes.find(key);
      if (created == creates.end() || (entry.state == version_state::revoked) != (revoked != revokes.end()) ||
          (revoked != revokes.end() && revoked->second <= created->second)) {
         invalid_catalog();
      }
   }
   for (const auto& [id, head] : value.heads) {
      if (revisions[id].size() != head.revision) {
         invalid_catalog();
      }
      auto previous_create = std::uint64_t{};
      for (auto version = std::uint64_t{1}; version <= head.latest_version; ++version) {
         const auto created = creates.at({id, version});
         if ((version == 1 && created != 1) || created <= previous_create) {
            invalid_catalog();
         }
         previous_create = created;
      }
   }
   try {
      require_capacity(value, value.encoded_bytes);
   } catch (const std::exception&) {
      invalid_catalog();
   }
}

std::uint64_t version_store::input_size(const create_version_request& value) const {
   validate_id(value.secret_id, 256);
   validate_id(value.mutation_id, 128);
   if (value.material.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret material is empty");
   }
   if (value.material.size() > _settings.max_material_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "Managed secret material exceeds its size limit");
   }
   if (value.kind != secret_kind::bytes && value.kind != secret_kind::symmetric_key) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed secret material kind is invalid");
   }
   validate_policy_shape(value.policy);
   auto bytes =
       static_cast<std::uint64_t>(value.material.size() + value.secret_id.size() + value.mutation_id.size() + 128);
   for (const auto& purpose : value.policy.purposes) {
      bytes += purpose.size();
   }
   return bytes;
}

} // namespace forge::plugins::crypto::secrets
