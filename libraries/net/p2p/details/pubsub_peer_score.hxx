#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/asio/ip/address.hpp>

namespace forge::net::p2p::detail {

// Node-owned, bounded score accounting; no callback, executor, clock source or persistence.
class pubsub_peer_score {
 public:
   using clock = std::chrono::steady_clock;

   struct validation {
      std::vector<std::uint8_t> message_id;
      std::uint64_t generation = 0;
   };

   explicit pubsub_peer_score(pubsub::scoring_params params, clock::time_point now);

   // Only actual direct remote IPs belong here. A circuit has no inner socket IP: pass an empty span.
   // Every live peer reserves its retention slot. False means admission/attribution was refused;
   // callers must not treat refused validation work as a scored/accepted message.
   [[nodiscard]] bool connect(const peer_id& peer, std::span<const boost::asio::ip::address> ips,
                              clock::time_point now);
   [[nodiscard]] bool update_ips(const peer_id& peer, std::span<const boost::asio::ip::address> ips,
                                 clock::time_point now);
   void remove_ip(const peer_id& peer, const boost::asio::ip::address& ip, clock::time_point now);
   [[nodiscard]] bool disconnect(const peer_id& peer, clock::time_point now);
   [[nodiscard]] bool graft(const peer_id& peer, const pubsub::topic& subject, clock::time_point now);
   [[nodiscard]] bool graft(const peer_id& peer, std::string_view subject, clock::time_point now);
   [[nodiscard]] bool prune(const peer_id& peer, const pubsub::topic& subject, clock::time_point now);
   [[nodiscard]] bool prune(const peer_id& peer, std::string_view subject, clock::time_point now);
   [[nodiscard]] bool set_application_score(const peer_id& peer, double sample, clock::time_point now);
   [[nodiscard]] bool add_behaviour_penalty(const peer_id& peer, double amount, clock::time_point now);

   [[nodiscard]] std::optional<validation> validation_start(const peer_id& source, const pubsub::topic& subject,
                                                           std::span<const std::uint8_t> id,
                                                           clock::time_point now);
   // Accept/reject/ignore are terminal and idempotent. Retry keeps pending attribution, without P4.
   // Expired or replaced generations are never recreated by a late completion.
   [[nodiscard]] bool validation_complete(const validation& ticket, pubsub::validation_result result,
                                         clock::time_point now);
   [[nodiscard]] bool validation_complete(std::span<const std::uint8_t> id, std::uint64_t generation,
                                         pubsub::validation_result result, clock::time_point now);
   [[nodiscard]] bool duplicate_delivery(const peer_id& source, const pubsub::topic& subject,
                                        std::span<const std::uint8_t> id, clock::time_point now);
   // Structural/signature rejection before application validation: only the actual sender is penalized.
   [[nodiscard]] bool reject_invalid(const peer_id& source, const pubsub::topic& subject, clock::time_point now);

   // Supplied observations are clamped to the logical maximum under the engine mutex. Tick catches up intervals
   // without an unbounded loop; disconnected counters stay frozen until reconnect/expiry.
   void tick(clock::time_point now);
   [[nodiscard]] double score(const peer_id& peer, clock::time_point now);
   [[nodiscard]] std::optional<pubsub::peer_score_snapshot> inspect(const peer_id& peer, clock::time_point now);
   [[nodiscard]] pubsub::score_snapshot snapshot(clock::time_point now);

 private:
   using peer_expiry_index = std::multimap<clock::time_point, peer_id>;
   using delivery_expiry_index = std::multimap<clock::time_point, std::vector<std::uint8_t>>;
   using ip_index = std::map<boost::asio::ip::address, std::size_t>;

   struct message_less {
      using is_transparent = void;
      [[nodiscard]] bool operator()(std::span<const std::uint8_t> left,
                                    std::span<const std::uint8_t> right) const noexcept;
   };

   struct topic_state {
      bool in_mesh = false;
      bool mesh_active = false;
      clock::time_point grafted{};
      clock::duration mesh_time{};
      double first_deliveries = 0.0;
      double mesh_deliveries = 0.0;
      double mesh_failure = 0.0;
      double invalid_deliveries = 0.0;
   };

