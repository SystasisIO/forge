module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.pubsub;

import forge.crypto.asymmetric;
import forge.crypto.digest.sha256;
import forge.multiformats.multicodec;
import forge.multiformats.exceptions;
import forge.multiformats.multihash;
import forge.multiformats.varint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;

#include "details/identity_signature.hxx"
#include "details/protobuf.hxx"

namespace forge::net::p2p::pubsub {
namespace {

constexpr auto signing_prefix = std::string_view{"libp2p-pubsub:"};

void validate_options(const options& opts) {
   const auto& limits = opts.limits;
   if (limits.max_rpc_size == 0 || limits.max_message_size == 0 || limits.max_data_size == 0 ||
       limits.max_topic_size == 0 || limits.max_subscriptions == 0 || limits.max_messages == 0 ||
       limits.max_control_entries == 0 || limits.max_message_ids == 0 || limits.max_peers_per_topic == 0 ||
       limits.max_topics == 0 || limits.max_validation_queue == 0 || limits.max_outbound_queue_bytes == 0 ||
       limits.max_ihave_per_peer == 0 || limits.max_iwant_per_peer == 0 || limits.max_graft_per_peer == 0 ||
       limits.heartbeat_initial_delay.count() <= 0 || limits.heartbeat_interval.count() <= 0 ||
       limits.fanout_ttl.count() <= 0 || limits.prune_backoff.count() <= 0 ||
       limits.unsubscribe_backoff.count() <= 0 || limits.mesh_n == 0 || limits.mesh_n_low == 0 ||
       limits.mesh_n_high < limits.mesh_n_low || limits.history_length == 0 || limits.history_gossip == 0 ||
       limits.gossip_lazy == 0 || !std::isfinite(limits.gossip_factor) || limits.gossip_factor <= 0.0 ||
       limits.gossip_factor > 1.0 || limits.gossip_retransmission == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid GossipSub options");
   }
}

void require_score(bool valid, std::string_view field) {
   if (!valid) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "invalid GossipSub scoring parameter: " + std::string{field});
   }
}

void finite_scores(std::initializer_list<std::pair<std::string_view, double>> fields) {
   for (const auto& [name, value] : fields) {
      require_score(std::isfinite(value), name);
   }
}

void score_duration(std::chrono::milliseconds duration, std::string_view field, bool positive) {
   const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::duration::max());
   require_score(duration.count() >= (positive ? 1 : 0) && duration <= maximum, field);
}

void validate_topic(const topic& value, const options& opts) {
   if (value.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub topic must not be empty");
   }
   if (value.value.size() > opts.limits.max_topic_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub topic exceeds max size");
   }
}

[[nodiscard]] std::vector<std::uint8_t> digest_bytes(const forge::crypto::digest::sha256& digest) {
   const auto span = digest.to_uint8_span();
   return {span.begin(), span.end()};
}

void append_bool(std::vector<std::uint8_t>& out, std::uint32_t field, bool value) {
   detail::append_uint64(out, field, value ? 1U : 0U);
}

[[nodiscard]] std::vector<std::uint8_t> encode_subscription_payload(const subscription& value, const options& opts) {
   validate_topic(value.subject, opts);
   auto out = std::vector<std::uint8_t>{};
   append_bool(out, 1, value.subscribe);
   detail::append_string(out, 2, value.subject.value);
   return out;
}

