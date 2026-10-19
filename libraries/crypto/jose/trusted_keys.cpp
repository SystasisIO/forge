module;

#include <algorithm>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.crypto.jose.trusted_keys;

import forge.codec.hex;
import forge.crypto.digest.sha256;

namespace forge::crypto::jose {
namespace {

bool valid_identifier(std::string_view value) {
   return !value.empty() && value.size() <= 256 && std::ranges::all_of(value, [](unsigned char byte) {
      return byte >= 0x21 && byte <= 0x7e;
   });
}

std::string fingerprint(const std::vector<std::string>& fields) {
   // Canonical Raw framing makes field boundaries unambiguous.
   const auto hash = digest::sha256::hash(fields);
   return forge::codec::hex::encode(hash.to_uint8_span());
}

} // namespace

struct trusted_keys::impl {
   snapshot_descriptor descriptor;
   header_policy policy;
   std::vector<trusted_key> keys;
   std::vector<verified_key_binding> bindings;
   std::string identity;
};

verified_key_binding identify_key(const trusted_key& key) {
   if (key.kid.size() > 128 || !std::ranges::all_of(key.kid, [](unsigned char byte) {
          return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                 (byte >= '0' && byte <= '9') || byte == '.' || byte == '_' || byte == '-';
       }) || static_cast<unsigned char>(key.public_key[0]) != 4) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "invalid JOSE public key binding");
   }
   auto normalized = asymmetric::p256::public_key_data{};
   try {
      const auto point = asymmetric::p256::public_key{key.public_key};
      if (!point.valid()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_key, "invalid JOSE public key");
      }
      normalized = point.serialize();
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "invalid JOSE public key");
   }
   const auto bytes = std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(normalized.data()),
                                                  normalized.size()};
   return {.kid = key.kid,
           .public_key = asymmetric::r1_public_key{normalized},
           .identity = fingerprint({"forge.crypto.jose.key.v1", "P-256", forge::codec::hex::encode(bytes)})};
}

trusted_keys::trusted_keys(snapshot_descriptor descriptor, std::vector<trusted_key> keys, header_policy policy) {
   policy.validate();
   if (!valid_identifier(descriptor.trust_domain) || !valid_identifier(descriptor.verifier_profile) ||
       keys.empty() || keys.size() > 128 ||
       (policy.key_id == key_id_policy::absent && (keys.size() != 1 || !keys.front().kid.empty()))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_policy, "invalid JOSE trust snapshot");
   }
   std::ranges::sort(keys, {}, &trusted_key::kid);
   auto state = std::make_shared<impl>();
   state->descriptor = std::move(descriptor);
   state->policy = std::move(policy);
   state->keys = std::move(keys);
   auto fields = std::vector<std::string>{
       "forge.crypto.jose.snapshot.v1", state->descriptor.trust_domain, state->descriptor.verifier_profile,
       std::to_string(state->descriptor.revision), state->policy.typ ? "typ-present" : "typ-absent",
       state->policy.typ.value_or(""), state->policy.key_id == key_id_policy::required ? "kid-required" : "kid-absent",
       std::to_string(state->policy.bounds.compact_bytes), std::to_string(state->policy.bounds.protected_header_bytes),
       std::to_string(state->policy.bounds.payload_bytes), "ES256", "exact-header-v1"};
   for (const auto& key : state->keys) {
      if ((state->policy.key_id == key_id_policy::required && key.kid.empty()) ||
          (!state->bindings.empty() && state->bindings.back().kid == key.kid)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_key, "ambiguous JOSE key identifier");
      }
      auto binding = identify_key(key);
      fields.push_back(binding.kid);
      fields.push_back(binding.identity);
      state->bindings.push_back(std::move(binding));
   }
   state->identity = fingerprint(fields);
   _impl = std::move(state);
}

trusted_keys::~trusted_keys() = default;

const snapshot_descriptor& trusted_keys::descriptor() const& noexcept { return _impl->descriptor; }
const header_policy& trusted_keys::policy() const& noexcept { return _impl->policy; }
const std::string& trusted_keys::identity() const& noexcept { return _impl->identity; }
std::span<const trusted_key> trusted_keys::keys() const& noexcept { return _impl->keys; }

const verified_key_binding& trusted_keys::key_binding(std::size_t index) const& {
   if (index >= _impl->bindings.size()) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_key, "JOSE key is not in the trust snapshot");
   }
   return _impl->bindings[index];
}

std::optional<std::size_t> trusted_keys::find(std::string_view kid) const noexcept {
   const auto entry = std::ranges::lower_bound(_impl->keys, kid, {}, &trusted_key::kid);
   if (entry == _impl->keys.end() || entry->kid != kid) {
      return std::nullopt;
   }
   return static_cast<std::size_t>(entry - _impl->keys.begin());
}

} // namespace forge::crypto::jose
