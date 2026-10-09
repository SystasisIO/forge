module;

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/address.hpp>

export module forge.net.p2p.pubsub;

import forge.crypto.asymmetric;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;

export namespace forge::net::p2p::pubsub {

enum class version : std::uint8_t {
   v1_0,
   v1_1,
   v1_2,
   v1_3,
};

enum class validation_result : std::uint8_t {
   accept,
   reject,
   ignore,
   retry,
};

enum class signature_policy : std::uint8_t {
   strict_sign,
   strict_no_sign,
   lax_sign,
   lax_no_sign,
};

struct topic {
   std::string value;

   [[nodiscard]] friend bool operator==(const topic&, const topic&) noexcept = default;
   [[nodiscard]] friend auto operator<=>(const topic&, const topic&) noexcept = default;
};

struct topic_score_params {
   double topic_weight = 1.0;
   double time_in_mesh_weight = 0.0;
   std::chrono::milliseconds time_in_mesh_quantum{1'000};
   double time_in_mesh_cap = 3'600.0;
   double first_message_deliveries_weight = 0.0;
   double first_message_deliveries_decay = 0.99;
   double first_message_deliveries_cap = 100.0;
   double mesh_message_deliveries_weight = 0.0;
   double mesh_message_deliveries_decay = 0.99;
   double mesh_message_deliveries_cap = 100.0;
   double mesh_message_deliveries_threshold = 20.0;
   std::chrono::milliseconds mesh_message_deliveries_window{10};
   std::chrono::milliseconds mesh_message_deliveries_activation{5'000};
   double mesh_failure_penalty_weight = 0.0;
   double mesh_failure_penalty_decay = 0.99;
   double invalid_message_deliveries_weight = -1.0;
   double invalid_message_deliveries_decay = 0.99;
};

struct score_thresholds {
   double gossip_threshold = -10.0;
   double publish_threshold = -50.0;
   double graylist_threshold = -80.0;
   double accept_px_threshold = 10.0;
   double opportunistic_graft_threshold = 20.0;
};

struct score_limits {
   std::size_t max_connected_peers = 1'024;
   // Includes a reserved retention slot for every connected peer. No penalty eviction on disconnect.
   std::size_t max_retained_peers = 1'024;
   std::size_t max_messages = 8'192;
   std::size_t max_delivery_peers = 128; // Includes the first sender.
   std::size_t max_message_id_size = 256;
   std::size_t max_peer_id_size = 512;
   std::size_t max_topics = 128;
   std::size_t max_topic_size = 255;
   std::size_t max_ips_per_peer = 8;
   std::size_t max_ip_allowlist = 128;
};

struct scoring_params {
   std::map<topic, topic_score_params> topics;
   double topic_score_cap = 0.0; // Zero disables the aggregate positive topic cap.
   // The node samples this outside ALL node/engine locks, then passes the finite value to the engine.
   std::function<double(const peer_id&)> app_specific_score;
   double app_specific_weight = 0.0;
   double ip_colocation_factor_weight = 0.0;
   double ip_colocation_factor_threshold = 10.0;
   // Exact numeric IPs, matching the Rust donor. No inferred relay IP or automatic IPv6 subnet.
   std::vector<boost::asio::ip::address> ip_colocation_factor_allowlist;
   double behaviour_penalty_weight = 0.0;
   double behaviour_penalty_threshold = 0.0;
   double behaviour_penalty_decay = 0.99;
   std::chrono::milliseconds decay_interval{1'000};
   double decay_to_zero = 0.01;
   std::chrono::milliseconds retain_score{3'600'000};
   std::chrono::milliseconds seen_message_ttl{120'000};
   score_thresholds thresholds{};
   score_limits limits{};
};

// Throws invalid_options. Disabled scoring (options::scoring == nullopt) has no scoring requirements.
void validate(const topic_score_params& params);
void validate(const score_thresholds& thresholds);
void validate(const scoring_params& params);

struct topic_score_snapshot {
   topic subject;
   bool in_mesh = false;
   bool mesh_deliveries_active = false;
   std::chrono::steady_clock::duration time_in_mesh{};
   double first_message_deliveries = 0.0;
   double mesh_message_deliveries = 0.0;
   double mesh_failure_penalty = 0.0;
   double invalid_message_deliveries = 0.0;
   double weighted_score = 0.0;
};

struct peer_score_snapshot {
   peer_id peer;
   bool connected = false;
   std::optional<std::chrono::steady_clock::time_point> retain_until;
   double value = 0.0;
   double topic_score = 0.0;
   double app_specific_sample = 0.0;
   double app_specific_score = 0.0;
   double ip_colocation_factor = 0.0;
   double ip_colocation_score = 0.0;
   double behaviour_penalty = 0.0;
   double behaviour_score = 0.0;
   std::vector<boost::asio::ip::address> ips;
   std::vector<topic_score_snapshot> topics;
};

struct score_snapshot {
   std::vector<peer_score_snapshot> peers;
   std::size_t connected_peers = 0;
   std::size_t retained_peers = 0;
   std::size_t delivery_records = 0;
   std::size_t pending_validations = 0;
   std::uint64_t capacity_rejections = 0;
};

enum class trace_kind : std::uint8_t {
   rpc_read,
   rpc_write,
   validation_committed,
   delivery,
};

struct trace_event {
   trace_kind kind = trace_kind::rpc_read;
   peer_id peer;
   std::optional<peer_id> author;
   topic subject;
   std::span<const std::uint8_t> message_id;
   std::span<const std::uint8_t> seqno;
   std::span<const std::uint8_t> data;
   std::uint64_t generation = 0;
   std::optional<validation_result> result;
   protocol_id protocol;
   std::uint64_t session_id = 0;
   std::int64_t stream_id = -1;
   // The original completed length-delimited I/O, not a reconstructed encoding.
   std::span<const std::uint8_t> framed_rpc;
};

// Synchronous, passive and outside node/engine locks. All spans expire when the callback returns.
using tracer = std::function<void(const trace_event&)>;

struct limits {
   std::size_t max_rpc_size = 1024 * 1024;
   std::size_t max_message_size = 1024 * 1024;
   std::size_t max_data_size = 1024 * 1024;
   std::size_t max_topic_size = 255;
   std::size_t max_subscriptions = 1024;
   std::size_t max_messages = 1024;
   std::size_t max_control_entries = 1024;
   std::size_t max_message_ids = 5000;
   std::size_t max_peers_per_topic = 12;
   std::size_t max_topics = 1024;
   std::size_t max_validation_queue = 4096;
   std::size_t max_validation_attempts = 3;
   std::size_t max_validation_redeliveries = 8;
   std::size_t max_validation_requests = 8;
   std::size_t max_remote_topics_per_peer = 128;
   std::size_t max_remote_topic_entries = 8'192;
   std::size_t max_iwant_promises = 4'096;
   std::size_t max_outbound_queue_bytes = 4 * 1024 * 1024;
   std::size_t max_ihave_per_peer = 10;
   std::size_t max_iwant_per_peer = 10;
   std::size_t max_graft_per_peer = 10; // Outbound GRAFT entries per control batch, not an inbound cutoff.
   std::chrono::milliseconds heartbeat_initial_delay{100};
   std::chrono::milliseconds heartbeat_interval{1'000};
   std::chrono::seconds fanout_ttl{60};
   std::chrono::seconds prune_backoff{60};
   std::chrono::seconds unsubscribe_backoff{10};
   std::chrono::milliseconds validation_retry_initial_delay{250};
   std::chrono::milliseconds validation_retry_max_delay{5'000};
   std::size_t mesh_n = 6;
   std::size_t mesh_n_low = 5;
   std::size_t mesh_n_high = 12;
   std::size_t mesh_outbound_min = 2;
   std::size_t mesh_score_min = 4;
   std::size_t opportunistic_graft_ticks = 60;
   std::size_t opportunistic_graft_peers = 2;
   std::chrono::milliseconds iwant_followup_time{3'000};
   std::size_t history_length = 5;
   std::size_t history_gossip = 3;
   std::size_t gossip_lazy = 6;
   double gossip_factor = 0.25;
   std::size_t gossip_retransmission = 3;
   // Local wire budgets, not peer capability or protocol-version requirements.
   std::size_t max_idontwant_message_id_size = 256;
   std::size_t max_partial_group_id_size = 256;
   std::size_t max_partial_metadata_size = 64 * 1024;
};

struct options {
   version preferred = version::v1_1;
   bool allow_v1_0_fallback = true;
   signature_policy signatures = signature_policy::strict_sign;
   limits limits{};
   std::optional<scoring_params> scoring; // Donor-compatible opt-in, disabled by default.
   bool flood_publish = false;
   bool peer_exchange = false;
   pubsub::tracer tracer;
};

// Validate enabled scoring once at configuration/admission, not on every wire encode/decode.
void validate(const options& opts);

struct subscription {
   bool subscribe = true;
   topic subject;
   // Presence is wire data; only a subscribed, extension-enabled runtime acts on these flags.
   std::optional<bool> requests_partial;
   std::optional<bool> supports_sending_partial;
};

struct message {
   std::optional<peer_id> from;
   std::vector<std::uint8_t> data;
   std::vector<std::uint8_t> seqno;
   topic subject;
   std::vector<std::uint8_t> signature;
   std::vector<std::uint8_t> key;
};

struct peer_info {
   peer_id peer;
   std::vector<std::uint8_t> signed_peer_record;
};

// Wire advertisement only. First-RPC placement and mutual support belong to the stream owner.
struct extensions {
   std::optional<bool> partial_messages;
};

// Preserve proto2 presence, including explicitly empty application-defined bytes.
// Data uses max_data_size, metadata/group have separate bounds, the payload uses max_message_size.
struct partial_message {
   std::optional<topic> subject;
   std::optional<std::vector<std::uint8_t>> group_id;
   std::optional<std::vector<std::uint8_t>> data;
   std::optional<std::vector<std::uint8_t>> metadata;
};

struct control {
   struct ihave {
      topic subject;
      std::vector<std::vector<std::uint8_t>> message_ids;
   };

   struct iwant {
      std::vector<std::vector<std::uint8_t>> message_ids;
   };

   struct idontwant {
      std::vector<std::vector<std::uint8_t>> message_ids;
   };

   struct graft {
      topic subject;
   };

   struct prune {
      topic subject;
      std::vector<peer_info> peers;
      std::chrono::seconds backoff{0};
   };

   std::vector<ihave> have;
   std::vector<iwant> want;
   std::vector<graft> grafts;
   std::vector<prune> prunes;
   std::vector<idontwant> dont_want;
   std::optional<pubsub::extensions> extensions;
};

struct rpc {
   std::vector<subscription> subscriptions;
   std::vector<message> messages;
   std::optional<control> control_value;
   std::optional<partial_message> partial;
};

struct publish_options {
   bool sign = true;
};

struct event {
   peer_id source;
   message value;
};

using handler = std::function<boost::asio::awaitable<validation_result>(event)>;

struct score {
   double value = 0.0;
   std::uint64_t invalid_messages = 0;
   std::uint64_t duplicate_messages = 0;
   std::uint64_t delivered_messages = 0;
};

struct snapshot {
   std::size_t topics = 0;
   std::size_t peers = 0;
   std::size_t mesh_edges = 0;
   std::size_t cached_messages = 0;
   std::uint64_t messages_published = 0;
   std::uint64_t messages_received = 0;
   std::uint64_t messages_delivered = 0;
   std::uint64_t duplicates = 0;
   std::uint64_t invalid_messages = 0;
   std::uint64_t control_messages = 0;
   std::uint64_t trace_failures = 0;
   std::uint64_t application_score_failures = 0;
};

struct codec {
   struct received_rpc {
      rpc value;
      std::vector<topic> invalid_messages; // Structurally valid messages rejected by receive policy.
   };
   struct gossip_cursor {
      std::size_t have = 0;
      std::size_t want = 0;
      std::size_t id = 0;
   };
   struct gossip_chunk {
      rpc value;
      std::size_t wire_bytes = 0;
   };

   [[nodiscard]] static protocol_id protocol(version value);
   [[nodiscard]] static std::vector<std::uint8_t> encode(const rpc& value);
   [[nodiscard]] static std::vector<std::uint8_t> encode(const rpc& value, const options& opts);
   [[nodiscard]] static std::size_t control_payload_size(const control& value, const options& opts);
   // One-shot packing; each chunk contains one IHAVE or one normalized IWANT.
   // Individually unencodable entries are skipped without suppressing later IDs.
   [[nodiscard]] static std::optional<gossip_chunk> next_gossip(const control& value, gossip_cursor& cursor,
                                                              const options& opts);
   // Allocation-free framed charge for the same chunks; nullopt exceeds the queue byte bound.
   [[nodiscard]] static std::optional<std::size_t> gossip_wire_size(const control& value, const options& opts);
   [[nodiscard]] static rpc decode(std::span<const std::uint8_t> bytes);
   [[nodiscard]] static rpc decode(std::span<const std::uint8_t> bytes, const options& opts);
   // Applies receive policy during the same structural parse; decode remains wire-only.
   [[nodiscard]] static received_rpc decode_received(std::span<const std::uint8_t> bytes, const options& opts);
   [[nodiscard]] static std::vector<std::uint8_t> encode_message(const message& value);
   [[nodiscard]] static std::vector<std::uint8_t> encode_message(const message& value, const options& opts);
   [[nodiscard]] static std::vector<std::uint8_t> signing_payload(const message& value);
   [[nodiscard]] static std::vector<std::uint8_t> signing_payload(const message& value, const options& opts);
   [[nodiscard]] static std::vector<std::uint8_t> message_id(const message& value);
   [[nodiscard]] static std::vector<std::uint8_t> message_id(const message& value, const options& opts);
   static void sign_message(message& value, const forge::crypto::asymmetric::private_key& key);
   static void sign_message(message& value, const forge::crypto::asymmetric::private_key& key, const options& opts);
   [[nodiscard]] static bool verify_message(const message& value);
   [[nodiscard]] static bool verify_message(const message& value, const options& opts);
};

} // namespace forge::net::p2p::pubsub