[[nodiscard]] subscription decode_subscription_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = subscription{};
   auto saw_topic = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub subscription flag must be varint");
         }
         out.subscribe = in.read_varint() != 0;
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub subscription topic must be bytes");
         }
         out.subject.value = in.string();
         saw_topic = true;
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub subscription is missing topic");
   }
   validate_topic(out.subject, opts);
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_message_payload(const message& value, const options& opts,
                                                               bool include_signature_key) {
   validate_topic(value.subject, opts);
   if (value.data.size() > opts.limits.max_data_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub message exceeds max data size");
   }
   auto out = std::vector<std::uint8_t>{};
   if (value.from) {
      detail::append_bytes(out, 1, value.from->to_bytes());
   }
   if (!value.data.empty()) {
      detail::append_bytes(out, 2, value.data);
   }
   if (!value.seqno.empty()) {
      detail::append_bytes(out, 3, value.seqno);
   }
   detail::append_string(out, 4, value.subject.value);
   if (include_signature_key && !value.signature.empty()) {
      detail::append_bytes(out, 5, value.signature);
   }
   if (include_signature_key && !value.key.empty()) {
      detail::append_bytes(out, 6, value.key);
   }
   if (out.size() > opts.limits.max_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub message exceeds max size");
   }
   return out;
}

[[nodiscard]] message decode_message_payload(std::span<const std::uint8_t> bytes, const options& opts,
                                             bool receive_policy, bool& invalid) {
   if (bytes.size() > opts.limits.max_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message exceeds max size");
   }
   auto out = message{};
   auto saw_topic = false;
   auto auth_present = false;
   const auto no_auth = receive_policy && opts.signatures == signature_policy::strict_no_sign;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (field == 1 || field == 3 || field == 5 || field == 6) {
         auth_present = true; // Presence is sticky: empty or repeated protobuf fields still carry auth info.
      }
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message source must be bytes");
         }
         if (no_auth) {
            in.skip(type); // Forbidden source bytes are structural input, not a Peer ID to parse.
         } else {
            out.from = peer_id::from_bytes(in.bytes());
         }
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message data must be bytes");
         }
         out.data = in.bytes();
         if (out.data.size() > opts.limits.max_data_size) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message exceeds max data size");
         }
         break;
      case 3:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message seqno must be bytes");
         }
         if (no_auth) { in.skip(type); } else { out.seqno = in.bytes(); }
         break;
      case 4:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message topic must be bytes");
         }
         out.subject.value = in.string();
         saw_topic = true;
         break;
      case 5:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message signature must be bytes");
         }
         if (no_auth) { in.skip(type); } else { out.signature = in.bytes(); }
         break;
      case 6:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message key must be bytes");
         }
         if (no_auth) { in.skip(type); } else { out.key = in.bytes(); }
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub message is missing topic");
   }
   validate_topic(out.subject, opts);
   // Finish all structural validation before deciding a per-message policy rejection.
   invalid = receive_policy && ((no_auth && auth_present) ||
       (opts.signatures == signature_policy::strict_sign && out.seqno.size() != 8U));
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_peer_payload(const peer_info& value) {
   auto out = std::vector<std::uint8_t>{};
   detail::append_bytes(out, 1, value.peer.to_bytes());
   if (!value.signed_peer_record.empty()) {
      detail::append_bytes(out, 2, value.signed_peer_record);
   }
   return out;
}

[[nodiscard]] peer_info decode_peer_payload(std::span<const std::uint8_t> bytes) {
   auto out = peer_info{};
   auto saw_peer = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub peer id must be bytes");
         }
         out.peer = peer_id::from_bytes(in.bytes());
         saw_peer = true;
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub signed peer record must be bytes");
         }
         out.signed_peer_record = in.bytes();
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_peer) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub peer info is missing peer id");
   }
   return out;
}

void append_message_ids(std::vector<std::uint8_t>& out, std::uint32_t field,
                        const std::vector<std::vector<std::uint8_t>>& ids, const options& opts) {
   if (ids.size() > opts.limits.max_message_ids) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub control has too many message ids");
   }
   for (const auto& id : ids) {
      if (id.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub message id must not be empty");
      }
      detail::append_bytes(out, field, id);
   }
}

[[nodiscard]] std::vector<std::uint8_t> encode_ihave_payload(const control::ihave& value, const options& opts) {
   validate_topic(value.subject, opts);
   auto out = std::vector<std::uint8_t>{};
   detail::append_string(out, 1, value.subject.value);
   append_message_ids(out, 2, value.message_ids, opts);
   return out;
}

