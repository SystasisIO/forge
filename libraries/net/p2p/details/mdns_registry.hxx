#pragma once

#include "interface_state.hxx"
#include "mdns_codec.hxx"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <span>
#include <vector>

import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.mdns_policy;

namespace forge::net::p2p::detail {

// Ordinary private component. All calls are serialized by its owner.
class mdns_registry {
 public:
   using time_point = std::chrono::steady_clock::time_point;
   struct limits {
      std::size_t records = 1024;
      std::size_t bytes = 1024 * 1024;
      std::size_t leases = 4096;
      std::size_t lease_bytes = 1024 * 1024;
   };
   struct lease {
      peer_id peer;
      endpoint address;
      std::uint32_t interface_index = 0;
      std::uint64_t generation = 0;
      time_point expires_at;
   };

   mdns_registry(mdns_policy policy, limits bounds, mdns_codec::name service, peer_id local);
   // Full local snapshot; removed/replaced generations withdraw their records.
   void replace_interfaces(std::span<const interface_state::interface> interfaces);
   void withdraw_interface(std::uint32_t index, std::uint64_t generation);
   // Input is a decoded packet. Retains whole IN PTR/TXT records only; other
   // records cannot establish libp2p hints. Unknown goodbyes never allocate.
   void apply(std::uint32_t index, std::uint64_t generation, const mdns_codec::message& packet, time_point now);
   void expire(time_point now);
   // Output is bounded by limits::leases and lease_bytes, including each
   // interface's separate evidence and copied peer/endpoint string payloads.
   [[nodiscard]] std::vector<lease> snapshot(time_point now) const;
   [[nodiscard]] std::optional<time_point> next_expiry() const;
   [[nodiscard]] std::size_t record_count() const noexcept;
   // Accounted storage includes list links, entries, labels and TXT containers;
   // allocator bookkeeping is excluded. No spare record capacity is retained.
   [[nodiscard]] std::size_t byte_count() const noexcept;

 private:
   struct entry {
      mdns_codec::record record;
      std::uint32_t index;
      std::uint64_t generation;
      time_point received_at;
      time_point expires_at;
      std::size_t bytes;
   };
   mdns_policy _policy;
   limits _bounds;
   mdns_codec::name _service;
   peer_id _local;
   std::map<std::uint32_t, std::uint64_t> _interfaces;
   std::list<entry> _records;
   std::size_t _bytes = 0;
};

} // namespace forge::net::p2p::detail
