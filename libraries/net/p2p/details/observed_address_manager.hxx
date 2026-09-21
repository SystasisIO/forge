#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace forge::net::p2p::detail {

// Runtime-free observation state. Callers supply authenticated connection
// endpoints and resolved listen addresses, and remove disconnected sessions.
class observed_address_manager final {
   using time_point = std::chrono::steady_clock::time_point;

 public:
   struct options {
      std::size_t max_observations = 1024;
      std::size_t max_candidates = 256;
      std::size_t min_observers = 4;
      std::size_t max_confirmed_per_local = 3;
      std::chrono::steady_clock::duration ttl = std::chrono::minutes{10};
   };

   observed_address_manager();
   explicit observed_address_manager(options value);

   // false rejects only this update; a prior valid session observation remains.
   // Public/private numeric addresses are eligible, but quorum is not an
   // Internet-reachability verdict. Returned addresses carry no peer suffix.
   // Candidate capacity counts (actual local, reported) pairs. A same-family
   // wildcard listener covers the actual local IP only at its listening port.
   // A QUIC session may instead retain its socket's wildcard bind; this key
   // requires the exact owned wildcard listener and is never an invented local IP.
   [[nodiscard]] bool observe(std::uint64_t session_id, const peer_id& authenticated_peer,
                              const endpoint& local, const endpoint& remote, const endpoint& reported,
                              std::span<const endpoint> listened, time_point now);
   void remove(std::uint64_t session_id);
   void expire(time_point now);
   [[nodiscard]] std::vector<endpoint> confirmed(time_point now) const;

 private:
   using address_key = std::vector<std::uint8_t>;
   using candidate_key = std::pair<address_key, address_key>;

   struct observation {
      std::uint64_t session_id;
      std::string peer;
      std::string observer;
      candidate_key key;
      endpoint reported;
      time_point expires_at;
   };

   struct ranked_candidate {
      const candidate_key* key;
      const endpoint* address;
      std::size_t observers;
   };

   [[nodiscard]] static std::size_t count_observers(const std::vector<const observation*>& values);
   void expire_locked(time_point now);

   const options options_;
   mutable std::mutex mutex_;
   std::vector<observation> observations_;
};

} // namespace forge::net::p2p::detail