[[nodiscard]] control::ihave decode_ihave_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = control::ihave{};
   auto saw_topic = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IHAVE topic must be bytes");
         }
         out.subject.value = in.string();
         saw_topic = true;
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IHAVE message id must be bytes");
         }
         out.message_ids.push_back(in.bytes());
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IHAVE is missing topic");
   }
   validate_topic(out.subject, opts);
   if (out.message_ids.size() > opts.limits.max_message_ids) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IHAVE has too many message ids");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_iwant_payload(const control::iwant& value, const options& opts) {
   auto out = std::vector<std::uint8_t>{};
   append_message_ids(out, 1, value.message_ids, opts);
   return out;
}

[[nodiscard]] control::iwant decode_iwant_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = control::iwant{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IWANT message id must be bytes");
         }
         out.message_ids.push_back(in.bytes());
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (out.message_ids.size() > opts.limits.max_message_ids) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub IWANT has too many message ids");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_graft_payload(const control::graft& value, const options& opts) {
   validate_topic(value.subject, opts);
   auto out = std::vector<std::uint8_t>{};
   detail::append_string(out, 1, value.subject.value);
   return out;
}

[[nodiscard]] control::graft decode_graft_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = control::graft{};
   auto saw_topic = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub GRAFT topic must be bytes");
         }
         out.subject.value = in.string();
         saw_topic = true;
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub GRAFT is missing topic");
   }
   validate_topic(out.subject, opts);
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_prune_payload(const control::prune& value, const options& opts) {
   validate_topic(value.subject, opts);
   if (value.peers.size() > opts.limits.max_peers_per_topic) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub PRUNE has too many peers");
   }
   auto out = std::vector<std::uint8_t>{};
   detail::append_string(out, 1, value.subject.value);
   if (opts.preferred == version::v1_1) {
      for (const auto& peer : value.peers) {
         detail::append_bytes(out, 2, encode_peer_payload(peer));
      }
      if (value.backoff.count() > 0) {
         detail::append_uint64(out, 3, static_cast<std::uint64_t>(value.backoff.count()));
      }
   }
   return out;
}

[[nodiscard]] control::prune decode_prune_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = control::prune{};
   auto saw_topic = false;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub PRUNE topic must be bytes");
         }
         out.subject.value = in.string();
         saw_topic = true;
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub PRUNE peer must be bytes");
         }
         out.peers.push_back(decode_peer_payload(in.bytes()));
         break;
      case 3:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub PRUNE backoff must be varint");
         }
         out.backoff = std::chrono::seconds{static_cast<std::int64_t>(in.read_varint())};
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (!saw_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub PRUNE is missing topic");
   }
   validate_topic(out.subject, opts);
   if (out.peers.size() > opts.limits.max_peers_per_topic) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub PRUNE has too many peers");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_control_payload(const control& value, const options& opts) {
   const auto total = value.have.size() + value.want.size() + value.grafts.size() + value.prunes.size();
   if (total > opts.limits.max_control_entries) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub control message has too many entries");
   }
   auto out = std::vector<std::uint8_t>{};
   for (const auto& item : value.have) {
      detail::append_bytes(out, 1, encode_ihave_payload(item, opts));
   }
   for (const auto& item : value.want) {
      detail::append_bytes(out, 2, encode_iwant_payload(item, opts));
   }
   for (const auto& item : value.grafts) {
      detail::append_bytes(out, 3, encode_graft_payload(item, opts));
   }
   for (const auto& item : value.prunes) {
      detail::append_bytes(out, 4, encode_prune_payload(item, opts));
   }
   return out;
}

