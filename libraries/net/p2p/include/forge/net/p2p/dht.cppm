module;

#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

export module forge.net.p2p.dht;

import forge.multiformats.multiaddr;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;

export namespace forge::net::p2p::dht {
enum class mode : std::uint16_t {
   client = 0,
   server = 1,
};

enum class message_type : std::uint16_t {
   put_value = 0,
   get_value = 1,
   add_provider = 2,
   get_providers = 3,
   find_node = 4,
   ping = 5,
};

enum class connection_type : std::uint16_t {
   not_connected = 0,
   connected = 1,
   can_connect = 2,
   cannot_connect = 3,
};

struct options {
   mode operating_mode = mode::client;
   std::size_t replication = 20;
   std::size_t alpha = 10;
   std::size_t max_outbound_message_size = 1024 * 1024;
   std::size_t max_inbound_message_size = 4 * 1024 * 1024;
   std::size_t max_record_size = 1024 * 1024;
   std::size_t max_closer_peers = 20;
   std::size_t max_provider_peers = 20;
   std::size_t max_peer_endpoints = 64;
   std::size_t max_query_peers = 256;
   std::size_t replacement_cache_size = 20;
   std::size_t failure_threshold = 3;
   std::chrono::milliseconds query_timeout{10'000};
   std::chrono::milliseconds refresh_interval{600'000};
   std::chrono::seconds value_record_ttl{172'800};
   std::chrono::seconds provider_record_ttl{172'800};
   std::chrono::seconds provider_address_ttl{86'400};
   std::chrono::seconds provider_republish_interval{79'200};
};

struct key {
   std::vector<std::uint8_t> bytes;

   [[nodiscard]] friend bool operator==(const key&, const key&) noexcept = default;
   [[nodiscard]] friend auto operator<=>(const key&, const key&) noexcept = default;
};

struct distance {
   std::array<std::uint8_t, 32> bytes{};

   [[nodiscard]] friend bool operator==(const distance&, const distance&) noexcept = default;
   [[nodiscard]] friend auto operator<=>(const distance&, const distance&) noexcept = default;
};

struct record {
   key key_value;
   std::vector<std::uint8_t> value;
   std::string time_received;
   std::optional<peer_id> publisher;
   std::chrono::seconds ttl{0};
};

struct peer {
   peer_id id;
   std::vector<forge::multiformats::multiaddr> endpoints;
   connection_type connection = connection_type::not_connected;
};

struct message {
   message_type type = message_type::find_node;
   std::int32_t cluster_level_raw = 0;
   key key_value;
   std::optional<record> record_value;
   std::vector<peer> closer_peers;
   std::vector<peer> provider_peers;
};

struct query_result {
   key target;
   std::vector<peer> closest_peers;
   std::vector<peer> provider_peers;
   std::optional<record> record_value;
   bool complete = false;
};

struct query_options {
   std::size_t requested_count = 20;
   std::size_t quorum = 1;
   std::chrono::milliseconds timeout{10'000};
};

struct value_put_result {
   record selected;
   std::size_t accepted = 0;
   std::size_t attempted = 0;
   bool quorum_reached = false;
};

struct value_get_result {
   std::optional<record> selected;
   std::size_t responses = 0;
   std::size_t valid_records = 0;
   bool quorum_reached = false;
};

enum class routing_admission : std::uint8_t {
   candidate,
   verified_server,
};

struct routing_status {
   std::size_t active = 0;
   std::size_t replacements = 0;
   std::size_t candidates = 0;
   std::size_t nonempty_buckets = 0;
};

struct routing_refresh_bucket {
   std::size_t common_prefix_length = 0;
   std::uint64_t generation = 0;

   [[nodiscard]] friend bool operator==(const routing_refresh_bucket&,
                                        const routing_refresh_bucket&) noexcept = default;
};

enum class profile_kind : std::uint8_t {
   amino_v1,
   custom,
};

struct profile_capabilities {
   bool peers = true;
   bool providers = false;
   bool values = false;
};

struct value_validation_context {
   std::chrono::system_clock::time_point now{};
   const public_key_resolver* public_keys = nullptr;
};

struct value_expiry_context {
   std::chrono::system_clock::time_point now{};
   std::chrono::system_clock::time_point supplied_expires_at{};
};

struct value_policy {
   std::vector<std::uint8_t> key_prefix;
   std::function<void(const record&, value_validation_context)> validate;
   std::function<std::size_t(std::span<const record>)> select;
   std::function<std::chrono::system_clock::time_point(const record&, value_expiry_context)> expiry;
};

struct profile {
   protocol_id protocol;
   profile_kind kind = profile_kind::custom;
   mode operating_mode = mode::client;
   profile_capabilities capabilities;
   options limits;
   std::vector<value_policy> value_policies;
};

class routing_table;

struct codec {
   [[nodiscard]] static std::vector<std::uint8_t> encode(const message& value);
   [[nodiscard]] static std::vector<std::uint8_t> encode(const message& value, const options& opts);
   [[nodiscard]] static std::vector<std::uint8_t> encode(const message& value, const profile& profile_value);
   [[nodiscard]] static message decode(std::span<const std::uint8_t> bytes);
   [[nodiscard]] static message decode(std::span<const std::uint8_t> bytes, const options& opts);
   [[nodiscard]] static message decode(std::span<const std::uint8_t> bytes, const profile& profile_value);
};
} // namespace forge::net::p2p::dht

export namespace forge::net::p2p {

class dht::routing_table {
 public:
   routing_table(peer_id local_peer, dht::options options_value = {});
   ~routing_table();

   routing_table(const routing_table&) = delete;
   routing_table& operator=(const routing_table&) = delete;

   routing_table(routing_table&&) noexcept;
   routing_table& operator=(routing_table&&) noexcept;

   void upsert(peer value, routing_admission admission = routing_admission::verified_server);
   void remove(const peer_id& peer);
   void mark_failure(const peer_id& peer);
   [[nodiscard]] std::vector<peer> closest(std::span<const std::uint8_t> target, std::size_t limit) const;
   [[nodiscard]] std::vector<peer> query_seeds(std::span<const std::uint8_t> target, std::size_t limit) const;
   [[nodiscard]] std::vector<peer> snapshot() const;
   [[nodiscard]] routing_status status() const;
   [[nodiscard]] std::vector<routing_refresh_bucket> plan_refresh(std::chrono::steady_clock::time_point now,
                                                                  std::chrono::milliseconds stale_after) const;
   [[nodiscard]] bool mark_refreshed(routing_refresh_bucket bucket, std::chrono::steady_clock::time_point refreshed_at);

 private:
   struct impl;
   std::unique_ptr<impl> impl_;
};

[[nodiscard]] dht::key make_dht_key(std::span<const std::uint8_t> value);
[[nodiscard]] dht::key make_dht_key(const peer_id& peer);
[[nodiscard]] dht::distance distance_between(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right);
[[nodiscard]] dht::profile amino_v1(dht::mode operating_mode = dht::mode::client);
[[nodiscard]] dht::profile custom_dht_profile(protocol_id protocol, dht::mode operating_mode,
                                              dht::profile_capabilities capabilities,
                                              std::vector<dht::value_policy> value_policies = {},
                                              dht::options limits = {});
// Checks declared protocol, capabilities and limits before application value
// policies are supplied. A usable profile must still pass validate().
void validate_declaration(const dht::profile& profile);
void validate(const dht::profile& profile);
[[nodiscard]] const dht::value_policy* value_policy_for(const dht::profile& profile,
                                                        std::span<const std::uint8_t> key) noexcept;

} // namespace forge::net::p2p
