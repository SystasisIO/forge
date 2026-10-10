#pragma once

namespace forge::net::p2p {

[[nodiscard]] inline capability_set capabilities_for(std::span<const protocol_id> protocols) noexcept {
   auto out = capability_set{};
   for (const auto& protocol : protocols) {
      if (protocol == builtins::relay_hop) {
         out.add(capabilities::relay);
         out.add(capabilities::relay_reservation);
      } else if (protocol == builtins::peer_exchange) {
         out.add(capabilities::peer_exchange);
      } else if (protocol == builtins::rendezvous) {
         out.add(capabilities::rendezvous);
      } else if (protocol == builtins::dcutr) {
         out.add(capabilities::hole_punching);
      } else if (protocol == builtins::autonat_v1 || protocol == builtins::autonat_v2_dial_request ||
                 protocol == builtins::autonat_v2_dial_back) {
         out.add(capabilities::autonat);
      } else if (protocol == builtins::meshsub_v13 || protocol == builtins::meshsub_v12 ||
                 protocol == builtins::meshsub_v11 || protocol == builtins::meshsub_v10) {
         out.add(capabilities::pubsub);
      }
   }
   return out;
}

} // namespace forge::net::p2p