[[nodiscard]] control decode_control_payload(std::span<const std::uint8_t> bytes, const options& opts) {
   auto out = control{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (type != detail::wire_type::length_delimited) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub control entry must be bytes");
      }
      switch (field) {
      case 1:
         out.have.push_back(decode_ihave_payload(in.bytes(), opts));
         break;
      case 2:
         out.want.push_back(decode_iwant_payload(in.bytes(), opts));
         break;
      case 3:
         out.grafts.push_back(decode_graft_payload(in.bytes(), opts));
         break;
      case 4:
         out.prunes.push_back(decode_prune_payload(in.bytes(), opts));
         break;
      default:
         in.skip(type);
         break;
      }
   }
   const auto total = out.have.size() + out.want.size() + out.grafts.size() + out.prunes.size();
   if (total > opts.limits.max_control_entries) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub control message has too many entries");
   }
   return out;
}

[[nodiscard]] codec::received_rpc decode_rpc_payload(std::span<const std::uint8_t> bytes, const options& opts,
                                                      bool receive_policy) {
   validate_options(opts);
   const auto payload = detail::unwrap_message(bytes, opts.limits.max_rpc_size);
   auto out = codec::received_rpc{};
   auto messages = std::size_t{};
   auto in = detail::reader{payload};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub subscription must be bytes");
         }
         out.value.subscriptions.push_back(decode_subscription_payload(in.bytes(), opts));
         break;
      case 2: {
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub publish message must be bytes");
         }
         // Bound the original wire count, including messages rejected by receive policy.
         if (messages == opts.limits.max_messages) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub RPC exceeds element limits");
         }
         ++messages;
         auto invalid = false;
         auto value = decode_message_payload(in.bytes(), opts, receive_policy, invalid);
         if (invalid) { out.invalid_messages.push_back(std::move(value.subject)); }
         else { out.value.messages.push_back(std::move(value)); }
         break;
      }
      case 3:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub control message must be bytes");
         }
         out.value.control_value = decode_control_payload(in.bytes(), opts);
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (out.value.subscriptions.size() > opts.limits.max_subscriptions) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "GossipSub RPC exceeds element limits");
   }
   return out;
}

[[nodiscard]] std::optional<public_key> public_key_from_message(const message& value) {
   if (!value.from) {
      return std::nullopt;
   }
   if (!value.key.empty()) {
      return decode_public_key(value.key);
   }
   const auto hash = forge::multiformats::multihash::decode(value.from->to_bytes());
   if (hash.code != forge::multiformats::code_value(forge::multiformats::multicodec_code::identity) ||
       hash.digest.empty()) {
      return std::nullopt;
   }
   return decode_public_key(hash.digest);
}

} // namespace

