#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace forge::net::p2p::direct::detail {

class quic_client_token_cache {
 public:
   explicit quic_client_token_cache(std::size_t max_entries);

   quic_client_token_cache(const quic_client_token_cache&) = delete;
   quic_client_token_cache& operator=(const quic_client_token_cache&) = delete;

   [[nodiscard]] std::optional<std::vector<std::uint8_t>> take(std::string_view key);
   void store(std::string key, std::vector<std::uint8_t> token);
   void close() noexcept;

   [[nodiscard]] static std::string make_key(std::span<const std::uint8_t> expected_peer, std::string_view host_kind,
                                             std::string_view host, std::string_view zone, std::uint16_t port);

 private:
   using clock = std::chrono::steady_clock;

   struct entry {
      std::vector<std::uint8_t> token;
      clock::time_point expires_at;
      std::uint64_t last_used = 0;
   };

   struct tombstone {
      clock::time_point expires_at;
      std::uint64_t last_used = 0;
   };

   void prune_expired_locked(clock::time_point now) const;
   void make_room_locked(std::size_t entry_bytes, std::size_t tombstone_bytes);
   void erase_entry_locked(std::map<std::string, entry>::iterator value) const;
   void erase_tombstone_locked(std::map<std::string, tombstone>::iterator value) const;
   [[nodiscard]] std::uint64_t next_lru_locked() const noexcept;

   static constexpr auto max_token_bytes = std::size_t{512};
   static constexpr auto max_key_bytes = std::size_t{512};
   static constexpr auto max_raw_bytes = std::size_t{1 * 1024 * 1024};
   static constexpr auto max_seen_digests = std::size_t{2'048};

   std::size_t max_entries_ = 0;
   mutable std::mutex mutex_;
   mutable std::map<std::string, entry> entries_;
   mutable std::map<std::string, tombstone> tombstones_;
   mutable std::size_t raw_bytes_ = 0;
   mutable std::uint64_t lru_ = 0;
   bool closed_ = false;
};

} // namespace forge::net::p2p::direct::detail
