module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module forge.net.p2p.endpoint;

import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;

namespace forge::net::p2p {
namespace {

[[nodiscard]] forge::multiformats::protocol_code host_kind_code(endpoint::host_kind kind) {
   using enum endpoint::host_kind;
   switch (kind) {
      case ip4:
         return forge::multiformats::protocol_code::ip4;
      case ip6:
         return forge::multiformats::protocol_code::ip6;
      case dns:
         return forge::multiformats::protocol_code::dns;
      case dns4:
         return forge::multiformats::protocol_code::dns4;
      case dns6:
         return forge::multiformats::protocol_code::dns6;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unsupported P2P endpoint address kind");
}

[[nodiscard]] forge::multiformats::protocol_code protocol_kind_code(endpoint::protocol_kind kind) {
   using enum endpoint::protocol_kind;
   switch (kind) {
      case quic_v1:
         return forge::multiformats::protocol_code::quic_v1;
      case tcp:
         return forge::multiformats::protocol_code::tcp;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unsupported P2P endpoint transport kind");
}

[[nodiscard]] endpoint::host_kind endpoint_host_kind(forge::multiformats::protocol_code code) {
   switch (code) {
      case forge::multiformats::protocol_code::ip4:
         return endpoint::host_kind::ip4;
      case forge::multiformats::protocol_code::ip6:
         return endpoint::host_kind::ip6;
      case forge::multiformats::protocol_code::dns:
         return endpoint::host_kind::dns;
      case forge::multiformats::protocol_code::dns4:
         return endpoint::host_kind::dns4;
      case forge::multiformats::protocol_code::dns6:
         return endpoint::host_kind::dns6;
      default:
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint must start with an address component");
   }
}

[[nodiscard]] endpoint::protocol_kind protocol_kind(forge::multiformats::protocol_code code) {
   switch (code) {
      case forge::multiformats::protocol_code::quic_v1:
         return endpoint::protocol_kind::quic_v1;
      case forge::multiformats::protocol_code::tcp:
         return endpoint::protocol_kind::tcp;
      default:
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint is missing supported transport component");
   }
}

[[nodiscard]] forge::multiformats::protocol_code encapsulation_code(endpoint::encapsulation_kind value) {
   switch (value) {
      case endpoint::encapsulation_kind::none:
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint has no encapsulation component");
      case endpoint::encapsulation_kind::ws:
         return forge::multiformats::protocol_code::ws;
      case endpoint::encapsulation_kind::wss:
         return forge::multiformats::protocol_code::wss;
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unsupported P2P endpoint encapsulation kind");
}

} // namespace

forge::multiformats::multiaddr endpoint::to_multiaddr() const {
   auto out = forge::multiformats::multiaddr{};
   if (!transport.zone.empty()) {
      if (transport.host_type != host_kind::ip6) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint zone requires an IPv6 host");
      }
      out.push({.code = forge::multiformats::protocol_code::ip6zone, .value = transport.zone});
   }
   out.push({.code = host_kind_code(transport.host_type), .value = transport.host});
   if (transport.protocol == protocol_kind::quic_v1) {
      out.push({.code = forge::multiformats::protocol_code::udp, .value = std::to_string(transport.port)});
      out.push({.code = forge::multiformats::protocol_code::quic_v1, .value = {}});
   } else {
      out.push({.code = protocol_kind_code(transport.protocol), .value = std::to_string(transport.port)});
   }
   if (encapsulation != encapsulation_kind::none) {
      out.push({.code = encapsulation_code(encapsulation), .value = {}});
   }
   if (peer.has_value()) {
      out.push({.code = forge::multiformats::protocol_code::p2p, .value = peer->to_string()});
   }
   if (relayed.has_value()) {
      out.push({.code = forge::multiformats::protocol_code::p2p_circuit, .value = {}});
      out.push({.code = forge::multiformats::protocol_code::p2p, .value = relayed->target.to_string()});
   }
   return out;
}

std::string endpoint::to_string() const {
   return to_multiaddr().to_string();
}

bool endpoint::is_direct_quic() const noexcept {
   return !relayed.has_value() && encapsulation == encapsulation_kind::none && transport.protocol == protocol_kind::quic_v1;
}

bool endpoint::is_direct_tcp() const noexcept {
   return !relayed.has_value() && encapsulation == encapsulation_kind::none && transport.protocol == protocol_kind::tcp;
}

endpoint parse_endpoint(std::string_view value) {
   const auto parsed = forge::multiformats::multiaddr::parse(value);
   const auto& components = parsed.components();
   if (components.size() < 2) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint must include address and transport components");
   }

   auto address_index = std::size_t{};
   auto zone = std::string{};
   if (components.front().code == forge::multiformats::protocol_code::ip6zone) {
      if (components.size() < 3 || components[1].code != forge::multiformats::protocol_code::ip6) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P ip6zone must directly precede an ip6 component");
      }
      zone = components.front().value;
      address_index = 1;
   }
   for (auto index = address_index + 1; index < components.size(); ++index) {
      if (components[index].code == forge::multiformats::protocol_code::ip6zone) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P ip6zone must directly precede an ip6 component");
      }
   }

   auto result = endpoint{.transport = forge::net::transport::endpoint{
                              .host_type = endpoint_host_kind(components[address_index].code),
                              .host = components[address_index].value,
                              .zone = std::move(zone),
                          }};
   auto suffix = address_index + 2;
   if (suffix > components.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint must include address and transport components");
   }
   const auto transport_index = address_index + 1;
   if (components[transport_index].code == forge::multiformats::protocol_code::udp) {
      if (components.size() < suffix + 1) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P UDP endpoint must include quic-v1 component");
      }
      try {
         result.transport.port = static_cast<std::uint16_t>(std::stoul(components[transport_index].value));
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint port is invalid");
      }
      if (components[suffix].code != forge::multiformats::protocol_code::quic_v1) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P UDP endpoint is missing quic-v1 component");
      }
      result.transport.protocol = endpoint::protocol_kind::quic_v1;
      ++suffix;
   } else {
      result.transport.protocol = protocol_kind(components[transport_index].code);
      try {
         result.transport.port = static_cast<std::uint16_t>(std::stoul(components[transport_index].value));
      } catch (...) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint port is invalid");
      }
   }

   if (suffix < components.size() && (components[suffix].code == forge::multiformats::protocol_code::ws ||
                                      components[suffix].code == forge::multiformats::protocol_code::wss)) {
      result.encapsulation = components[suffix].code == forge::multiformats::protocol_code::ws
                                 ? endpoint::encapsulation_kind::ws
                                 : endpoint::encapsulation_kind::wss;
      ++suffix;
   }

   if (suffix < components.size() && components[suffix].code == forge::multiformats::protocol_code::p2p) {
      result.peer = peer_id::from_string(components[suffix].value);
      ++suffix;
   }

   if (suffix < components.size() && components[suffix].code == forge::multiformats::protocol_code::p2p_circuit) {
      ++suffix;
      if (suffix >= components.size() || components[suffix].code != forge::multiformats::protocol_code::p2p) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P relayed endpoint must use p2p-circuit/p2p suffix");
      }
      result.relayed = endpoint::circuit{.target = peer_id::from_string(components[suffix].value)};
      ++suffix;
   }

   if (suffix != components.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint contains unsupported extra components");
   }
   return result;
}

} // namespace forge::net::p2p