void validate(const topic_score_params& p) {
   finite_scores({{"topic_weight", p.topic_weight}, {"time_in_mesh_weight", p.time_in_mesh_weight},
                  {"time_in_mesh_cap", p.time_in_mesh_cap},
                  {"first_message_deliveries_weight", p.first_message_deliveries_weight},
                  {"first_message_deliveries_decay", p.first_message_deliveries_decay},
                  {"first_message_deliveries_cap", p.first_message_deliveries_cap},
                  {"mesh_message_deliveries_weight", p.mesh_message_deliveries_weight},
                  {"mesh_message_deliveries_decay", p.mesh_message_deliveries_decay},
                  {"mesh_message_deliveries_cap", p.mesh_message_deliveries_cap},
                  {"mesh_message_deliveries_threshold", p.mesh_message_deliveries_threshold},
                  {"mesh_failure_penalty_weight", p.mesh_failure_penalty_weight},
                  {"mesh_failure_penalty_decay", p.mesh_failure_penalty_decay},
                  {"invalid_message_deliveries_weight", p.invalid_message_deliveries_weight},
                  {"invalid_message_deliveries_decay", p.invalid_message_deliveries_decay}});
   require_score(p.topic_weight >= 0.0, "topic_weight");
   require_score(p.time_in_mesh_weight >= 0.0, "time_in_mesh_weight");
   require_score(p.first_message_deliveries_weight >= 0.0, "first_message_deliveries_weight");
   require_score(p.mesh_message_deliveries_weight <= 0.0, "mesh_message_deliveries_weight");
   require_score(p.mesh_failure_penalty_weight <= 0.0, "mesh_failure_penalty_weight");
   require_score(p.invalid_message_deliveries_weight <= 0.0, "invalid_message_deliveries_weight");
   require_score(p.time_in_mesh_cap >= 0.0 && (p.time_in_mesh_weight == 0.0 || p.time_in_mesh_cap > 0.0),
                 "time_in_mesh_cap");
   require_score(p.first_message_deliveries_cap >= 0.0 &&
                     (p.first_message_deliveries_weight == 0.0 || p.first_message_deliveries_cap > 0.0),
                 "first_message_deliveries_cap");
   require_score(p.mesh_message_deliveries_cap >= 0.0 &&
                     (p.mesh_message_deliveries_weight == 0.0 || p.mesh_message_deliveries_cap > 0.0),
                 "mesh_message_deliveries_cap");
   require_score(p.mesh_message_deliveries_threshold >= 0.0 &&
                     (p.mesh_message_deliveries_weight == 0.0 || p.mesh_message_deliveries_threshold > 0.0),
                 "mesh_message_deliveries_threshold");
   for (const auto& [name, decay] : {
            std::pair{"first_message_deliveries_decay", p.first_message_deliveries_decay},
            std::pair{"mesh_message_deliveries_decay", p.mesh_message_deliveries_decay},
            std::pair{"mesh_failure_penalty_decay", p.mesh_failure_penalty_decay},
            std::pair{"invalid_message_deliveries_decay", p.invalid_message_deliveries_decay}}) {
      require_score(decay > 0.0 && decay < 1.0, name);
   }
   score_duration(p.time_in_mesh_quantum, "time_in_mesh_quantum", true);
   score_duration(p.mesh_message_deliveries_window, "mesh_message_deliveries_window", false);
   score_duration(p.mesh_message_deliveries_activation, "mesh_message_deliveries_activation", false);
   require_score((p.mesh_message_deliveries_weight == 0.0 && p.mesh_failure_penalty_weight == 0.0) ||
                     p.mesh_message_deliveries_activation >= std::chrono::seconds{1},
                 "mesh_message_deliveries_activation");
}

void validate(const score_thresholds& t) {
   finite_scores({{"gossip_threshold", t.gossip_threshold}, {"publish_threshold", t.publish_threshold},
                  {"graylist_threshold", t.graylist_threshold}, {"accept_px_threshold", t.accept_px_threshold},
                  {"opportunistic_graft_threshold", t.opportunistic_graft_threshold}});
   require_score(t.gossip_threshold <= 0.0, "gossip_threshold");
   require_score(t.publish_threshold <= t.gossip_threshold, "publish_threshold");
   require_score(t.graylist_threshold <= t.publish_threshold, "graylist_threshold");
   require_score(t.accept_px_threshold >= 0.0, "accept_px_threshold");
   require_score(t.opportunistic_graft_threshold >= 0.0, "opportunistic_graft_threshold");
}