   struct topic_less {
      using is_transparent = void;
      [[nodiscard]] bool operator()(const pubsub::topic& left, const pubsub::topic& right) const noexcept;
      [[nodiscard]] bool operator()(const pubsub::topic& left, std::string_view right) const noexcept;
      [[nodiscard]] bool operator()(std::string_view left, const pubsub::topic& right) const noexcept;
   };

   struct peer_state {
      bool connected = true;
      std::uint64_t generation = 0;
      clock::time_point retain_until{};
      peer_expiry_index::iterator expiry;
      double application_score = 0.0;
      double behaviour_penalty = 0.0;
      std::vector<boost::asio::ip::address> ips;
      std::map<pubsub::topic, topic_state, topic_less> topics;
   };

   struct peer_reference {
      peer_id peer;
      std::uint64_t generation = 0;
   };

   enum class delivery_status { pending, valid, invalid, ignored };

   struct delivery_record {
      std::uint64_t generation = 0;
      pubsub::topic subject;
      peer_reference first;
      bool first_pending_duplicate = false;
      delivery_status status = delivery_status::pending;
      clock::time_point expires{};
      clock::time_point validated{};
      delivery_expiry_index::iterator expiry;
      std::vector<peer_reference> duplicates;
   };

   struct score_parts {
      double topics = 0.0;
      double application = 0.0;
      double ip_factor = 0.0;
      double ip_score = 0.0;
      double behaviour = 0.0;
      double total = 0.0;
   };

   [[nodiscard]] static clock::time_point deadline(clock::time_point now, std::chrono::milliseconds duration);
   [[nodiscard]] static clock::duration elapsed(clock::time_point from, clock::time_point to);
   [[nodiscard]] static double finite_value(long double value);
   [[nodiscard]] static double add(double left, double right);
   [[nodiscard]] static double weighted(double value, double weight);
   [[nodiscard]] static double squared(double value);
   [[nodiscard]] static double weighted_square(double value, double weight);
   [[nodiscard]] static boost::asio::ip::address canonical_ip(const boost::asio::ip::address& address);
   [[nodiscard]] std::optional<std::vector<boost::asio::ip::address>> prepare_ips(
       std::span<const boost::asio::ip::address> ips) const;
   [[nodiscard]] ip_index prepare_ip_entries(std::span<const boost::asio::ip::address> ips) const;
   void add_ips(std::span<const boost::asio::ip::address> ips);
   void remove_ips(std::span<const boost::asio::ip::address> ips);
   void replace_ips(peer_state& state, std::vector<boost::asio::ip::address> ips);
   void schedule_retention(peer_state& state, clock::time_point until);
   [[nodiscard]] bool valid_topic(const pubsub::topic& subject) const;
   void advance(clock::time_point& now);
   void capacity_rejected();
   void leave_mesh(topic_state& state, const pubsub::topic_score_params& params);
   void mark_delivery(const peer_id& peer, std::uint64_t generation, const pubsub::topic& subject,
                      bool first, bool invalid);
   [[nodiscard]] double topic_score(const topic_state& state, const pubsub::topic_score_params& params) const;
   [[nodiscard]] double ip_factor(const peer_state& state) const;
   [[nodiscard]] score_parts calculate(const peer_state& state) const;
   [[nodiscard]] double score_locked(const peer_state& state) const;
   [[nodiscard]] pubsub::peer_score_snapshot inspect_locked(const peer_id& peer, const peer_state& state) const;

   const pubsub::scoring_params _params;
   mutable std::mutex _mutex;
   std::map<peer_id, peer_state> _peers;
   std::map<std::vector<std::uint8_t>, delivery_record, message_less> _deliveries;
   peer_expiry_index _peer_expiry;
   delivery_expiry_index _delivery_expiry;
   ip_index _ip_peers;
   clock::time_point _last_now;
   clock::time_point _last_decay;
   std::uint64_t _next_peer = 1;
   std::uint64_t _next_delivery = 1;
   std::uint64_t _capacity_rejections = 0;
   std::size_t _connected = 0;
};

} // namespace forge::net::p2p::detail
