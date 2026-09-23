module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.chain.signer.plugin;

import forge.api.auth.authenticated_caller;
import forge.chain.api.exceptions;
import forge.chain.api.block_signer;
import forge.chain.protocol.block_signing;
import forge.chain.protocol.transaction;
import forge.chain.protocol.values;
import forge.chain.transaction.types;
import forge.crypto.asymmetric;
import forge.crypto.bls.serialization;
import forge.plugins.chain.signer.exceptions;
import forge.plugins.chain.signer.types;
import forge.raw.raw;

#include "details/signing_policy.hxx"

namespace forge::plugins::chain::signer {
namespace {

[[nodiscard]] bool is_lower_hex(std::string_view value) {
   return value.size() == forge::crypto::digest::sha256::byte_size * 2U && std::ranges::all_of(value, [](char current) {
             return (current >= '0' && current <= '9') || (current >= 'a' && current <= 'f');
          });
}

[[nodiscard]] forge::crypto::digest::sha256 parse_digest(std::string_view value, std::string_view field) {
   if (!is_lower_hex(value)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer digest must be canonical lowercase hexadecimal",
                            forge::exceptions::ctx("field", field));
   }

   try {
      auto result = forge::crypto::digest::sha256{std::string{value}};
      if (result.str() != value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer digest is not canonical",
                               forge::exceptions::ctx("field", field));
      }
      return result;
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer digest is invalid",
                            forge::exceptions::ctx("field", field));
   }
}

[[nodiscard]] forge::chain::protocol::name parse_name(std::string_view value, std::string_view field) {
   try {
      const auto result = forge::chain::protocol::name{value};
      if (!result || result.to_string() != value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer name is not canonical",
                               forge::exceptions::ctx("field", field), forge::exceptions::ctx("value", value));
      }
      return result;
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer name is invalid",
                            forge::exceptions::ctx("field", field), forge::exceptions::ctx("value", value));
   }
}

[[nodiscard]] forge::crypto::asymmetric::public_key parse_signing_public_key(std::string_view value) {
   try {
      auto result = forge::crypto::asymmetric::encoding::forge().parse_public(value);
      if (forge::crypto::asymmetric::encoding::forge().format(result) != value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer public key is not canonical");
      }
      if (forge::crypto::asymmetric::type(result) != forge::crypto::asymmetric::algorithm::secp256k1) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer public key must use K1");
      }
      return result;
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer public key is invalid");
   }
}

[[nodiscard]] forge::crypto::bls::public_key parse_finality_public_key(std::string_view value) {
   try {
      auto result = forge::crypto::bls::encoding::parse_public_key(value);
      if (forge::crypto::bls::encoding::format(result) != value) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer finality public key is not canonical");
      }
      return result;
   } catch (const exceptions::invalid_config&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer finality public key is invalid");
   }
}

template <typename Provider>
[[nodiscard]] std::shared_ptr<Provider> find_provider(const auto& providers, std::string_view name,
                                                      std::string_view kind) {
   const auto found = std::ranges::find_if(providers, [name](const auto& value) { return value.name == name; });
   if (found == providers.end() || found->value == nullptr) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer binding references an unavailable provider",
                            forge::exceptions::ctx("kind", kind), forge::exceptions::ctx("provider", name));
   }
   return found->value;
}

template <typename Provider> void validate_providers(const std::vector<Provider>& providers, std::string_view kind) {
   for (auto index = std::size_t{}; index < providers.size(); ++index) {
      const auto& current = providers[index];
      if (current.name.empty() || current.value == nullptr) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer provider is incomplete",
                               forge::exceptions::ctx("kind", kind));
      }

      for (auto previous = std::size_t{}; previous < index; ++previous) {
         if (providers[previous].name == current.name) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer provider name is duplicated",
                                  forge::exceptions::ctx("kind", kind),
                                  forge::exceptions::ctx("provider", current.name));
         }
      }
   }
}

} // namespace

struct signing_policy::compiled_caller {
   forge::api::auth::caller_source source = forge::api::auth::caller_source::p2p_peer;
   forge::crypto::digest::sha256 fingerprint;
};

struct signing_policy::compiled_action {
   forge::chain::protocol::account_name account;
   forge::chain::protocol::action_name action;
   forge::chain::protocol::permission_level authorization;
};

struct signing_policy::compiled_context_free_action {
   forge::chain::protocol::account_name account;
   forge::chain::protocol::action_name action;
};