void validate(const scoring_params& p) {
   validate(p.thresholds);
   finite_scores({{"topic_score_cap", p.topic_score_cap}, {"app_specific_weight", p.app_specific_weight},
                  {"ip_colocation_factor_weight", p.ip_colocation_factor_weight},
                  {"ip_colocation_factor_threshold", p.ip_colocation_factor_threshold},
                  {"behaviour_penalty_weight", p.behaviour_penalty_weight},
                  {"behaviour_penalty_threshold", p.behaviour_penalty_threshold},
                  {"behaviour_penalty_decay", p.behaviour_penalty_decay}, {"decay_to_zero", p.decay_to_zero}});
   require_score(p.topic_score_cap >= 0.0, "topic_score_cap");
   require_score(p.app_specific_weight == 0.0 || static_cast<bool>(p.app_specific_score), "app_specific_score");
   require_score(p.ip_colocation_factor_weight <= 0.0, "ip_colocation_factor_weight");
   require_score(p.ip_colocation_factor_threshold >= 1.0, "ip_colocation_factor_threshold");
   require_score(p.behaviour_penalty_weight <= 0.0, "behaviour_penalty_weight");
   require_score(p.behaviour_penalty_threshold >= 0.0, "behaviour_penalty_threshold");
   require_score(p.behaviour_penalty_decay > 0.0 && p.behaviour_penalty_decay < 1.0, "behaviour_penalty_decay");
   require_score(p.decay_to_zero > 0.0 && p.decay_to_zero < 1.0, "decay_to_zero");
   score_duration(p.decay_interval, "decay_interval", true);
   require_score(p.decay_interval >= std::chrono::seconds{1}, "decay_interval");
   score_duration(p.retain_score, "retain_score", false);
   score_duration(p.seen_message_ttl, "seen_message_ttl", true);
   const auto& b = p.limits;
   require_score(b.max_connected_peers > 0 && b.max_retained_peers >= b.max_connected_peers,
                 "limits.max_retained_peers");
   require_score(b.max_messages > 0 && b.max_delivery_peers > 0 && b.max_message_id_size > 0 && b.max_peer_id_size > 0 &&
                     b.max_topics > 0 && b.max_topic_size > 0 && b.max_ips_per_peer > 0 && b.max_ip_allowlist > 0,
                 "limits");
   require_score(p.topics.size() <= b.max_topics, "topics");
   require_score(p.ip_colocation_factor_allowlist.size() <= b.max_ip_allowlist, "ip_colocation_factor_allowlist");
   for (const auto& [subject, params] : p.topics) {
      require_score(!subject.value.empty() && subject.value.size() <= b.max_topic_size, "topics.key");
      validate(params);
   }
}

void validate(const options& opts) {
   validate_options(opts);
   const auto& b = opts.limits;
   score_duration(b.iwant_followup_time, "router.iwant_followup_time", true);
   require_score(b.max_remote_topics_per_peer > 0 && b.max_remote_topic_entries > 0 && b.max_iwant_promises > 0 &&
                 b.mesh_n_low <= b.mesh_n && b.mesh_n <= b.mesh_n_high &&
                 b.mesh_outbound_min <= b.mesh_n / 2 && b.mesh_outbound_min < b.mesh_n_low &&
                 b.opportunistic_graft_ticks > 0 && b.history_gossip <= b.history_length &&
                 b.max_messages <= (std::numeric_limits<std::size_t>::max)() / b.history_length, "router.limits");
   if (opts.scoring) {
      validate(*opts.scoring);
   }
}

protocol_id codec::protocol(version value) {
   switch (value) {
   case version::v1_0:
      return builtins::meshsub_v10;
   case version::v1_1:
      return builtins::meshsub_v11;
   }
   return builtins::meshsub_v11;
}

std::vector<std::uint8_t> codec::encode(const rpc& value) {
   return encode(value, options{});
}

std::vector<std::uint8_t> codec::encode(const rpc& value, const options& opts) {
   validate_options(opts);
   if (value.subscriptions.size() > opts.limits.max_subscriptions) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub RPC has too many subscriptions");
   }
   if (value.messages.size() > opts.limits.max_messages) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub RPC has too many messages");
   }
   auto out = std::vector<std::uint8_t>{};
   for (const auto& item : value.subscriptions) {
      detail::append_bytes(out, 1, encode_subscription_payload(item, opts));
   }
   for (const auto& item : value.messages) {
      detail::append_bytes(out, 2, encode_message_payload(item, opts, true));
   }
   if (value.control_value) {
      detail::append_bytes(out, 3, encode_control_payload(*value.control_value, opts));
   }
   if (out.size() > opts.limits.max_rpc_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub RPC exceeds max size");
   }
   return detail::wrap_message(out);
}

std::size_t codec::control_payload_size(const control& value, const options& opts) {
   validate_options(opts);
   return encode_control_payload(value, opts).size();
}

