#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace forge::plugins::net::p2p::node::detail {

class peer_contributions final : public std::enable_shared_from_this<peer_contributions> {
 public:
   peer_contributions();
   ~peer_contributions() noexcept;
   void attach(std::shared_ptr<forge::net::p2p::node> node, const forge::net::p2p::topology::policy& limits);
   [[nodiscard]] peer_contribution protect(forge::net::p2p::peer_id peer);
   [[nodiscard]] peer_contribution tag(forge::net::p2p::peer_id peer, std::int64_t value);
   void close() noexcept;

 private:
   enum class kind { protection, score };
   struct record {
      forge::net::p2p::peer_id cleanup_peer;
      std::string cleanup_key;
      std::uint64_t id = 0;
      std::int64_t value = 0;
      kind type = kind::protection;
      std::atomic_bool active = false;
   };
   struct peer_state {
      std::map<std::uint64_t, std::shared_ptr<record>> records;
      std::uint64_t positive = 0;
      std::uint64_t negative = 0;
   };
   [[nodiscard]] peer_contribution add(forge::net::p2p::peer_id peer, kind type, std::int64_t value);
   void release(const std::shared_ptr<record>& contribution) noexcept;
   void remove_native(record& contribution) noexcept;
   static std::uint64_t magnitude(std::int64_t value) noexcept;

   std::mutex mutex_;
   std::map<forge::net::p2p::peer_id, peer_state> peers_;
   std::shared_ptr<forge::net::p2p::node> node_;
   std::size_t max_peers_ = 0;
   std::size_t max_per_peer_ = 0;
   std::size_t max_key_size_ = 0;
   std::uint64_t next_id_ = 0;
   bool closed_ = false;
};

} // namespace forge::plugins::net::p2p::node::detail
