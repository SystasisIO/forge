#include "details/mdns_wire.hxx"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

import forge.net.transport.endpoint;

namespace forge::net::p2p::detail::mdns_wire {
namespace {
namespace codec = mdns_codec;
namespace ip = boost::asio::ip;

ip::address unscoped(ip::address value) {
   if (value.is_v6()) {
      auto v6 = value.to_v6();
      v6.scope_id(0);
      return v6;
   }
   return value;
}

bool same_record(const codec::record& a, const codec::record& b) {
   if (!same_name(a.owner, b.owner) || a.type != b.type ||
       (a.class_code & codec::class_mask) != (b.class_code & codec::class_mask)) {
      return false;
   }
   if (auto x = std::get_if<codec::ptr>(&a.data)) {
      const auto y = std::get_if<codec::ptr>(&b.data);
      return y && same_name(x->target, y->target);
   }
   if (auto x = std::get_if<codec::srv>(&a.data)) {
      const auto y = std::get_if<codec::srv>(&b.data);
      return y && x->priority == y->priority && x->weight == y->weight && x->port == y->port &&
             same_name(x->target, y->target);
   }
   return a.data == b.data;
}
} // namespace

bool usable(const interface_state::address& address) noexcept {
   return address.flags_known && !address.tentative && !address.duplicate && !address.deprecated &&
          !address.detached && !address.value.is_unspecified() && !address.value.is_multicast() &&
          !address.value.is_loopback() && !(address.value.is_v6() && address.value.to_v6().is_v4_mapped());
}

bool usable(const interface_state::interface& interface) noexcept {
   return interface.index != 0 && interface.generation != 0 && interface.up && interface.running &&
          interface.multicast && !interface.loopback;
}

bool same_name(const codec::name& a, const codec::name& b) {
   const auto fold = [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; };
   return std::ranges::equal(a, b, [&](const auto& x, const auto& y) {
      return std::ranges::equal(x, y, [&](unsigned char l, unsigned char r) { return fold(l) == fold(r); });
   });
}

ip::udp::endpoint group(bool ipv6, std::uint32_t index) {
   if (!ipv6) {
      return {ip::make_address_v4("224.0.0.251"), 5353};
   }
   auto address = ip::make_address_v6("ff02::fb");
   address.scope_id(index);
   return {address, 5353};
}

bool on_link(const interface_state::interface& interface, ip::address remote) {
   if (remote.is_unspecified() || remote.is_multicast()) {
      return false;
   }
   if (remote.is_v6() && remote.to_v6().scope_id() != 0 && remote.to_v6().scope_id() != interface.index) {
      return false;
   }
   remote = unscoped(remote);
   for (const auto& local : interface.addresses) {
      if (!usable(local) || !local.prefix_length || *local.prefix_length == 0 ||
          local.value.is_v4() != remote.is_v4()) {
         continue;
      }
      auto left = std::array<std::uint8_t, 16>{};
      auto right = left;
      const auto size = remote.is_v4() ? 4U : 16U;
      if (*local.prefix_length > size * 8) {
         continue;
      }
      if (remote.is_v4()) {
         const auto l = local.value.to_v4().to_bytes();
         const auto r = remote.to_v4().to_bytes();
         std::copy(l.begin(), l.end(), left.begin());
         std::copy(r.begin(), r.end(), right.begin());
      } else {
         left = local.value.to_v6().to_bytes();
         right = remote.to_v6().to_bytes();
      }
      auto matches = true;
      for (unsigned bit = 0; bit < *local.prefix_length; ++bit) {
         const auto mask = 0x80U >> (bit % 8);
         matches = matches && ((left[bit / 8] & mask) == (right[bit / 8] & mask));
      }
      if (matches) {
         return true;
      }
   }
   return false;
}

bool admit(const interface_state::interface& interface, bool ipv6,
           const forge::net::transport::datagram_io::received& route) {
   if (!usable(interface) || route.interface_index != interface.index || route.local.port() != 5353 ||
       route.remote.port() == 0 || route.remote.address().is_unspecified() ||
       route.remote.address().is_multicast() || route.remote.address().is_loopback() ||
       route.remote.address().is_v6() != ipv6 || route.local.address().is_v6() != ipv6) {
      return false;
   }
   for (auto address : {route.local.address(), route.remote.address()}) {
      if (address.is_v6() && (address.to_v6().is_v4_mapped() ||
          (address.to_v6().scope_id() != 0 && address.to_v6().scope_id() != interface.index))) {
         return false;
      }
   }
   if (unscoped(route.local.address()) == unscoped(group(ipv6, interface.index).address())) {
      return true; // RFC6762: link-scoped multicast destination is local-link evidence.
   }
   const auto destination = unscoped(route.local.address());
   const auto ours = std::ranges::any_of(interface.addresses, [&](const auto& address) {
      return usable(address) && unscoped(address.value) == destination;
   });
   return ours && on_link(interface, route.remote.address());
}

codec::limits bounds(const mdns_policy& policy) {
   auto result = codec::limits{};
   result.max_packet_size = policy.max_packet_size;
   result.max_records = policy.max_records_per_packet;
   result.max_records_per_section = policy.max_records_per_packet;
   result.max_txt_attributes_per_record = std::min(policy.max_addresses_per_peer, std::size_t{64});
   return result;
}

codec::message query(const codec::name& service) {
   auto value = codec::message{};
   value.questions.push_back({service, codec::type_ptr, 1});
   return value;
}

codec::message advertisement(const mdns_policy& policy, const codec::name& service, std::string_view instance,
                             const peer_id& local, const interface_state::interface& interface,
                             std::span<const endpoint> listeners) {
   if (service.size() != 3 || service[0].empty() || service[0].front() != '_' ||
       service[1] != "_udp" || service[2] != "local" || instance.size() < 32 || instance.size() > 63 ||
       !std::ranges::all_of(instance, [](char c) { return c >= 'a' && c <= 'z'; }) || !usable(interface)) {
      throw std::invalid_argument{"invalid mDNS advertisement identity/interface"};
   }
   if (listeners.size() > 256) {
      throw std::length_error{"mDNS local listener limit exceeded"};
   }
   auto owner = service;
   owner.insert(owner.begin(), std::string{instance});
   const auto host = codec::name{std::string{instance}, "local"};
   auto attributes = codec::txt{};
   auto advertised = std::vector<ip::address>{};
   std::uint16_t port = 0;
   for (const auto& listener : listeners) {
      if ((!listener.is_direct_tcp() && !listener.is_direct_quic()) || listener.transport.port == 0 ||
          (listener.peer && listener.peer->value != local.value) ||
          (listener.transport.host_type != endpoint::host_kind::ip4 &&
           listener.transport.host_type != endpoint::host_kind::ip6)) {
         continue;
      }
      const auto literal = listener.transport.literal_address();
      if (literal.is_v6() && literal.to_v6().scope_id() != 0 &&
          literal.to_v6().scope_id() != interface.index) {
         continue;
      }
      for (const auto& address : interface.addresses) {
         if (!usable(address) || address.value.is_v4() != literal.is_v4() ||
             (!literal.is_unspecified() && unscoped(literal) != unscoped(address.value))) {
            continue;
         }
         auto projected = listener;
         projected.transport = forge::net::transport::endpoint::from_address(
             unscoped(address.value), listener.transport.port, listener.transport.protocol);
         projected.transport.zone.clear();
         projected.peer = local;
         const auto text = "dnsaddr=" + projected.to_string();
         if (text.size() > 255) {
            throw std::length_error{"mDNS dnsaddr attribute exceeds 255 bytes"};
         }
         auto attribute = codec::bytes{text.begin(), text.end()};
         if (std::find(attributes.attributes.begin(), attributes.attributes.end(), attribute) != attributes.attributes.end()) {
            continue;
         }
         if (attributes.attributes.size() >= std::min(policy.max_addresses_per_peer, std::size_t{64})) {
            throw std::length_error{"mDNS advertisement address limit exceeded"};
         }
         attributes.attributes.push_back(std::move(attribute));
         const auto value = unscoped(address.value);
         if (std::find(advertised.begin(), advertised.end(), value) == advertised.end()) {
            advertised.push_back(value);
         }
         if (port == 0) { port = listener.transport.port; }
      }
   }
   auto packet = codec::message{.head = {.flags = 0x8400}};
   if (attributes.attributes.empty()) {
      return packet;
   }
   const auto ttl = static_cast<std::uint32_t>(policy.record_ttl.count());
   packet.answers.push_back({service, codec::type_ptr, 1, ttl, codec::ptr{owner}});
   packet.additionals.push_back({owner, codec::type_txt, 0x8001, ttl, std::move(attributes)});
   packet.additionals.push_back({owner, codec::type_srv, 0x8001, ttl, codec::srv{0, 0, port, host}});
   for (const auto& address : advertised) {
      if (address.is_v4()) {
         packet.additionals.push_back({host, codec::type_a, 0x8001, ttl, codec::a{address.to_v4().to_bytes()}});
      } else {
         packet.additionals.push_back({host, codec::type_aaaa, 0x8001, ttl, codec::aaaa{address.to_v6().to_bytes()}});
      }
   }
   // Size validation precedes socket enqueue. No silently incomplete TXT/RR set.
   static_cast<void>(codec::encode(packet, bounds(policy)));
   return packet;
}

std::optional<codec::message> answer(const codec::message& request, const codec::message& advertised, bool legacy) {
   if ((request.head.flags & 0xf80f) != 0 || advertised.answers.empty()) {
      return std::nullopt;
   }
   auto response = codec::message{.head = {.id = legacy ? request.head.id : std::uint16_t{0}, .flags = 0x8400}};
   const auto known = [&](const auto& record) {
      return std::ranges::any_of(request.answers, [&](const auto& prior) {
         return prior.ttl >= record.ttl / 2 + record.ttl % 2 && same_record(prior, record);
      });
   };
   const auto contains = [](const auto& records, const auto& record) {
      return std::ranges::any_of(records, [&](const auto& prior) { return same_record(prior, record); });
   };
   const auto visit = [&](auto visitor) {
      for (const auto* records : {&advertised.answers, &advertised.additionals}) {
         for (const auto& record : *records) { visitor(record); }
      }
   };
   auto meta = false;
   for (const auto& question : request.questions) {
      const auto qclass = question.class_code & codec::class_mask;
      if (qclass != 1 && qclass != 255) { continue; }
      meta = meta || ((question.type == codec::type_ptr || question.type == 255) &&
                      same_name(question.owner, {"_services", "_dns-sd", "_udp", "local"}));
      visit([&](const auto& record) {
         if (same_name(question.owner, record.owner) && (question.type == record.type || question.type == 255) &&
             !known(record) && !contains(response.answers, record)) {
            response.answers.push_back(record);
         }
      });
   }
   if (meta) {
      const auto record = codec::record{{"_services", "_dns-sd", "_udp", "local"}, codec::type_ptr, 1,
          advertised.answers.front().ttl, codec::ptr{advertised.answers.front().owner}};
      if (!known(record) && !contains(response.answers, record)) { response.answers.push_back(record); }
   }
   if (response.answers.empty()) { return std::nullopt; }
   const auto additional = [&](const auto& record) {
      if (!known(record) && !contains(response.answers, record) && !contains(response.additionals, record)) {
         response.additionals.push_back(record);
      }
   };
   // Build dependencies only from surviving answers. A suppressed PTR must
   // not leave unsolicited TXT/SRV/address records behind.
   for (const auto& answer : response.answers) {
      const auto* ptr = std::get_if<codec::ptr>(&answer.data);
      if (!ptr) { continue; }
      visit([&](const auto& record) {
         if ((record.type == codec::type_txt || record.type == codec::type_srv) &&
             same_name(record.owner, ptr->target)) { additional(record); }
      });
   }
   const auto addresses = [&](const auto& record) {
      const auto* srv = std::get_if<codec::srv>(&record.data);
      if (!srv) { return; }
      visit([&](const auto& address) {
         if ((address.type == codec::type_a || address.type == codec::type_aaaa) &&
             same_name(address.owner, srv->target)) { additional(address); }
      });
   };
   for (const auto& record : response.answers) { addresses(record); }
   for (const auto& answer : response.answers) {
      const auto* ptr = std::get_if<codec::ptr>(&answer.data);
      if (!ptr) { continue; }
      // A known SRV suppresses its own emission, not the surviving PTR's
      // address dependencies. Derive those from the original advertisement;
      // additional() independently suppresses each known A/AAAA record.
      visit([&](const auto& record) {
         if (record.type == codec::type_srv && same_name(record.owner, ptr->target)) {
            addresses(record);
         }
      });
   }
   if (legacy) {
      response.questions = request.questions;
      for (auto& question : response.questions) { question.class_code &= codec::class_mask; }
      for (auto* records : {&response.answers, &response.additionals}) {
         for (auto& record : *records) {
            record.class_code &= codec::class_mask;
            record.ttl = std::min(record.ttl, std::uint32_t{10});
         }
      }
   }
   return response;
}

} // namespace forge::net::p2p::detail::mdns_wire