struct signing_policy::compiled_profile {
   std::string name;
   forge::chain::protocol::chain_id chain;
   std::shared_ptr<forge::crypto::signer::provider> provider;
   std::shared_ptr<semantic_authorization_provider> semantic_authorization;
   forge::chain::transaction::signing_key key;
   bool allow_local = false;
   std::vector<compiled_caller> callers;
   std::vector<compiled_action> actions;
   std::vector<compiled_context_free_action> context_free_actions;
   std::uint32_t max_expiration_seconds = 0;
   std::uint32_t max_delay_seconds = 0;
   std::uint32_t max_actions = 0;
   std::uint64_t max_packed_bytes = 0;
   bool allow_context_free_data = false;
   bool allow_context_free_actions = false;
   bool allow_extensions = false;
};

struct signing_policy::compiled_block_profile {
   std::string name;
   forge::chain::protocol::chain_id chain;
   forge::chain::protocol::account_name producer;
   std::vector<block_key_selection> keys;
   bool allow_local = false;
   std::vector<compiled_caller> callers;
   std::uint64_t max_header_bytes = 0;
};

signing_policy::signing_policy(const plugin_options& options, const config& settings) {
   validate_providers(options.providers, "K1");
   validate_providers(options.finality_providers, "finality");

   profiles_.reserve(settings.transaction_profiles.size());
   for (const auto& source : settings.transaction_profiles) {
      if (source.name.empty() || source.signing.key_id.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer transaction profile is incomplete");
      }
      if (std::ranges::any_of(profiles_, [&source](const auto& value) { return value.name == source.name; })) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer transaction profile name is duplicated",
                               forge::exceptions::ctx("profile", source.name));
      }

      const auto remote_enabled = !source.callers.empty();
      if (source.authorization != remote_authorization::semantic &&
          source.authorization != remote_authorization::profile_only) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "Chain signer remote authorization mode is invalid",
                               forge::exceptions::ctx("profile", source.name));
      }
      if (remote_enabled && source.authorization == remote_authorization::semantic &&
          options.semantic_authorization == nullptr) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                               "Chain signer remote profile requires a semantic authorization provider",
                               forge::exceptions::ctx("profile", source.name));
      }

      auto profile = compiled_profile{
          .name = source.name,
          .chain = parse_digest(source.chain_id, "chain-id"),
          .provider = find_provider<forge::crypto::signer::provider>(options.providers, source.signing.provider, "K1"),
          .semantic_authorization = remote_enabled && source.authorization == remote_authorization::semantic
                                        ? options.semantic_authorization
                                        : nullptr,
          .key =
              {
                  .id = {.value = source.signing.key_id},
                  .public_key = parse_signing_public_key(source.signing.expected_public_key),
              },
          .allow_local = source.allow_local,
          .max_expiration_seconds = source.max_expiration_seconds,
          .max_delay_seconds = source.max_delay_seconds,
          .max_actions = source.max_actions,
          .max_packed_bytes = source.max_packed_bytes,
          .allow_context_free_data = source.allow_context_free_data,
          .allow_context_free_actions = source.allow_context_free_actions,
          .allow_extensions = source.allow_extensions,
      };
      if (profile.max_expiration_seconds == 0U || profile.max_actions == 0U || profile.max_packed_bytes == 0U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer transaction profile limits must be positive",
                               forge::exceptions::ctx("profile", source.name));
      }

      profile.callers.reserve(source.callers.size());
      for (const auto& caller : source.callers) {
         auto value = compiled_caller{
             .source = caller.source,
             .fingerprint = parse_digest(caller.fingerprint, "caller.fingerprint"),
         };
         if (std::ranges::any_of(profile.callers, [&value](const auto& current) {
                return current.source == value.source && current.fingerprint == value.fingerprint;
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer caller rule is duplicated",
                                  forge::exceptions::ctx("profile", source.name));
         }
         profile.callers.push_back(std::move(value));
      }

      profile.actions.reserve(source.actions.size());
      for (const auto& action : source.actions) {
         auto value = compiled_action{
             .account = parse_name(action.account, "action.account"),
             .action = parse_name(action.action, "action.action"),
             .authorization =
                 {
                     .actor = parse_name(action.actor, "action.actor"),
                     .permission = parse_name(action.permission, "action.permission"),
                 },
         };
         if (std::ranges::any_of(profile.actions, [&value](const auto& current) {
                return current.account == value.account && current.action == value.action &&
                       current.authorization == value.authorization;
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer action rule is duplicated",
                                  forge::exceptions::ctx("profile", source.name));
         }
         profile.actions.push_back(std::move(value));
      }
      if (profile.actions.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer transaction profile requires an action rule",
                               forge::exceptions::ctx("profile", source.name));
      }

      profile.context_free_actions.reserve(source.context_free_actions.size());
      for (const auto& action : source.context_free_actions) {
         auto value = compiled_context_free_action{
             .account = parse_name(action.account, "context-free-action.account"),
             .action = parse_name(action.action, "context-free-action.action"),
         };
         if (std::ranges::any_of(profile.context_free_actions, [&value](const auto& current) {
                return current.account == value.account && current.action == value.action;
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer context-free action rule is duplicated",
                                  forge::exceptions::ctx("profile", source.name));
         }
         profile.context_free_actions.push_back(std::move(value));
      }
      profiles_.push_back(std::move(profile));
   }

   block_profiles_.reserve(settings.block_profiles.size());
   for (const auto& source : settings.block_profiles) {
      if (source.name.empty() || source.signing.empty() || source.signing.size() > 64U ||
          source.max_header_bytes == 0U || source.max_header_bytes > 64U * 1024U * 1024U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer block profile is incomplete or exceeds limits");
      }
      auto profile = compiled_block_profile{
          .name = source.name,
          .chain = parse_digest(source.chain_id, "chain-id"),
          .producer = parse_name(source.producer, "producer"),
          .allow_local = source.allow_local,
          .max_header_bytes = source.max_header_bytes,
      };
      for (const auto& existing : block_profiles_) {
         if (existing.name == profile.name || (existing.chain == profile.chain && existing.producer == profile.producer)) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer block profile is duplicated or ambiguous",
                                  forge::exceptions::ctx("profile", source.name));
         }
      }
      profile.keys.reserve(source.signing.size());
      for (const auto& binding : source.signing) {
         if (binding.key_id.empty()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer block key id is empty");
         }
         auto key = block_key_selection{
             .provider = find_provider<forge::crypto::signer::provider>(options.providers, binding.provider, "K1"),
             .key = {.id = {.value = binding.key_id},
                     .public_key = parse_signing_public_key(binding.expected_public_key)},
         };
         if (std::ranges::any_of(profile.keys, [&key](const auto& prior) {
                return prior.key.public_key == key.key.public_key ||
                       (prior.provider == key.provider && prior.key.id == key.key.id);
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer block key binding is duplicated");
         }
         for (const auto& prior_profile : block_profiles_) {
            if (prior_profile.chain != profile.chain &&
                std::ranges::any_of(prior_profile.keys, [&key](const auto& prior) {
                   return prior.key.public_key == key.key.public_key;
                })) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                                     "Chain signer producer key must not be shared across chains");
            }
         }
         profile.keys.push_back(std::move(key));
      }
      for (const auto& caller : source.callers) {
         auto allowed = compiled_caller{.source = caller.source,
                                        .fingerprint = parse_digest(caller.fingerprint, "caller.fingerprint")};
         if (std::ranges::any_of(profile.callers, [&allowed](const auto& prior) {
                return prior.source == allowed.source && prior.fingerprint == allowed.fingerprint;
             })) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Chain signer block caller is duplicated");
         }
         profile.callers.push_back(std::move(allowed));
      }
      block_profiles_.push_back(std::move(profile));
   }

   if (settings.finality) {
      finality_provider_ = find_provider<forge::crypto::bls::signer::provider>(options.finality_providers,
                                                                               settings.finality->provider, "finality");
      finality_key_ = parse_finality_public_key(settings.finality->expected_public_key);
   }
}

