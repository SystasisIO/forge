module;

#include <cstddef>
#include <cstdint>
#include <map>
#include <new>
#include <span>
#include <string>
#include <string_view>

module forge.net.p2p.node;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "details/pubsub_idontwant.hxx"

namespace forge::net::p2p::detail {

void pubsub_idontwant::receive(const peer_id& peer, std::span<const pubsub::control::idontwant> values,
                               const pubsub::limits& limits, std::size_t max_peers) noexcept {
   if (values.empty()) { return; }
   try {
      auto found = _peers.find(peer);
      if (found == _peers.end()) {
         if (_peers.size() >= max_peers) { ++_ignored; return; }
         found = _peers.try_emplace(peer).first;
      }
      auto& state = found->second;
      if (state.rpcs >= limits.max_idontwant_rpcs_per_heartbeat) { ++_ignored; return; }
      ++state.rpcs; // One RPC, regardless of the number of repeated IDONTWANT rows.
      auto count = std::size_t{};
      for (const auto& value : values) {
         for (const auto& raw : value.message_ids) {
            if (count == limits.max_idontwant_ids_per_rpc) { ++_ignored; return; }
            ++count;
            if (raw.empty() || raw.size() > limits.max_idontwant_message_id_size) { ++_ignored; continue; }
            const auto id = std::string_view{reinterpret_cast<const char*>(raw.data()), raw.size()};
            if (const auto existing = state.ids.find(id); existing != state.ids.end()) {
               existing->second = limits.idontwant_ttl;
               continue;
            }
            if (state.ids.size() >= limits.max_idontwant_entries_per_peer || _size >= limits.max_idontwant_entries ||
                raw.size() > limits.max_idontwant_bytes_per_peer - state.bytes ||
                raw.size() > limits.max_idontwant_bytes - _bytes) { ++_ignored; continue; }
            state.ids.emplace(std::string{id}, limits.idontwant_ttl);
            state.bytes += raw.size();
            _bytes += raw.size();
            ++_size;
         }
      }
   } catch (const std::bad_alloc&) {
      ++_ignored; // Optional suppression must not invalidate a healthy stream on local pressure.
   }
}

bool pubsub_idontwant::contains(const peer_id& peer, std::string_view id) const noexcept {
   const auto found = _peers.find(peer);
   return found != _peers.end() && found->second.ids.contains(id);
}

void pubsub_idontwant::heartbeat() noexcept {
   for (auto peer = _peers.begin(); peer != _peers.end();) {
      auto& state = peer->second;
      state.rpcs = 0;
      for (auto id = state.ids.begin(); id != state.ids.end();) {
         if (--id->second == 0) {
            state.bytes -= id->first.size();
            _bytes -= id->first.size();
            --_size;
            id = state.ids.erase(id);
         } else { ++id; }
      }
      if (state.ids.empty()) { peer = _peers.erase(peer); }
      else { ++peer; }
   }
}

void pubsub_idontwant::forget(const peer_id& peer) noexcept {
   const auto found = _peers.find(peer);
   if (found == _peers.end()) { return; }
   _size -= found->second.ids.size();
   _bytes -= found->second.bytes;
   _peers.erase(found);
}

void pubsub_idontwant::clear() noexcept { _peers.clear(); _size = 0; _bytes = 0; }
std::size_t pubsub_idontwant::size() const noexcept { return _size; }
std::size_t pubsub_idontwant::bytes() const noexcept { return _bytes; }
std::uint64_t pubsub_idontwant::ignored() const noexcept { return _ignored; }

} // namespace forge::net::p2p::detail
