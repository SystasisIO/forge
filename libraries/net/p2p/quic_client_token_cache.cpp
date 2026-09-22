module;

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.crypto.digest.sha256;

#include "details/quic_client_token_cache.hxx"

namespace forge::net::p2p::direct::detail {
namespace {

constexpr auto token_cache_lifetime = std::chrono::minutes{55};
constexpr auto digest_bytes = std::size_t{forge::crypto::digest::sha256::byte_size};

[[nodiscard]] std::string normalized_host(std::string_view value) {
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value, error);
   if (!error) {
      return address.to_string();
   }
   auto out = std::string{value};
   std::ranges::transform(out, out.begin(),
                          [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
   return out;
}

void append_component(std::string& out, std::string_view value) {
   out += std::to_string(value.size());
   out.push_back(':');
   out.append(value);
}

[[nodiscard]] std::string token_digest(std::span<const std::uint8_t> token) {
   const auto digest = forge::crypto::digest::sha256::hash(token);
   return std::string{digest.data(), digest.data_size()};
}

template <typename Map> [[nodiscard]] typename Map::iterator oldest_entry(Map& values) {
   return std::ranges::min_element(values, [](const auto& left, const auto& right) {
      if (left.second.last_used != right.second.last_used) {
         return left.second.last_used < right.second.last_used;
      }
      return left.first < right.first;
   });
}

} // namespace

quic_client_token_cache::quic_client_token_cache(std::size_t max_entries)
    : max_entries_(std::min(max_entries, std::size_t{1'024})) {}

std::optional<std::vector<std::uint8_t>> quic_client_token_cache::take(std::string_view key) {
   if (key.empty() || key.size() > max_key_bytes) {
      return std::nullopt;
   }
   auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return std::nullopt;
   }
   prune_expired_locked(clock::now());
   const auto found = entries_.find(std::string{key});
   if (found == entries_.end()) {
      return std::nullopt;
   }
   const auto entry_bytes = found->first.size() + found->second.token.size();
   auto token = std::move(found->second.token);
   raw_bytes_ -= entry_bytes;
   entries_.erase(found);
   return token;
}

void quic_client_token_cache::store(std::string key, std::vector<std::uint8_t> token) {
   if (max_entries_ == 0 || key.empty() || key.size() > max_key_bytes || token.empty() ||
       token.size() > max_token_bytes) {
      return;
   }
   const auto entry_bytes = key.size() + token.size();
   if (entry_bytes > max_raw_bytes || digest_bytes > max_raw_bytes - entry_bytes) {
      return;
   }
   const auto digest = token_digest(token);
   auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return;
   }
   const auto now = clock::now();
   prune_expired_locked(now);
   if (tombstones_.contains(digest)) {
      return;
   }
   if (const auto existing = entries_.find(key); existing != entries_.end()) {
      erase_entry_locked(existing);
   }
   make_room_locked(entry_bytes, digest_bytes);
   if (entries_.size() >= max_entries_ || tombstones_.size() >= max_seen_digests ||
       raw_bytes_ > max_raw_bytes - entry_bytes - digest_bytes) {
      return;
   }
   const auto expires_at = now + token_cache_lifetime;
   tombstones_.emplace(digest, tombstone{.expires_at = expires_at, .last_used = next_lru_locked()});
   raw_bytes_ += digest_bytes;
   entries_.emplace(std::move(key),
                    entry{.token = std::move(token), .expires_at = expires_at, .last_used = next_lru_locked()});
   raw_bytes_ += entry_bytes;
}

void quic_client_token_cache::close() noexcept {
   auto lock = std::scoped_lock{mutex_};
   closed_ = true;
   entries_.clear();
   tombstones_.clear();
   raw_bytes_ = 0;
}

std::string quic_client_token_cache::make_key(std::span<const std::uint8_t> expected_peer, std::string_view host_kind,
                                              std::string_view host, std::string_view zone, std::uint16_t port) {
   auto out = std::string{"forge-quic-v1"};
   append_component(out, std::string_view{reinterpret_cast<const char*>(expected_peer.data()), expected_peer.size()});
   append_component(out, host_kind);
   append_component(out, normalized_host(host));
   append_component(out, zone);
   append_component(out, std::to_string(port));
   return out;
}

void quic_client_token_cache::prune_expired_locked(clock::time_point now) const {
   for (auto iterator = entries_.begin(); iterator != entries_.end();) {
      if (iterator->second.expires_at > now) {
         ++iterator;
         continue;
      }
      const auto current = iterator++;
      erase_entry_locked(current);
   }
   for (auto iterator = tombstones_.begin(); iterator != tombstones_.end();) {
      if (iterator->second.expires_at > now) {
         ++iterator;
         continue;
      }
      const auto current = iterator++;
      erase_tombstone_locked(current);
   }
}

void quic_client_token_cache::make_room_locked(std::size_t entry_bytes, std::size_t tombstone_bytes) {
   const auto required_bytes = entry_bytes + tombstone_bytes;
   while (entries_.size() >= max_entries_ || tombstones_.size() >= max_seen_digests ||
          raw_bytes_ > max_raw_bytes - required_bytes) {
      if (!entries_.empty() && (entries_.size() >= max_entries_ || raw_bytes_ > max_raw_bytes - required_bytes)) {
         erase_entry_locked(oldest_entry(entries_));
         continue;
      }
      if (!tombstones_.empty()) {
         erase_tombstone_locked(oldest_entry(tombstones_));
         continue;
      }
      return;
   }
}

void quic_client_token_cache::erase_entry_locked(std::map<std::string, entry>::iterator value) const {
   raw_bytes_ -= value->first.size() + value->second.token.size();
   entries_.erase(value);
}

void quic_client_token_cache::erase_tombstone_locked(std::map<std::string, tombstone>::iterator value) const {
   raw_bytes_ -= digest_bytes;
   tombstones_.erase(value);
}

std::uint64_t quic_client_token_cache::next_lru_locked() const noexcept {
   if (lru_ == (std::numeric_limits<std::uint64_t>::max)()) {
      lru_ = 0;
      for (auto& [_, entry] : entries_) {
         entry.last_used = ++lru_;
      }
      for (auto& [_, tombstone] : tombstones_) {
         tombstone.last_used = ++lru_;
      }
   }
   return ++lru_;
}

} // namespace forge::net::p2p::direct::detail