namespace {

std::optional<codec::gossip_chunk> next_gossip_chunk(const control& value, codec::gossip_cursor& cursor,
                                                   const options& opts, bool materialize) {
   if (cursor.have > value.have.size() || cursor.want > value.want.size() ||
       (cursor.have < value.have.size() &&
           (cursor.want != 0 || cursor.id > value.have[cursor.have].message_ids.size())) ||
       (cursor.have == value.have.size() && cursor.want < value.want.size() &&
           cursor.id > value.want[cursor.want].message_ids.size()) ||
       (cursor.have == value.have.size() && cursor.want == value.want.size() && cursor.id != 0)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub gossip cursor exceeds its input");
   }
   const auto limit = opts.limits.max_rpc_size;
   // Match the serializer's canonical length-delimited headers without buffers
   // or ID copies. Every addition is checked against the protobuf payload bound.
   const auto field_size = [limit](std::uint32_t field, std::size_t bytes) -> std::optional<std::size_t> {
      const auto header = forge::multiformats::varint_encoded_size((std::uint64_t{field} << 3U) | 2U) +
          forge::multiformats::varint_encoded_size(bytes);
      if (header > limit || bytes > limit - header) { return std::nullopt; }
      return bytes + header;
   };
   const auto payload_size = [&field_size](std::size_t bytes) -> std::optional<std::size_t> {
      const auto entry = field_size(1, bytes); // IHAVE and IWANT keys are both one byte.
      return entry ? field_size(3, *entry) : std::nullopt;
   };
   const auto chunk = [](rpc value, std::size_t payload) {
      const auto prefix = forge::multiformats::varint_encoded_size(payload);
      if (prefix > (std::numeric_limits<std::size_t>::max)() - payload) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub framed size overflows");
      }
      return codec::gossip_chunk{.value = std::move(value), .wire_bytes = payload + prefix};
   };
   while (cursor.have < value.have.size()) {
      const auto& have = value.have[cursor.have];
      if (have.subject.value.empty() || have.subject.value.size() > opts.limits.max_topic_size ||
          have.subject.value.size() > limit) { ++cursor.have; cursor.id = 0; continue; }
      const auto base = field_size(1, have.subject.value.size());
      if (!base || !payload_size(*base)) { ++cursor.have; cursor.id = 0; continue; }
      auto bytes = *base;
      auto count = std::size_t{};
      auto ids = std::vector<std::vector<std::uint8_t>>{};
      while (cursor.id < have.message_ids.size()) {
         const auto& id = have.message_ids[cursor.id];
         const auto field = field_size(2, id.size());
         if (id.empty() || !field || *field > limit - *base || !payload_size(*base + *field)) { ++cursor.id; continue; }
         if (count == opts.limits.max_message_ids || *field > limit - bytes || !payload_size(bytes + *field)) {
            break;
         }
         bytes += *field;
         ++count;
         if (materialize) { ids.push_back(id); }
         ++cursor.id;
      }
      if (cursor.id == have.message_ids.size()) { ++cursor.have; cursor.id = 0; }
      if (count != 0) {
         auto out = rpc{};
         if (materialize) {
            out.control_value.emplace();
            out.control_value->have.push_back(control::ihave{.subject = have.subject, .message_ids = std::move(ids)});
         }
         return chunk(std::move(out), *payload_size(bytes));
      }
   }
   auto bytes = std::size_t{};
   auto count = std::size_t{};
   auto ids = std::vector<std::vector<std::uint8_t>>{};
   const auto wanted = [&] {
      auto out = rpc{};
      if (materialize) {
         out.control_value.emplace();
         out.control_value->want.push_back(control::iwant{.message_ids = std::move(ids)});
      }
      return chunk(std::move(out), *payload_size(bytes));
   };
   while (cursor.want < value.want.size()) {
      const auto& want = value.want[cursor.want];
      while (cursor.id < want.message_ids.size()) {
         const auto& id = want.message_ids[cursor.id];
         const auto field = field_size(1, id.size());
         if (id.empty() || !field || !payload_size(*field)) { ++cursor.id; continue; }
         if (count == opts.limits.max_message_ids || *field > limit - bytes || !payload_size(bytes + *field)) {
            return wanted();
         }
         bytes += *field;
         ++count;
         if (materialize) { ids.push_back(id); }
         ++cursor.id;
      }
      ++cursor.want;
      cursor.id = 0;
   }
   if (count == 0) { return std::nullopt; }
   return wanted();
}

} // namespace

