module;

#include <optional>
#include <algorithm>
#include <boost/asio/ip/address.hpp>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <set>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.exceptions;
import forge.multiformats.multiaddr;

#include "details/host_addresses.hxx"

namespace forge::net::p2p::host_addresses {
namespace {

[[nodiscard]] std::string lower_host(std::string value) {
   std::ranges::transform(value, value.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
   while (!value.empty() && value.back() == '.') {
      value.pop_back();
   }
   return value;
}

[[nodiscard]] bool is_localhost_name(std::string value) {
   value = lower_host(std::move(value));
   return value == "localhost" || value.ends_with(".localhost");
}

[[nodiscard]] bool is_private_v4(const boost::asio::ip::address_v4& address) noexcept {
   const auto value = address.to_uint();
   return (value & 0xff00'0000U) == 0x0a00'0000U || (value & 0xfff0'0000U) == 0xac10'0000U ||
          (value & 0xffff'0000U) == 0xc0a8'0000U || (value & 0xffc0'0000U) == 0x6440'0000U;
}

[[nodiscard]] bool is_link_local_v4(const boost::asio::ip::address_v4& address) noexcept {
   return (address.to_uint() & 0xffff'0000U) == 0xa9fe'0000U;
}

[[nodiscard]] bool is_unroutable_v4(const boost::asio::ip::address_v4& address) noexcept {
   const auto value = address.to_uint();
   return (value & 0xff00'0000U) == 0x0000'0000U || (value & 0xffff'ffc0U) == 0xc000'0000U ||
          (value & 0xffff'ff00U) == 0xc000'0200U || (value & 0xffff'ff00U) == 0xc058'6300U ||
          (value & 0xfffe'0000U) == 0xc612'0000U || (value & 0xffff'ff00U) == 0xc633'6400U ||
          (value & 0xffff'ff00U) == 0xcb00'7100U || (value & 0xf000'0000U) == 0xe000'0000U ||
          (value & 0xf000'0000U) == 0xf000'0000U;
}

[[nodiscard]] bool is_private_v6(const boost::asio::ip::address_v6& address) noexcept {
   const auto bytes = address.to_bytes();
   return (bytes[0] & 0xfeU) == 0xfcU;
}

[[nodiscard]] bool is_public_v6(const boost::asio::ip::address_v6& address) noexcept {
   const auto bytes = address.to_bytes();
   const auto documentation = bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x0dU && bytes[3] == 0xb8U;
   const auto global_unicast = (bytes[0] & 0xe0U) == 0x20U && !documentation;
   const auto well_known_nat64 = bytes[0] == 0 && bytes[1] == 0x64U && bytes[2] == 0xffU && bytes[3] == 0x9bU &&
                                 bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 &&
                                 bytes[8] == 0 && bytes[9] == 0 && bytes[10] == 0 && bytes[11] == 0;
   const auto local_nat64 = bytes[0] == 0 && bytes[1] == 0x64U && bytes[2] == 0xffU && bytes[3] == 0x9bU &&
                            bytes[4] == 0 && bytes[5] == 1;
   return global_unicast || well_known_nat64 || local_nat64;
}

} // namespace

std::optional<std::string> observer_group(const endpoint& value) {
   if (value.transport.host_type != endpoint::host_kind::ip4 && value.transport.host_type != endpoint::host_kind::ip6) {
      return std::nullopt;
   }
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.transport.host, error);
   if (error || address.is_v4() != (value.transport.host_type == endpoint::host_kind::ip4)) {
      return std::nullopt;
   }
   if (address.is_v4()) {
      return address.to_string();
   }
   auto bytes = address.to_v6().to_bytes();
   if (address.to_v6().is_v4_mapped()) {
      return boost::asio::ip::address_v4{boost::asio::ip::address_v4::bytes_type{
          bytes[12], bytes[13], bytes[14], bytes[15]}}.to_string();
   }
   std::fill(bytes.begin() + 7, bytes.end(), 0);
   return boost::asio::ip::address_v6{bytes}.to_string();
}

endpoint_scope classify_endpoint_scope(const endpoint& value) {
   using host_kind = endpoint::host_kind;
   switch (value.transport.host_type) {
   case host_kind::dns:
   case host_kind::dns4:
   case host_kind::dns6:
      return is_localhost_name(value.transport.host) ? endpoint_scope::loopback : endpoint_scope::dns;
   case host_kind::ip4:
   case host_kind::ip6:
      break;
   }

   auto error = boost::system::error_code{};
   const auto parsed = boost::asio::ip::make_address(value.transport.host, error);
   if (error) {
      return endpoint_scope::unroutable;
   }
   if (parsed.is_v4()) {
      const auto address = parsed.to_v4();
      if (address.is_loopback()) {
         return endpoint_scope::loopback;
      }
      if (address.is_unspecified() || address.is_multicast()) {
         return endpoint_scope::unroutable;
      }
      if (is_link_local_v4(address)) {
         return endpoint_scope::link_local;
      }
      if (is_private_v4(address)) {
         return endpoint_scope::private_address;
      }
      if (is_unroutable_v4(address)) {
         return endpoint_scope::unroutable;
      }
      return endpoint_scope::public_address;
   }

   const auto address = parsed.to_v6();
   if (address.is_loopback()) {
      return endpoint_scope::loopback;
   }
   if (address.is_unspecified() || address.is_multicast()) {
      return endpoint_scope::unroutable;
   }
   if (address.is_link_local()) {
      return endpoint_scope::link_local;
   }
   if (is_private_v6(address)) {
      return endpoint_scope::private_address;
   }
   return is_public_v6(address) ? endpoint_scope::public_address : endpoint_scope::unroutable;
}

namespace {

[[nodiscard]] bool peer_suffix_matches(const endpoint& value, const peer_id& peer) {
   if (value.relayed.has_value()) {
      return value.relayed->target.to_bytes() == peer.to_bytes();
   }
   return !value.peer.has_value() || value.peer->to_bytes() == peer.to_bytes();
}

enum class peer_suffix {
   absent,
   terminal,
};

[[nodiscard]] std::optional<peer_suffix>
validate_peer_suffix(const forge::multiformats::multiaddr& value, const peer_id& peer) {
   const auto& components = value.components();
   auto circuit = std::optional<std::size_t>{};
   auto terminal = false;
   for (auto index = std::size_t{0}; index < components.size(); ++index) {
      const auto& component = components[index];
      if (component.code == forge::multiformats::protocol_code::p2p_circuit) {
         if (circuit.has_value() || index + 2 != components.size() ||
             components[index + 1].code != forge::multiformats::protocol_code::p2p) {
            return std::nullopt;
         }
         circuit = index;
         continue;
      }
      if (component.code != forge::multiformats::protocol_code::p2p) {
         continue;
      }
      try {
         const auto component_peer = peer_id::from_string(component.value);
         if (index + 1 == components.size()) {
            if (component_peer != peer) {
               return std::nullopt;
            }
            terminal = true;
         } else if (components[index + 1].code != forge::multiformats::protocol_code::p2p_circuit) {
            return std::nullopt;
         }
      } catch (const forge::exceptions::base&) {
         return std::nullopt;
      }
   }
   return terminal ? peer_suffix::terminal : peer_suffix::absent;
}

[[nodiscard]] bool source_allows(endpoint_scope candidate, const learning_context& context) {
   if (candidate == endpoint_scope::link_local || candidate == endpoint_scope::unroutable) {
      return false;
   }
   if (context.source == source_kind::routed && !context.remote_endpoint.has_value()) {
      return false;
   }
   if (candidate == endpoint_scope::dns || candidate == endpoint_scope::public_address) {
      return true;
   }
   if (context.source == source_kind::third_party || !context.remote_endpoint.has_value()) {
      return false;
   }

   const auto remote = classify_endpoint_scope(*context.remote_endpoint);
   switch (context.source) {
   case source_kind::authenticated:
      if (remote == endpoint_scope::loopback) {
         return candidate == endpoint_scope::loopback || candidate == endpoint_scope::private_address;
      }
      return remote == endpoint_scope::private_address && candidate == endpoint_scope::private_address;
   case source_kind::routed:
      return (remote == endpoint_scope::loopback && candidate == endpoint_scope::loopback) ||
             (remote == endpoint_scope::private_address && candidate == endpoint_scope::private_address);
   case source_kind::third_party:
      return false;
   }
   return false;
}

} // namespace

std::vector<endpoint> merge_advertised(const std::vector<endpoint>& configured, const std::vector<endpoint>& listened,
                                       const peer_id& local) {
   auto out = std::vector<endpoint>{};
   auto seen = std::set<std::string>{};
   const auto append = [&](endpoint value) {
      value.peer = local;
      const auto key = value.to_string();
      if (seen.insert(key).second) {
         out.push_back(std::move(value));
      }
   };
   for (const auto& endpoint : configured) {
      append(endpoint);
   }
   for (const auto& endpoint : listened) {
      append(endpoint);
   }
   return out;
}

std::optional<endpoint> learned(endpoint value, const peer_id& peer) {
   return learned(std::move(value), peer, learning_context{});
}

std::optional<endpoint> learned(endpoint value, const peer_id& peer, learning_context context) {
   if (!peer_suffix_matches(value, peer)) {
      return std::nullopt;
   }
   if (!source_allows(classify_endpoint_scope(value), context)) {
      return std::nullopt;
   }
   if (!value.relayed.has_value()) {
      value.peer = peer;
   }
   return value;
}

std::vector<endpoint> sanitize_discovered_endpoints(std::vector<endpoint> values, const peer_id& peer,
                                                    learning_context context) {
   auto out = std::vector<endpoint>{};
   auto seen = std::set<std::string>{};
   out.reserve(values.size());
   for (auto& value : values) {
      auto item = learned(std::move(value), peer, context);
      if (!item) {
         continue;
      }
      const auto key = item->to_string();
      if (seen.insert(key).second) {
         out.push_back(std::move(*item));
      }
   }
   return out;
}

std::optional<forge::multiformats::multiaddr>
learned(forge::multiformats::multiaddr value, const peer_id& peer, learning_context context) {
   const auto suffix = validate_peer_suffix(value, peer);
   if (!suffix) {
      return std::nullopt;
   }
   if (*suffix == peer_suffix::absent) {
      value.push({.code = forge::multiformats::protocol_code::p2p, .value = peer.to_string()});
   }
   try {
      if (auto concrete = learned(parse_endpoint(value.to_string()), peer, context)) {
         return concrete->to_multiaddr();
      }
      return std::nullopt;
   } catch (const forge::exceptions::base&) {
      const auto& components = value.components();
      if (components.empty() || std::ranges::none_of(components, [](const auto& component) {
             return component.code == forge::multiformats::protocol_code::dnsaddr;
          }) ||
          !source_allows(endpoint_scope::dns, context)) {
         return std::nullopt;
      }
      return value;
   }
}

std::vector<forge::multiformats::multiaddr>
sanitize_discovered_addresses(std::vector<forge::multiformats::multiaddr> values, const peer_id& peer,
                             learning_context context) {
   auto out = std::vector<forge::multiformats::multiaddr>{};
   auto seen = std::set<std::string>{};
   out.reserve(values.size());
   for (auto& value : values) {
      auto item = learned(std::move(value), peer, context);
      if (!item) {
         continue;
      }
      const auto key = item->to_string();
      if (seen.insert(key).second) {
         out.push_back(std::move(*item));
      }
   }
   return out;
}

} // namespace forge::net::p2p::host_addresses
