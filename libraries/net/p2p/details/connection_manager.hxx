#pragma once

namespace forge::net::p2p {

class connection_manager {
 public:
   enum class direction { inbound, outbound };

   struct policy {
      std::size_t max_sessions = 1024;
      std::size_t low_watermark = 1024;
      std::size_t max_tagged_peers = 1024;
      std::size_t max_tags_per_peer = 16;
      std::size_t max_tag_size = 128;
      std::chrono::milliseconds grace_period{60'000};
      std::chrono::milliseconds prune_silence{10'000};
   };

   struct session_record {
      std::uint64_t id = 0;
      peer_id peer;
      direction direction = direction::outbound;
      std::chrono::steady_clock::time_point opened_at{};
      std::chrono::steady_clock::time_point last_used_at{};
      double network_score = 0.0;
   };

   struct admission {
      bool accepted = false;
      std::vector<std::uint64_t> pruned;
      std::string reason;
   };

   struct snapshot {
      std::size_t active_sessions = 0;
      std::size_t active_peers = 0;
      std::vector<peer_id> protected_peers;
      std::vector<session_record> sessions;
   };

   struct peer_prune_plan {
      std::size_t connected_peers = 0;
      std::vector<peer_id> victim_peers;
      std::vector<std::uint64_t> session_ids;
   };

   explicit connection_manager(policy value);

   [[nodiscard]] const policy& configured_policy() const noexcept;
   void protect(const peer_id& peer, std::string tag);
   [[nodiscard]] bool unprotect(const peer_id& peer, std::string_view tag);
   [[nodiscard]] bool is_protected(const peer_id& peer) const;
   void tag(const peer_id& peer, std::string tag, std::int64_t value);
   [[nodiscard]] bool untag(const peer_id& peer, std::string_view tag);
   [[nodiscard]] std::int64_t aggregate_tag_value(const peer_id& peer) const;
   void update_network_score(const peer_id& peer, double score);
   [[nodiscard]] peer_prune_plan plan_peer_prune(std::size_t target_peers, std::size_t max_victims,
                                                 std::chrono::steady_clock::time_point now) const;
   [[nodiscard]] admission remember(session_record record, std::chrono::steady_clock::time_point now);
   void forget(std::uint64_t id);
   void forget_peer(const peer_id& peer);
   void touch(std::uint64_t id, std::chrono::steady_clock::time_point now);
   void clear();
   [[nodiscard]] snapshot current(std::size_t max_sessions) const;
   [[nodiscard]] std::size_t size() const noexcept;
   // Private test seam; callers use the same owner lock as remember().
   void fail_next_prepare_for_test() noexcept;

 private:
   struct peer_prune_candidate {
      peer_id peer;
      std::int64_t tag_value = 0;
      double network_score = 0.0;
      std::chrono::steady_clock::time_point opened_at{};
      std::chrono::steady_clock::time_point last_used_at{};
   };

   [[nodiscard]] std::optional<std::uint64_t>
   select_prune_one(const std::vector<std::uint64_t>& selected, std::chrono::steady_clock::time_point now,
                    std::optional<direction> required_direction = std::nullopt) const;
   [[nodiscard]] bool should_prune_before(const session_record& left, const session_record& right) const;
   void erase_record(std::uint64_t id);

   policy policy_;
   std::map<peer_id, std::set<std::string>> protected_;
   std::map<peer_id, std::map<std::string, std::int64_t>> tags_;
   std::map<std::uint64_t, session_record> sessions_;
   std::map<peer_id, std::set<std::uint64_t>> sessions_by_peer_;
   std::map<peer_id, double> network_scores_;
   std::optional<std::chrono::steady_clock::time_point> last_prune_;
   bool fail_next_prepare_for_test_ = false;
};

[[nodiscard]] connection_manager::policy connection_policy_for(const node::limits& limits);

} // namespace forge::net::p2p
