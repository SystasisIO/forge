#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace forge::net::p2p::detail {

class reachability_state {
 public:
   struct candidate_snapshot {
      std::uint64_t generation = 0;
      std::vector<endpoint> addresses;
   };

   explicit reachability_state(reachability_policy policy);
   [[nodiscard]] bool record_v1(const peer_id& observer, const endpoint& remote,
                                std::uint64_t generation, reachability::state value, bool internet_scope,
                                std::chrono::steady_clock::time_point now);
   [[nodiscard]] bool record_v2(const peer_id& observer, const endpoint& remote,
                                std::uint64_t generation, endpoint address, reachability::state value, bool verified_dialback,
                                std::chrono::steady_clock::time_point now);
   [[nodiscard]] std::uint64_t set_addresses(std::span<const endpoint> current);
   [[nodiscard]] candidate_snapshot candidates() const;
   void invalidate() noexcept;
   void close() noexcept;
   void expire(std::chrono::steady_clock::time_point now);
   [[nodiscard]] host_event snapshot(std::chrono::steady_clock::time_point now) const;

 private:
   struct observation {
      peer_id peer;
      std::string group;
      std::optional<endpoint> address;
      reachability::state value;
      std::chrono::steady_clock::time_point expires_at;
   };

   bool record(const peer_id& observer, const endpoint& remote, std::optional<endpoint> address,
               std::uint64_t generation, reachability::state value, std::chrono::steady_clock::time_point now);
   [[nodiscard]] reachability::state aggregate(const std::optional<endpoint>& address,
                                               std::chrono::steady_clock::time_point now) const;

   reachability_policy policy_;
   mutable std::mutex mutex_;
   std::vector<observation> observations_;
   std::vector<endpoint> addresses_;
   std::uint64_t address_generation_ = 0;
   bool closed_ = false;
};

} // namespace forge::net::p2p::detail
