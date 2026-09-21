module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.dht;

import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.chrono.timestamp;
import forge.net.p2p.ipns;

namespace forge::net::p2p {
namespace {

constexpr auto public_key_prefix = std::string_view{"/pk/"};
constexpr auto ipns_prefix = std::string_view{"/ipns/"};
constexpr auto amino_message_limit = std::size_t{16 * 1024};
// Amino does not carry this local resource bound on the wire; Forge fixes it per profile for deterministic admission.
constexpr auto amino_max_query_peers = std::size_t{256};
constexpr auto amino_max_closer_peers = std::size_t{21};
constexpr auto amino_max_provider_peers = std::size_t{256};
constexpr auto amino_max_peer_endpoints = std::size_t{64};
constexpr auto max_wire_lifetime = std::chrono::seconds{(std::numeric_limits<std::uint32_t>::max)()};

[[noreturn]] void throw_invalid_profile(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_record_rejected(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::record_rejected, std::move(message));
}

[[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] std::span<const std::uint8_t> suffix(const dht::key& key, std::span<const std::uint8_t> prefix) {
   if (key.bytes.size() <= prefix.size() || !std::ranges::equal(prefix, std::span{key.bytes}.first(prefix.size()))) {
      throw_invalid_profile("DHT record key does not match its value policy namespace");
   }
   return std::span{key.bytes}.subspan(prefix.size());
}

void validate_public_key_record(const dht::record& value, dht::value_validation_context) {
   try {
      const auto peer = peer_id::from_bytes(suffix(value.key_value, bytes(public_key_prefix)));
      const auto key = decode_public_key(value.value);
      validate_public_key(key, peer);
   } catch (const forge::exceptions::base& error) {
      throw_record_rejected(std::string{"invalid /pk record: "} + error.what());
   }
}

[[nodiscard]] std::size_t select_public_key_record(std::span<const dht::record> candidates) {
   if (candidates.empty()) {
      throw_invalid_profile("/pk selector requires at least one record");
   }
   return 0;
}

[[nodiscard]] std::chrono::system_clock::time_point public_key_expiry(const dht::record&,
                                                                      dht::value_expiry_context context) {
   return context.supplied_expires_at;
}

void validate_ipns_record(const dht::record& value, dht::value_validation_context context) {
   const auto peer = [&]() {
      try {
         return peer_id::from_bytes(suffix(value.key_value, bytes(ipns_prefix)));
      } catch (const forge::exceptions::base& error) {
         throw_record_rejected(std::string{"invalid /ipns record: "} + error.what());
      }
   }();
   const auto decoded = [&]() {
      try {
         return ipns::decode(value.value);
      } catch (const forge::exceptions::base& error) {
         throw_record_rejected(std::string{"invalid /ipns record: "} + error.what());
      }
   }();
   const auto now = std::chrono::time_point_cast<std::chrono::nanoseconds>(context.now);
   const auto deterministic_validation_failure = [](const forge::exceptions::base& error) {
      const auto code = exceptions::code_of(error);
      return code == exceptions::code::codec_error || code == exceptions::code::protocol_error ||
             code == exceptions::code::invalid_identity;
   };
   if (context.public_keys) {
      auto resolver_failure = std::exception_ptr{};
      const auto resolver = public_key_resolver{[&](const peer_id& requested) -> std::optional<public_key> {
         try {
            return (*context.public_keys)(requested);
         } catch (...) {
            resolver_failure = std::current_exception();
            return std::nullopt;
         }
      }};
      try {
         ipns::validate(decoded, peer, resolver, now);
         return;
      } catch (const forge::exceptions::base& error) {
         if (resolver_failure) {
            std::rethrow_exception(resolver_failure);
         }
         if (deterministic_validation_failure(error)) {
            throw_record_rejected(std::string{"invalid /ipns record: "} + error.what());
         }
         throw;
      }
   }
   try {
      ipns::validate(decoded, peer, std::nullopt, now);
   } catch (const forge::exceptions::base& error) {
      if (deterministic_validation_failure(error)) {
         throw_record_rejected(std::string{"invalid /ipns record: "} + error.what());
      }
      throw;
   }
}

[[nodiscard]] std::size_t select_ipns_record(std::span<const dht::record> candidates) {
   if (candidates.empty()) {
      throw_invalid_profile("/ipns selector requires at least one record");
   }
   auto decoded = std::vector<ipns::record>{};
   decoded.reserve(candidates.size());
   for (const auto& candidate : candidates) {
      decoded.push_back(ipns::decode(candidate.value));
   }
   return ipns::select(decoded);
}

[[nodiscard]] std::chrono::system_clock::time_point ipns_expiry(const dht::record& value,
                                                                dht::value_expiry_context context) {
   const auto signed_eol = ipns::decode(value.value).eol();
   const auto supplied =
       forge::chrono::timestamp{std::chrono::time_point_cast<std::chrono::nanoseconds>(context.supplied_expires_at)};
   if (signed_eol >= supplied) {
      return context.supplied_expires_at;
   }
   return std::chrono::time_point_cast<std::chrono::system_clock::duration>(signed_eol.whole_seconds()) +
          std::chrono::duration_cast<std::chrono::system_clock::duration>(signed_eol.subsecond());
}

[[nodiscard]] bool prefix_matches(std::span<const std::uint8_t> key, std::span<const std::uint8_t> prefix) noexcept {
   return key.size() > prefix.size() && std::ranges::equal(key.first(prefix.size()), prefix);
}

} // namespace

dht::profile amino_v1(dht::mode operating_mode) {
   auto limits = dht::options{};
   limits.operating_mode = operating_mode;
   limits.replication = 20;
   limits.alpha = 10;
   limits.max_outbound_message_size = amino_message_limit;
   limits.max_inbound_message_size = 4 * 1024 * 1024;
   limits.max_record_size = ipns::max_record_size;
   limits.max_closer_peers = amino_max_closer_peers;
   limits.max_provider_peers = amino_max_provider_peers;
   limits.max_peer_endpoints = amino_max_peer_endpoints;
   limits.max_query_peers = amino_max_query_peers;
   limits.replacement_cache_size = 20;
   limits.value_record_ttl = std::chrono::hours{48};
   limits.provider_record_ttl = std::chrono::hours{48};
   limits.provider_address_ttl = std::chrono::hours{24};
   limits.provider_republish_interval = std::chrono::hours{22};

   auto result = dht::profile{
       .protocol = builtins::kad_dht,
       .kind = dht::profile_kind::amino_v1,
       .operating_mode = operating_mode,
       .capabilities = {.peers = true, .providers = true, .values = true},
       .limits = limits,
       .value_policies =
           {
               dht::value_policy{.key_prefix = bytes(public_key_prefix),
                                 .validate = validate_public_key_record,
                                 .select = select_public_key_record,
                                 .expiry = public_key_expiry},
               dht::value_policy{.key_prefix = bytes(ipns_prefix),
                                 .validate = validate_ipns_record,
                                 .select = select_ipns_record,
                                 .expiry = ipns_expiry},
           },
   };
   validate(result);
   return result;
}

dht::profile custom_dht_profile(protocol_id protocol, dht::mode operating_mode, dht::profile_capabilities capabilities,
                                std::vector<dht::value_policy> value_policies, dht::options limits) {
   limits.operating_mode = operating_mode;
   auto result = dht::profile{
       .protocol = std::move(protocol),
       .kind = dht::profile_kind::custom,
       .operating_mode = operating_mode,
       .capabilities = capabilities,
       .limits = limits,
       .value_policies = std::move(value_policies),
   };
   validate(result);
   return result;
}

void validate(const dht::profile& profile) {
   if (profile.protocol.value.empty() || profile.protocol.value.front() != '/') {
      throw_invalid_profile("DHT profile protocol ID must be an absolute multistream protocol");
   }
   if (profile.kind == dht::profile_kind::custom && profile.protocol == builtins::kad_dht) {
      throw_invalid_profile("custom DHT profile cannot advertise the Amino protocol ID");
   }
   if (profile.kind == dht::profile_kind::amino_v1) {
      const auto public_key_namespace = bytes(public_key_prefix);
      const auto ipns_namespace = bytes(ipns_prefix);
      if (profile.protocol != builtins::kad_dht || !profile.capabilities.peers || !profile.capabilities.providers ||
          !profile.capabilities.values || profile.limits.operating_mode != profile.operating_mode ||
          profile.limits.replication != 20 || profile.limits.alpha != 10 ||
          profile.limits.max_outbound_message_size != amino_message_limit ||
          profile.limits.max_inbound_message_size != 4 * 1024 * 1024 ||
          profile.limits.max_record_size != ipns::max_record_size ||
          profile.limits.max_closer_peers != amino_max_closer_peers ||
          profile.limits.max_provider_peers != amino_max_provider_peers ||
          profile.limits.max_peer_endpoints != amino_max_peer_endpoints ||
          profile.limits.max_query_peers != amino_max_query_peers || profile.limits.replacement_cache_size != 20 ||
          profile.limits.failure_threshold != 3 || profile.limits.query_timeout != std::chrono::seconds{10} ||
          profile.limits.refresh_interval != std::chrono::minutes{10} ||
          profile.limits.value_record_ttl != std::chrono::hours{48} ||
          profile.limits.provider_record_ttl != std::chrono::hours{48} ||
          profile.limits.provider_address_ttl != std::chrono::hours{24} ||
          profile.limits.provider_republish_interval != std::chrono::hours{22} || profile.value_policies.size() != 2 ||
          profile.value_policies[0].key_prefix != public_key_namespace ||
          profile.value_policies[1].key_prefix != ipns_namespace) {
         throw_invalid_profile("Amino DHT profile parameters are fixed by /ipfs/kad/1.0.0");
      }
   }
   if (!profile.capabilities.peers && !profile.capabilities.providers && !profile.capabilities.values) {
      throw_invalid_profile("DHT profile must enable at least one operation family");
   }
   const auto invalid_provider_lifetime =
       profile.capabilities.providers &&
       (profile.limits.provider_record_ttl <= std::chrono::seconds::zero() ||
        profile.limits.provider_address_ttl <= std::chrono::seconds::zero() ||
        profile.limits.provider_republish_interval <= std::chrono::seconds::zero() ||
        profile.limits.provider_record_ttl > max_wire_lifetime ||
        profile.limits.provider_address_ttl > max_wire_lifetime ||
        profile.limits.provider_republish_interval >= profile.limits.provider_record_ttl ||
        profile.limits.provider_republish_interval >= profile.limits.provider_address_ttl ||
        profile.limits.query_timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(std::min(
                                            profile.limits.provider_record_ttl, profile.limits.provider_address_ttl)));
   const auto invalid_value_lifetime =
       profile.capabilities.values && (profile.limits.value_record_ttl <= std::chrono::seconds::zero() ||
                                       profile.limits.value_record_ttl > max_wire_lifetime);
   if (profile.limits.replication == 0 || profile.limits.alpha == 0 ||
       profile.limits.alpha > profile.limits.replication || profile.limits.max_outbound_message_size == 0 ||
       profile.limits.max_inbound_message_size == 0 || profile.limits.max_record_size == 0 ||
       profile.limits.max_closer_peers == 0 || profile.limits.max_provider_peers == 0 ||
       profile.limits.max_peer_endpoints == 0 || profile.limits.max_query_peers == 0 ||
       profile.limits.replacement_cache_size == 0 || profile.limits.failure_threshold == 0 ||
       profile.limits.query_timeout <= std::chrono::milliseconds::zero() ||
       profile.limits.refresh_interval <= std::chrono::milliseconds::zero() || invalid_value_lifetime ||
       invalid_provider_lifetime) {
      throw_invalid_profile("DHT profile limits must be positive and alpha must not exceed k");
   }
   if (profile.capabilities.values != !profile.value_policies.empty()) {
      throw_invalid_profile("DHT value capability and validator/selector policies must be configured together");
   }
   auto prefixes = std::vector<std::vector<std::uint8_t>>{};
   prefixes.reserve(profile.value_policies.size());
   for (const auto& policy : profile.value_policies) {
      if (policy.key_prefix.empty() || !policy.validate || !policy.select || !policy.expiry) {
         throw_invalid_profile("DHT value policy requires a prefix, validator, selector, and expiry policy");
      }
      const auto overlaps = std::ranges::any_of(prefixes, [&](const auto& existing) {
         const auto common = std::min(existing.size(), policy.key_prefix.size());
         return std::ranges::equal(std::span{existing}.first(common), std::span{policy.key_prefix}.first(common));
      });
      if (overlaps) {
         throw_invalid_profile("DHT value policy prefixes must not overlap");
      }
      prefixes.push_back(policy.key_prefix);
   }
}

const dht::value_policy* value_policy_for(const dht::profile& profile, std::span<const std::uint8_t> key) noexcept {
   const auto match = std::ranges::find_if(profile.value_policies,
                                           [&](const auto& policy) { return prefix_matches(key, policy.key_prefix); });
   return match == profile.value_policies.end() ? nullptr : &*match;
}

} // namespace forge::net::p2p
