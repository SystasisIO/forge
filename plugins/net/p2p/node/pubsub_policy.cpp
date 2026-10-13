module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.multiformats.varint;
import forge.net.p2p.exceptions;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.schema.exceptions;
import forge.schema.scalar;

#include "details/pubsub_policy.hxx"

namespace forge::plugins::net::p2p::node::detail {
namespace {

void validate_topic_list(const std::vector<std::string>& values, std::string_view name) {
   auto seen = std::set<std::string>{};
   for (const auto& value : values) {
      if (value.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub topic policy contains empty topic",
                             forge::exceptions::ctx("list", std::string{name}));
      }
      if (!seen.insert(value).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config,
                             "P2P PubSub topic policy contains duplicate topic",
                             forge::exceptions::ctx("list", std::string{name}), forge::exceptions::ctx("topic", value));
      }
   }
}

std::size_t checked_sum(std::size_t left, std::size_t right) {
   if (right > (std::numeric_limits<std::size_t>::max)() - left) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub message framing budget overflows");
   }
   return left + right;
}

} // namespace

std::chrono::milliseconds to_ms(std::uint64_t value) {
   try {
      return std::chrono::milliseconds{forge::schema::checked_integral_cast<std::chrono::milliseconds::rep>(value)};
   } catch (const forge::schema::exceptions::invalid_value& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
}

void patch_size(std::size_t& target, const std::optional<std::uint64_t>& value) {
   if (!value) { return; }
   try {
      target = forge::schema::checked_integral_cast<std::size_t>(*value);
   } catch (const forge::schema::exceptions::invalid_value& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
}

void patch_time(std::chrono::milliseconds& target, const std::optional<std::uint64_t>& value) {
   if (value) { target = to_ms(*value); }
}

void validate_pubsub_config(const pubsub_config& value) {
   validate_topic_list(value.allowed_topics, "allowed-topics");
   validate_topic_list(value.denied_topics, "denied-topics");
   try {
      forge::net::p2p::pubsub::validate(pubsub_options_for(value));
   } catch (const forge::net::p2p::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
}

forge::net::p2p::pubsub::options pubsub_options_for(const pubsub_config& settings) {
   auto out = forge::net::p2p::pubsub::options{};
   using native = forge::net::p2p::pubsub::version;
   if (settings.preferred_version) {
      switch (*settings.preferred_version) {
      case pubsub_version::v1_0: out.preferred = native::v1_0; break;
      case pubsub_version::v1_1: out.preferred = native::v1_1; break;
      case pubsub_version::v1_2: out.preferred = native::v1_2; break;
      case pubsub_version::v1_3: out.preferred = native::v1_3; break;
      default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P PubSub preferred version");
      }
   } else if (settings.partial_messages) {
      out.preferred = native::v1_3;
   }
   out.partial_messages = settings.partial_messages;
   if (out.partial_messages && out.preferred != native::v1_3) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P Partial Messages require preferred version v1-3");
   }
   using signature = forge::net::p2p::pubsub::signature_policy;
   out.signatures = settings.sign_publishes ? signature::strict_sign : signature::lax_no_sign;
   if (settings.signature_policy) {
      switch (*settings.signature_policy) {
      case pubsub_signature_policy::strict_sign: out.signatures = signature::strict_sign; break;
      case pubsub_signature_policy::strict_no_sign: out.signatures = signature::strict_no_sign; break;
      case pubsub_signature_policy::lax_sign: out.signatures = signature::lax_sign; break;
      case pubsub_signature_policy::lax_no_sign: out.signatures = signature::lax_no_sign; break;
      default: FORGE_THROW_EXCEPTION(exceptions::invalid_config, "invalid P2P PubSub signature policy");
      }
   }
   if ((out.signatures == signature::strict_no_sign && settings.sign_publishes) ||
       (out.signatures == signature::strict_sign && !settings.sign_publishes)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub signature policy conflicts with sign-publishes");
   }
   if (settings.allow_v1_0_fallback) { out.allow_v1_0_fallback = *settings.allow_v1_0_fallback; }
   if (settings.flood_publish) { out.flood_publish = *settings.flood_publish; }
   if (settings.peer_exchange) { out.peer_exchange = *settings.peer_exchange; }
   try {
      out.limits.max_data_size = forge::schema::checked_integral_cast<std::size_t>(settings.max_message_size);
      // Preserve the existing bounded message envelope, not a guarantee for every signing key/topic.
      out.limits.max_message_size = checked_sum(out.limits.max_data_size, 1024);
      const auto rpc_minimum = checked_sum(out.limits.max_message_size,
          checked_sum(1, forge::multiformats::varint_encoded_size(out.limits.max_message_size)));
      out.limits.max_rpc_size = std::max(out.limits.max_rpc_size, rpc_minimum);
      out.limits.max_rpc_size = forge::schema::checked_integral_cast<std::size_t>(
          settings.wire.max_rpc_size.value_or(out.limits.max_rpc_size));
      if (out.limits.max_rpc_size < rpc_minimum) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub RPC limit cannot fit the message envelope");
      }
      out.limits.max_outbound_queue_bytes = forge::schema::checked_integral_cast<std::size_t>(
          settings.wire.max_outbound_queue_bytes.value_or(out.limits.max_outbound_queue_bytes));
      out.limits.max_remote_topics_per_peer = forge::schema::checked_integral_cast<std::size_t>(
          settings.wire.max_remote_topics_per_peer.value_or(out.limits.max_remote_topics_per_peer));
      out.limits.max_remote_topic_entries = forge::schema::checked_integral_cast<std::size_t>(
          settings.wire.max_remote_topic_entries.value_or(out.limits.max_remote_topic_entries));
      out.limits.max_iwant_promises = forge::schema::checked_integral_cast<std::size_t>(
          settings.wire.max_iwant_promises.value_or(out.limits.max_iwant_promises));
      out.limits.max_topics = forge::schema::checked_integral_cast<std::size_t>(settings.max_topics);
      out.limits.max_validation_queue = forge::schema::checked_integral_cast<std::size_t>(settings.max_active_handlers);
   } catch (const forge::schema::exceptions::invalid_value& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
   auto& limits = out.limits;
   patch_size(limits.max_validation_attempts, settings.validation.max_attempts);
   patch_size(limits.max_validation_redeliveries, settings.validation.max_redeliveries);
   patch_size(limits.max_validation_requests, settings.validation.max_requests);
   patch_time(limits.validation_retry_initial_delay, settings.validation.retry_initial_delay_ms);
   patch_time(limits.validation_retry_max_delay, settings.validation.retry_max_delay_ms);
   patch_size(limits.idontwant_threshold, settings.idontwant.threshold);
   patch_size(limits.idontwant_ttl, settings.idontwant.ttl_heartbeats);
   patch_size(limits.max_idontwant_entries_per_peer, settings.idontwant.max_entries_per_peer);
   patch_size(limits.max_idontwant_bytes_per_peer, settings.idontwant.max_bytes_per_peer);
   patch_size(limits.max_idontwant_entries, settings.idontwant.max_entries);
   patch_size(limits.max_idontwant_bytes, settings.idontwant.max_bytes);
   patch_size(limits.max_partial_groups, settings.partial.max_groups);
   patch_size(limits.max_partial_groups_per_topic, settings.partial.max_groups_per_topic);
   patch_size(limits.max_partial_group_bytes, settings.partial.max_group_bytes);
   patch_size(limits.max_partial_metadata_size, settings.partial.max_metadata_size);
   patch_size(limits.partial_group_ttl, settings.partial.ttl_heartbeats);
   patch_size(limits.max_partial_callbacks, settings.partial.max_callbacks);
   patch_size(limits.max_partial_callback_bytes, settings.partial.max_callback_bytes);
   out.scoring = pubsub_scoring_for(settings.scoring);
   return out;
}

} // namespace forge::plugins::net::p2p::node::detail
