module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/describe.hpp>
#include <forge/exceptions/macros.hpp>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
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
import forge.api.core.exceptions;
import forge.crypto.core.constant_time;
import forge.crypto.digest.hmac;
import forge.plugins.crypto.secrets.exceptions;
import forge.plugins.crypto.secrets.version_types;
import forge.raw.raw;

#include "details/version_store.hxx"
#include "details/version_catalog.hxx"
#include "details/version_store_owned.hxx"
#include "details/secret_operations.hxx"

namespace forge::plugins::crypto::secrets {

void version_store::check_process() const {
   if (_process != static_cast<std::uint64_t>(::getpid())) {
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "Managed secret ownership is no longer valid");
   }
}

void version_store::check_start() const {
   check_process();
   if (_lifecycle.snapshot().stop_requested) {
      FORGE_THROW_EXCEPTION(exceptions::stopped, "Managed secret admission is closed");
   }
}

std::array<std::uint8_t, 32> version_store::fingerprint(const create_version_request& value,
                                                        const version_catalog& catalog) const {
   auto policy = value.policy;
   std::ranges::sort(policy.purposes);
   std::ranges::sort(policy.operations);
   auto bytes = forge::crypto::core::bytes(static_cast<std::size_t>(input_size(value) + 8192));
   auto wipe = std::unique_ptr<forge::crypto::core::bytes,
                               decltype([](auto* bytes) { forge::crypto::core::secure_erase(*bytes); })>{&bytes};
   auto stream = forge::datastream<std::uint8_t*>{bytes.data(), bytes.size()};
   forge::raw::pack(stream, std::string{"forge.secret-mutation.fingerprint.v1"}, std::uint8_t{0}, value.mutation_id,
                    value.secret_id, value.expected_revision, value.kind, policy,
                    static_cast<std::uint64_t>(value.material.size()));
   stream.write(reinterpret_cast<const char*>(value.material.span().data()), value.material.size());
   auto hmac = forge::crypto::digest::hmac_sha256{};
   return hmac.digest(catalog.receipt_key->span(), std::span<const std::uint8_t>{bytes.data(), stream.tellp()})
       .extract_as_byte_array();
}

std::array<std::uint8_t, 32> version_store::fingerprint(const revoke_version_request& value,
                                                        const version_catalog& catalog) const {
   auto bytes = forge::crypto::core::bytes(8192);
   auto wipe = std::unique_ptr<forge::crypto::core::bytes,
                               decltype([](auto* bytes) { forge::crypto::core::secure_erase(*bytes); })>{&bytes};
   auto stream = forge::datastream<std::uint8_t*>{bytes.data(), bytes.size()};
   forge::raw::pack(stream, std::string{"forge.secret-mutation.fingerprint.v1"}, std::uint8_t{1}, value.mutation_id,
                    value.ref, value.expected_revision);
   auto hmac = forge::crypto::digest::hmac_sha256{};
   return hmac.digest(catalog.receipt_key->span(), std::span<const std::uint8_t>{bytes.data(), stream.tellp()})
       .extract_as_byte_array();
}

