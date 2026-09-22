#include "details/mdns_registry.hxx"

#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <forge/exceptions/macros.hpp>
#include <limits>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;

namespace forge::net::p2p::detail {
namespace {

namespace codec = mdns_codec;
using time_point = mdns_registry::time_point;

bool same_name(const codec::name& a, const codec::name& b) {
   const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
   return std::ranges::equal(a, b, [&](const auto& x, const auto& y) {
      return std::ranges::equal(x, y, [&](unsigned char l, unsigned char r) { return fold(l) == fold(r); });
   });
}

codec::name folded_name(codec::name name) {
   for (auto& label : name) {
      for (auto& ch : label) {
         if (ch >= 'A' && ch <= 'Z') {
            ch += 'a' - 'A';
         }
      }
   }
   return name;
}

bool supported(const codec::record& r) {
   return (r.class_code & codec::class_mask) == 1 &&
          ((r.type == codec::type_ptr && std::holds_alternative<codec::ptr>(r.data)) ||
           (r.type == codec::type_txt && std::holds_alternative<codec::txt>(r.data)));
}

bool same_set(const codec::record& a, const codec::record& b) {
   return a.type == b.type && (a.class_code & codec::class_mask) == (b.class_code & codec::class_mask) &&
          same_name(a.owner, b.owner);
}

bool same_record(const codec::record& a, const codec::record& b) {
   if (!same_set(a, b)) {
      return false;
   }
   if (const auto* p = std::get_if<codec::ptr>(&a.data)) {
      const auto* q = std::get_if<codec::ptr>(&b.data);
      return q && same_name(p->target, q->target);
   }
   return a.data == b.data;
}

time_point deadline(time_point now, std::uint32_t seconds) {
   const auto duration = std::chrono::duration_cast<time_point::duration>(std::chrono::seconds{seconds});
   return now > time_point::max() - duration ? time_point::max() : now + duration;
}

std::optional<std::size_t> charge(const codec::record& record, std::size_t base, std::size_t limit) {
   auto total = base;
   const auto add = [&](std::size_t size) {
      if (total > limit || size > limit - total) {
         return false;
      }
      total += size;
      return true;
   };
   const auto name = [&](const codec::name& value) {
      for (const auto& label : value) {
         if (!add(sizeof(std::string)) || !add(label.size()) || !add(1)) {
            return false;
         }
      }
      return true;
   };
   if (!name(record.owner)) {
      return std::nullopt;
   }
   if (const auto* ptr = std::get_if<codec::ptr>(&record.data)) {
      if (!name(ptr->target)) {
         return std::nullopt;
      }
   } else if (const auto* txt = std::get_if<codec::txt>(&record.data)) {
      for (const auto& attr : txt->attributes) {
         if (!add(sizeof(codec::bytes)) || !add(attr.size())) {
            return std::nullopt;
         }
      }
   }
   if (total > limit) {
      return std::nullopt;
   }
   return total;
}

} // namespace

mdns_registry::mdns_registry(mdns_policy policy, limits bounds, codec::name service, peer_id local)
    : _policy(policy), _bounds(bounds), _service(std::move(service)), _local(std::move(local)) {
   validate(policy);
   if (!bounds.records || !bounds.bytes || !bounds.leases || !bounds.lease_bytes || _service.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid mDNS registry bounds or service");
   }
}

void mdns_registry::replace_interfaces(std::span<const interface_state::interface> interfaces) {
   auto next = std::map<std::uint32_t, std::uint64_t>{};
   for (const auto& iface : interfaces) {
      if (!iface.index || !iface.generation || !iface.up || !iface.multicast || iface.loopback) {
         continue;
      }
      if (next.size() == _policy.max_interfaces) {
         break;
      }
      next.emplace(iface.index, iface.generation);
   }
   _interfaces = std::move(next);
   std::erase_if(_records, [&](const entry& e) {
      const auto found = _interfaces.find(e.index);
      if (found != _interfaces.end() && found->second == e.generation) {
         return false;
      }
      _bytes -= e.bytes;
      return true;
   });
}

void mdns_registry::withdraw_interface(std::uint32_t index, std::uint64_t generation) {
   const auto found = _interfaces.find(index);
   if (found == _interfaces.end() || found->second != generation) {
      return;
   }
   _interfaces.erase(found);
   std::erase_if(_records, [&](const entry& e) {
      if (e.index != index || e.generation != generation) {
         return false;
      }
      _bytes -= e.bytes;
      return true;
   });
}

void mdns_registry::expire(time_point now) {
   std::erase_if(_records, [&](const entry& e) {
      if (e.expires_at > now) {
         return false;
      }
      _bytes -= e.bytes;
      return true;
   });
}

void mdns_registry::apply(std::uint32_t index, std::uint64_t generation, const codec::message& packet, time_point now) {
   expire(now);
   const auto iface = _interfaces.find(index);
   if (iface == _interfaces.end() || iface->second != generation || !(packet.head.flags & 0x8000)) {
      return;
   }
   auto remaining = _policy.max_records_per_packet;
   for (const auto* section : {&packet.answers, &packet.authorities, &packet.additionals}) {
      if (section->size() > remaining) {
         return;
      }
      remaining -= section->size();
   }
   // Examine the whole received RRset before changing it. A later member in
   // this packet must not be flushed by an earlier member.
   for (auto& e : _records) {
      if (e.index != index || e.generation != generation || deadline(e.received_at, 1) > now) {
         continue;
      }
      auto flush = false;
      auto present = false;
      for (const auto* section : {&packet.answers, &packet.authorities, &packet.additionals}) {
         for (const auto& r : *section) {
            if (!supported(r) || !r.ttl || !same_set(e.record, r)) {
               continue;
            }
            // Service PTRs are shared records: one responder cannot flush
            // another responder's announcement (RFC6762 section 10.2).
            flush |= r.type != codec::type_ptr && (r.class_code & codec::class_high_bit) != 0;
            present |= same_record(e.record, r);
         }
      }
      if (flush && !present) {
         e.expires_at = std::min(e.expires_at, deadline(now, 1));
      }
   }
   for (const auto* section : {&packet.answers, &packet.authorities, &packet.additionals}) {
      for (const auto& r : *section) {
         if (!supported(r)) {
            continue;
         }
         const auto found = std::ranges::find_if(_records, [&](const entry& e) {
            return e.index == index && e.generation == generation && same_record(e.record, r);
         });
         if (found != _records.end()) {
            found->expires_at = r.ttl ? deadline(now, r.ttl) : std::min(found->expires_at, deadline(now, 1));
            if (r.ttl) {
               found->received_at = now;
            }
            continue;
         }
         if (!r.ttl || _records.size() >= _bounds.records) {
            continue;
         }
         const auto bytes = charge(r, sizeof(entry) + 2 * sizeof(void*), _bounds.bytes - _bytes);
         if (!bytes) {
            continue;
         }
         _records.push_back({r, index, generation, now, deadline(now, r.ttl), *bytes});
         _bytes += *bytes;
      }
   }
}

std::vector<mdns_registry::lease> mdns_registry::snapshot(time_point now) const {
   auto out = std::vector<lease>{};
   auto addresses = std::map<peer_id, std::set<std::string>>{};
   using owner_key = std::tuple<std::uint32_t, std::uint64_t, codec::name>;
   auto owners = std::map<owner_key, time_point>{};
   for (const auto& p : _records) {
      const auto* ptr = std::get_if<codec::ptr>(&p.record.data);
      if (!ptr || ptr->target.empty() || p.expires_at <= now || !same_name(p.record.owner, _service)) {
         continue;
      }
      // The service PTR owner defines the namespace. Rust uses a single-label
      // target; other implementations use full names. Correlate either with
      // the exact TXT owner in the same interface generation.
      auto target = folded_name(ptr->target);
      const auto [found, inserted] = owners.emplace(owner_key{p.index, p.generation, std::move(target)}, p.expires_at);
      if (!inserted) {
         found->second = std::max(found->second, p.expires_at);
      }
   }
   auto output_bytes = std::size_t{};
   auto emitted = std::map<std::tuple<std::uint32_t, std::uint64_t, std::string>, std::size_t>{};
   // Each TXT record is visited once regardless of PTR duplication. Index
   // sizes are bounded by the record cache and output lease limits.
   for (const auto& t : _records) {
      const auto* txt = std::get_if<codec::txt>(&t.record.data);
      if (!txt || t.expires_at <= now) {
         continue;
      }
      const auto owner = owners.find(owner_key{t.index, t.generation, folded_name(t.record.owner)});
      if (owner == owners.end()) {
         continue;
      }
      for (const auto& attr : txt->attributes) {
         constexpr std::string_view prefix = "dnsaddr=";
         if (attr.size() <= prefix.size() || attr.size() > 255) {
            continue;
         }
         const auto text = std::string_view{reinterpret_cast<const char*>(attr.data()), attr.size()};
         if (!text.starts_with(prefix) || text.find('\0') != std::string_view::npos) {
            continue;
         }
         try {
            const auto raw = forge::multiformats::multiaddr::parse(text.substr(prefix.size()));
            if (std::ranges::any_of(raw.components(), [](const auto& c) {
                   return c.code == forge::multiformats::protocol_code::ip6zone;
                })) {
               continue;
            }
            auto address = parse_endpoint(raw.to_string());
            if (!address.peer || *address.peer == _local || address.relayed ||
                address.encapsulation != endpoint::encapsulation_kind::none ||
                (!address.is_direct_tcp() && !address.is_direct_quic()) || !address.transport.port ||
                (address.transport.host_type != endpoint::host_kind::ip4 &&
                 address.transport.host_type != endpoint::host_kind::ip6)) {
               continue;
            }
            boost::system::error_code error;
            const auto ip = boost::asio::ip::make_address(address.transport.host, error);
            if (error || ip.is_unspecified() || ip.is_multicast() || ip.is_loopback()) {
               continue;
            }
            if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
               const auto bytes = ip.to_v6().to_bytes();
               const auto v4 = boost::asio::ip::address_v4{
                   boost::asio::ip::address_v4::bytes_type{bytes[12], bytes[13], bytes[14], bytes[15]}};
               if (v4.is_unspecified() || v4.is_multicast() || v4.is_loopback()) {
                  continue;
               }
            }
            if ((ip.is_v4() && !_policy.ipv4_enabled) || (ip.is_v6() && !_policy.ipv6_enabled)) {
               continue;
            }
            if (ip.is_v6() && ip.to_v6().is_link_local()) {
               address.transport.zone = std::to_string(t.index);
            }
            const auto key = address.to_string();
            const auto expiry = std::min(owner->second, t.expires_at);
            const auto route_key = std::tuple{t.index, t.generation, key};
            if (const auto existing = emitted.find(route_key); existing != emitted.end()) {
               auto& value = out[existing->second];
               value.expires_at = std::max(value.expires_at, expiry);
               continue;
            }
            // TXT attributes are <=255 bytes; all derived strings below
            // are correspondingly bounded before this sum is evaluated.
            const auto bytes = sizeof(lease) + address.peer->value.size() * 2 + address.transport.host.size() +
                               address.transport.zone.size() + 4;
            if (out.size() == _bounds.leases || bytes > _bounds.lease_bytes - output_bytes) {
               continue;
            }
            auto peer = addresses.find(*address.peer);
            if (peer == addresses.end()) {
               if (addresses.size() == _policy.max_peers) {
                  continue;
               }
               peer = addresses.emplace(*address.peer, std::set<std::string>{}).first;
            }
            if (!peer->second.contains(key) && peer->second.size() == _policy.max_addresses_per_peer) {
               continue;
            }
            peer->second.insert(key);
            emitted.emplace(route_key, out.size());
            out.push_back({*address.peer, std::move(address), t.index, t.generation, expiry});
            output_bytes += bytes;
         } catch (const forge::exceptions::base&) {
            // Each TXT attribute is an independent, untrusted candidate.
         }
      }
   }
   return out;
}

std::optional<time_point> mdns_registry::next_expiry() const {
   auto next = std::optional<time_point>{};
   for (const auto& e : _records) {
      if (!next || e.expires_at < *next) {
         next = e.expires_at;
      }
   }
   return next;
}

std::size_t mdns_registry::record_count() const noexcept {
   return _records.size();
}
std::size_t mdns_registry::byte_count() const noexcept {
   return _bytes;
}

} // namespace forge::net::p2p::detail