std::optional<codec::gossip_chunk> codec::next_gossip(const control& value, gossip_cursor& cursor, const options& opts) {
   validate_options(opts);
   return next_gossip_chunk(value, cursor, opts, true);
}

std::optional<std::size_t> codec::gossip_wire_size(const control& value, const options& opts) {
   validate_options(opts);
   auto cursor = gossip_cursor{};
   auto bytes = std::size_t{};
   while (const auto chunk = next_gossip_chunk(value, cursor, opts, false)) {
      if (chunk->wire_bytes > opts.limits.max_outbound_queue_bytes - bytes) { return std::nullopt; }
      bytes += chunk->wire_bytes;
   }
   return bytes;
}

rpc codec::decode(std::span<const std::uint8_t> bytes) {
   return decode(bytes, options{});
}

rpc codec::decode(std::span<const std::uint8_t> bytes, const options& opts) {
   return decode_rpc_payload(bytes, opts, false).value;
}

codec::received_rpc codec::decode_received(std::span<const std::uint8_t> bytes, const options& opts) {
   return decode_rpc_payload(bytes, opts, true);
}

std::vector<std::uint8_t> codec::encode_message(const message& value) {
   return encode_message(value, options{});
}

std::vector<std::uint8_t> codec::encode_message(const message& value, const options& opts) {
   validate_options(opts);
   return encode_message_payload(value, opts, true);
}

std::vector<std::uint8_t> codec::signing_payload(const message& value) {
   return signing_payload(value, options{});
}

std::vector<std::uint8_t> codec::signing_payload(const message& value, const options& opts) {
   validate_options(opts);
   auto out = std::vector<std::uint8_t>{signing_prefix.begin(), signing_prefix.end()};
   const auto encoded = encode_message_payload(value, opts, false);
   out.insert(out.end(), encoded.begin(), encoded.end());
   return out;
}

std::vector<std::uint8_t> codec::message_id(const message& value) {
   return message_id(value, options{});
}

std::vector<std::uint8_t> codec::message_id(const message& value, const options& opts) {
   validate_options(opts);
   if (value.from && !value.seqno.empty()) {
      auto out = value.from->to_bytes();
      out.insert(out.end(), value.seqno.begin(), value.seqno.end());
      return out;
   }
   const auto encoded = encode_message_payload(value, opts, true);
   return digest_bytes(forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{encoded}));
}

void codec::sign_message(message& value, const forge::crypto::asymmetric::private_key& key) {
   sign_message(value, key, options{});
}

void codec::sign_message(message& value, const forge::crypto::asymmetric::private_key& key, const options& opts) {
   if (value.seqno.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "GossipSub signed message requires seqno");
   }
   const auto public_value = public_key_from_crypto(key.get_public_key());
   value.from = make_peer_id(public_value);
   value.key = encode_public_key(public_value);
   value.signature.clear();
   const auto payload = signing_payload(value, opts);
   value.signature = sign_identity(key, payload);
}

bool codec::verify_message(const message& value) {
   return verify_message(value, options{});
}

bool codec::verify_message(const message& value, const options& opts) {
   if (!value.from || value.signature.empty() || value.seqno.empty()) {
      return false;
   }
   try {
      const auto key = public_key_from_message(value);
      if (!key || make_peer_id(*key) != *value.from) {
         return false;
      }
      const auto payload = signing_payload(value, opts);
      return verify_identity_signature(*key, payload, value.signature);
   } catch (const forge::exceptions::base&) {
      return false;
   }
}

} // namespace forge::net::p2p::pubsub