version_mutation_result version_store::create_sync(create_version_request value) {
   const auto snapshot = current();
   const auto digest = fingerprint(value, *snapshot);
   if (const auto receipt = snapshot->receipts.find(value.mutation_id); receipt != snapshot->receipts.end()) {
      if (!forge::crypto::core::constant_time_equal(digest, receipt->second.fingerprint)) {
         FORGE_THROW_EXCEPTION(exceptions::idempotency_conflict, "Managed mutation id has different content");
      }
      return receipt->second.result;
   }
   const auto found = snapshot->heads.find(value.secret_id);
   const auto revision = found == snapshot->heads.end() ? 0 : found->second.revision;
   const auto latest = found == snapshot->heads.end() ? 0 : found->second.latest_version;
   if (revision != value.expected_revision) {
      FORGE_THROW_EXCEPTION(exceptions::revision_conflict, "Managed secret revision does not match");
   }
   auto policy = resolve_policy(std::move(value.policy));
   if (snapshot->versions.size() >= _settings.max_versions || latest >= _settings.max_versions_per_secret ||
       (found == snapshot->heads.end() && snapshot->heads.size() >= _settings.max_secret_ids) ||
       value.material.size() > _settings.max_total_material_bytes - snapshot->material_bytes ||
       latest == (std::numeric_limits<std::uint64_t>::max)() ||
       revision == (std::numeric_limits<std::uint64_t>::max)()) {
      FORGE_THROW_EXCEPTION(exceptions::capacity_exceeded, "Managed secret store capacity is exhausted");
   }
   auto candidate = std::make_shared<version_catalog>(*snapshot);
   auto result = version_mutation_result{
       .ref = {.secret_id = value.secret_id, .version = latest + 1},
       .revision = revision + 1,
   };
   candidate->material_bytes += value.material.size();
   auto resource = std::make_shared<loaded_secret>(loaded_secret{
       .id = value.secret_id,
       .kind = value.kind,
       .material = std::move(value.material),
       .purposes = std::move(policy.purposes),
       .operations = std::move(policy.operations),
       .allow_raw_export = policy.allow_raw_export,
       .max_plaintext_bytes = policy.max_plaintext_bytes,
       .max_ciphertext_bytes = policy.max_ciphertext_bytes,
       .max_aad_bytes = policy.max_aad_bytes,
   });
   candidate->heads[value.secret_id] = {.latest_version = latest + 1, .revision = revision + 1};
   candidate->versions.emplace(std::pair{value.secret_id, latest + 1},
                               version_catalog::entry{.ref = result.ref, .secret = std::move(resource)});
   candidate->receipts.emplace(std::move(value.mutation_id),
                               version_catalog::receipt{.kind = version_catalog::mutation_kind::create,
                                                        .fingerprint = digest,
                                                        .result = result});
   persist(std::move(candidate));
   return result;
}

version_mutation_result version_store::revoke_sync(revoke_version_request value) {
   const auto snapshot = current();
   const auto digest = fingerprint(value, *snapshot);
   if (const auto receipt = snapshot->receipts.find(value.mutation_id); receipt != snapshot->receipts.end()) {
      if (!forge::crypto::core::constant_time_equal(digest, receipt->second.fingerprint)) {
         FORGE_THROW_EXCEPTION(exceptions::idempotency_conflict, "Managed mutation id has different content");
      }
      return receipt->second.result;
   }
   const auto key = std::pair{value.ref.secret_id, value.ref.version};
   const auto found = snapshot->versions.find(key);
   if (found == snapshot->versions.end()) {
      FORGE_THROW_EXCEPTION(exceptions::version_not_found, "Managed secret version was not found");
   }
   const auto head = snapshot->heads.at(value.ref.secret_id);
   if (head.revision != value.expected_revision) {
      FORGE_THROW_EXCEPTION(exceptions::revision_conflict, "Managed secret revision does not match");
   }
   if (found->second.state == version_state::revoked) {
      FORGE_THROW_EXCEPTION(exceptions::version_revoked, "Managed secret version is revoked");
   }
   if (head.revision == (std::numeric_limits<std::uint64_t>::max)()) {
      FORGE_THROW_EXCEPTION(exceptions::capacity_exceeded, "Managed secret revision capacity is exhausted");
   }
   auto candidate = std::make_shared<version_catalog>(*snapshot);
   auto result = version_mutation_result{.ref = value.ref, .revision = head.revision + 1};
   candidate->versions.at(key).state = version_state::revoked;
   candidate->heads.at(value.ref.secret_id).revision = result.revision;
   candidate->receipts.emplace(std::move(value.mutation_id),
                               version_catalog::receipt{.kind = version_catalog::mutation_kind::revoke,
                                                        .fingerprint = digest,
                                                        .result = result});
   persist(std::move(candidate));
   return result;
}

std::shared_ptr<const loaded_secret> version_store::select(const version_ref& ref) {
   validate_ref(ref);
   const auto snapshot = current();
   const auto found = snapshot->versions.find({ref.secret_id, ref.version});
   if (found == snapshot->versions.end()) {
      FORGE_THROW_EXCEPTION(exceptions::version_not_found, "Managed secret version was not found");
   }
   if (found->second.state == version_state::revoked) {
      FORGE_THROW_EXCEPTION(exceptions::version_revoked, "Managed secret version is revoked");
   }
   return found->second.secret;
}