signing_policy::~signing_policy() = default;

bool signing_policy::matches(const compiled_profile& profile,
                             const forge::chain::transaction::unsigned_transaction& transaction,
                             const forge::api::auth::authenticated_caller& caller,
                             forge::chain::protocol::time_point_sec now) const {
   if (profile.chain != transaction.chain) {
      return false;
   }

   const auto local = !caller.transport_authenticated();
   if (local) {
      if (!profile.allow_local) {
         return false;
      }
   } else if (std::ranges::none_of(profile.callers, [&caller](const auto& allowed) {
                 return allowed.source == caller.source && allowed.fingerprint == caller.fingerprint;
              })) {
      return false;
   }

   const auto expiration = transaction.value.expiration.sec_since_epoch();
   const auto latest = static_cast<std::uint64_t>(now.sec_since_epoch()) + profile.max_expiration_seconds;
   if (expiration <= now.sec_since_epoch() || expiration > latest) {
      return false;
   }
   if (transaction.value.delay_sec.value > profile.max_delay_seconds) {
      return false;
   }

   const auto action_count = transaction.value.actions.size() + transaction.value.context_free_actions.size();
   if (action_count == 0U || action_count > profile.max_actions) {
      return false;
   }
   if ((!profile.allow_context_free_data && !transaction.context_free_data.empty()) ||
       (!profile.allow_context_free_actions && !transaction.value.context_free_actions.empty()) ||
       (!profile.allow_extensions && !transaction.value.transaction_extensions.empty())) {
      return false;
   }

   for (const auto& action : transaction.value.actions) {
      if (action.authorization.empty()) {
         return false;
      }

      for (const auto& authorization : action.authorization) {
         if (std::ranges::none_of(profile.actions, [&action, &authorization](const auto& allowed) {
                return allowed.account == action.account && allowed.action == action.name &&
                       allowed.authorization == authorization;
             })) {
            return false;
         }
      }
   }

   for (const auto& action : transaction.value.context_free_actions) {
      if (!action.authorization.empty() ||
          std::ranges::none_of(profile.context_free_actions, [&action](const auto& allowed) {
             return allowed.account == action.account && allowed.action == action.name;
          })) {
         return false;
      }
   }
   return true;
}