version_metadata version_store::describe_sync(version_query value) {
   validate_ref(value.ref);
   const auto snapshot = current();
   const auto found = snapshot->versions.find({value.ref.secret_id, value.ref.version});
   if (found == snapshot->versions.end()) {
      FORGE_THROW_EXCEPTION(exceptions::version_not_found, "Managed secret version was not found");
   }
   return {
       .ref = found->second.ref,
       .revision = snapshot->heads.at(value.ref.secret_id).revision,
       .kind = found->second.secret->kind,
       .state = found->second.state,
       .policy = policy_of(*found->second.secret),
   };
}

version_page version_store::list_sync(version_list_request value) {
   const auto snapshot = current();
   const auto head = snapshot->heads.find(value.secret_id);
   if (head == snapshot->heads.end()) {
      FORGE_THROW_EXCEPTION(exceptions::version_not_found, "Managed secret id was not found");
   }
   auto result = version_page{};
   result.versions.reserve(value.limit);
   auto found = snapshot->versions.upper_bound({value.secret_id, value.after_version});
   while (found != snapshot->versions.end() && found->first.first == value.secret_id &&
          result.versions.size() < value.limit) {
      result.versions.push_back({
          .ref = found->second.ref,
          .revision = head->second.revision,
          .kind = found->second.secret->kind,
          .state = found->second.state,
          .policy = policy_of(*found->second.secret),
      });
      ++found;
   }
   if (found != snapshot->versions.end() && found->first.first == value.secret_id) {
      result.next_after_version = result.versions.back().ref.version;
   }
   return result;
}

boost::asio::awaitable<version_mutation_result> version_store::create(create_version_request value) {
   return isolated(create_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_mutation_result> version_store::create_owned(std::shared_ptr<version_store> owner,
                                                                            create_version_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   auto admission = owner->reserve(owner->input_size(value));
   auto ticket = co_await owner->_write_gate.acquire();
   co_return co_await owner->blocking(
       [owner, value = std::move(value)]() mutable { return owner->create_sync(std::move(value)); });
}

boost::asio::awaitable<version_mutation_result> version_store::revoke(revoke_version_request value) {
   return isolated(revoke_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_mutation_result> version_store::revoke_owned(std::shared_ptr<version_store> owner,
                                                                            revoke_version_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref(value.ref);
   validate_id(value.mutation_id, 128);
   auto admission = owner->reserve(value.mutation_id.size() + value.ref.secret_id.size() + 64);
   auto ticket = co_await owner->_write_gate.acquire();
   co_return co_await owner->blocking(
       [owner, value = std::move(value)]() mutable { return owner->revoke_sync(std::move(value)); });
}

boost::asio::awaitable<version_metadata> version_store::describe(version_query value) {
   return isolated(describe_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_metadata> version_store::describe_owned(std::shared_ptr<version_store> owner,
                                                                       version_query value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref(value.ref);
   auto admission = owner->reserve(value.ref.secret_id.size() + 32);
   co_return co_await owner->blocking(
       [owner, value = std::move(value)]() mutable { return owner->describe_sync(std::move(value)); });
}

boost::asio::awaitable<version_page> version_store::list(version_list_request value) {
   return isolated(list_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_page> version_store::list_owned(std::shared_ptr<version_store> owner,
                                                               version_list_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_id(value.secret_id, 256);
   if (value.limit == 0 || value.limit > 128) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "Managed metadata page limit is invalid");
   }
   auto admission = owner->reserve(value.secret_id.size() + 32);
   co_return co_await owner->blocking(
       [owner, value = std::move(value)]() mutable { return owner->list_sync(std::move(value)); });
}

boost::asio::awaitable<version_get_result> version_store::get(version_get_request value) {
   return isolated(get_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_get_result> version_store::get_owned(std::shared_ptr<version_store> owner,
                                                                    version_get_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref({value.request.secret_id, value.version});
   validate_id(value.request.purpose, 128);
   auto admission = owner->reserve(value.request.secret_id.size() + value.request.purpose.size() + 32, true);
   co_return co_await owner->blocking([owner, value = std::move(value)]() mutable {
      auto secret = owner->select({value.request.secret_id, value.version});
      return version_get_result{.version = value.version,
                                .result = secret_operations::get_bytes(*secret, std::move(value.request))};
   });
}

boost::asio::awaitable<version_derive_result> version_store::derive(version_derive_request value) {
   return isolated(derive_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_derive_result> version_store::derive_owned(std::shared_ptr<version_store> owner,
                                                                          version_derive_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref({value.request.secret_id, value.version});
   validate_id(value.request.purpose, 128);
   if (value.request.salt.size() > owner->_settings.ceiling.max_aad_bytes ||
       value.request.info.size() > owner->_settings.ceiling.max_aad_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "Managed derivation exceeds its byte limit");
   }
   auto admission = owner->reserve(value.request.secret_id.size() + value.request.purpose.size() +
                                       value.request.salt.size() + value.request.info.size() + 64,
                                   true);
   co_return co_await owner->blocking([owner, value = std::move(value)]() mutable {
      return [&] {
         auto secret = owner->select({value.request.secret_id, value.version});
         if (value.request.salt.size() > secret->max_aad_bytes || value.request.info.size() > secret->max_aad_bytes ||
             value.request.output_size > secret->max_plaintext_bytes) {
            FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "Managed derivation exceeds its byte limit");
         }
         return version_derive_result{.version = value.version,
                                      .result =
                                          secret_operations::derive_hkdf_sha256(*secret, std::move(value.request))};
      }();
   });
}

boost::asio::awaitable<version_aead_encrypt_result> version_store::encrypt(version_aead_encrypt_request value) {
   return isolated(encrypt_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_aead_encrypt_result> version_store::encrypt_owned(std::shared_ptr<version_store> owner,
                                                                                 version_aead_encrypt_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref({value.request.secret_id, value.version});
   validate_id(value.request.purpose, 128);
   if (value.request.plaintext.size() > owner->_settings.ceiling.max_plaintext_bytes ||
       value.request.aad.size() > owner->_settings.ceiling.max_aad_bytes || value.request.nonce.size() > 12) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "Managed encryption exceeds its byte limit");
   }
   auto admission =
       owner->reserve(value.request.secret_id.size() + value.request.purpose.size() + value.request.nonce.size() +
                          value.request.plaintext.size() + value.request.aad.size() + 64,
                      true);
   co_return co_await owner->blocking([owner, value = std::move(value)]() mutable {
      auto secret = owner->select({value.request.secret_id, value.version});
      return version_aead_encrypt_result{
          .version = value.version, .result = secret_operations::encrypt_aes_gcm(*secret, std::move(value.request))};
   });
}

boost::asio::awaitable<version_aead_decrypt_result> version_store::decrypt(version_aead_decrypt_request value) {
   return isolated(decrypt_owned(shared_from_this(), std::move(value)));
}

boost::asio::awaitable<version_aead_decrypt_result> version_store::decrypt_owned(std::shared_ptr<version_store> owner,
                                                                                 version_aead_decrypt_request value) {
   owner->check_start();
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "Managed secret operation was canceled");
   }
   validate_ref({value.request.secret_id, value.version});
   validate_id(value.request.purpose, 128);
   if (value.request.ciphertext.size() > owner->_settings.ceiling.max_ciphertext_bytes ||
       value.request.aad.size() > owner->_settings.ceiling.max_aad_bytes || value.request.nonce.size() > 12 ||
       value.request.tag.size() > 16) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "Managed decryption exceeds its byte limit");
   }
   auto admission =
       owner->reserve(value.request.secret_id.size() + value.request.purpose.size() + value.request.nonce.size() +
                          value.request.tag.size() + value.request.ciphertext.size() + value.request.aad.size() + 64,
                      true);
   co_return co_await owner->blocking([owner, value = std::move(value)]() mutable {
      auto secret = owner->select({value.request.secret_id, value.version});
      return version_aead_decrypt_result{
          .version = value.version, .result = secret_operations::decrypt_aes_gcm(*secret, std::move(value.request))};
   });
}

} // namespace forge::plugins::crypto::secrets