signing_policy::transaction_selection
signing_policy::select_transaction(const forge::chain::transaction::unsigned_transaction& transaction,
                                   const forge::api::auth::authenticated_caller& caller,
                                   forge::chain::protocol::time_point_sec now) const {
   const auto* selected = static_cast<const compiled_profile*>(nullptr);
   for (const auto& profile : profiles_) {
      if (!matches(profile, transaction, caller, now)) {
         continue;
      }
      if (selected != nullptr) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer transaction matches multiple profiles");
      }
      selected = &profile;
   }
   if (selected == nullptr) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                            "Chain signer transaction does not match an exact profile");
   }
   return transaction_selection{
       .profile = selected->name,
       .provider = selected->provider,
       .semantic_authorization = selected->semantic_authorization,
       .key = selected->key,
       .max_packed_bytes = selected->max_packed_bytes,
   };
}

signing_policy::block_selection
signing_policy::select_block(const forge::chain::protocol::block_sign_request& request,
                             const forge::api::auth::authenticated_caller& caller) const {
   if (request.keys.empty() || request.keys.size() > 64U) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                            "Chain signer block key count must be between 1 and 64");
   }
   for (auto index = std::size_t{}; index < request.keys.size(); ++index) {
      if (forge::crypto::asymmetric::type(request.keys[index]) != forge::crypto::asymmetric::algorithm::secp256k1 ||
          std::ranges::find(request.keys.begin(), request.keys.begin() + static_cast<std::ptrdiff_t>(index),
                            request.keys[index]) != request.keys.begin() + static_cast<std::ptrdiff_t>(index)) {
         FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                               "Chain signer block keys must be unique K1 keys");
      }
   }

   for (const auto& profile : block_profiles_) {
      if (profile.chain != request.chain || profile.producer != request.header.producer) {
         continue;
      }
      if (!caller.transport_authenticated()) {
         if (!profile.allow_local) {
            break;
         }
      } else if (std::ranges::none_of(profile.callers, [&caller](const auto& allowed) {
                    return allowed.source == caller.source && allowed.fingerprint == caller.fingerprint;
                 })) {
         break;
      }
      auto result = block_selection{.profile = profile.name,
                                    .producer = profile.producer,
                                    .max_header_bytes = profile.max_header_bytes};
      result.keys.reserve(request.keys.size());
      for (const auto& requested : request.keys) {
         const auto found = std::ranges::find_if(profile.keys, [&requested](const auto& configured) {
            return configured.key.public_key == requested;
         });
         if (found == profile.keys.end()) {
            FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                                  "Chain signer block key is not in the selected profile");
         }
         result.keys.push_back(*found);
      }
      return result;
   }
   FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::authorization_denied,
                         "Chain signer block does not match an exact profile");
}

signing_policy::finality_selection signing_policy::select_finality() const {
   if (finality_provider_ == nullptr) {
      FORGE_THROW_EXCEPTION(forge::chain::api::exceptions::unavailable,
                            "Chain signer finality provider is not configured");
   }
   return finality_selection{
       .provider = finality_provider_,
       .expected_key = finality_key_,
   };
}

} // namespace forge::plugins::chain::signer
