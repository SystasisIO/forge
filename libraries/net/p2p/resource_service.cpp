module;

#include <string>

module forge.net.p2p.node;

import forge.net.p2p.protocol;

#include "details/resource_service.hxx"

namespace forge::net::p2p::detail {

std::string resource_service_id(const protocol_id& protocol, bool dht_profile) {
   if (dht_profile) {
      return "p2p.dht:" + protocol.value;
   }
   if (protocol == builtins::identify || protocol == builtins::identify_push) {
      return "p2p.identify";
   }
   if (protocol == builtins::ping) {
      return "p2p.ping";
   }
   if (protocol == builtins::autonat_v1 || protocol == builtins::autonat_v2_dial_request ||
       protocol == builtins::autonat_v2_dial_back) {
      return "p2p.autonat";
   }
   if (protocol == builtins::relay_hop || protocol == builtins::relay_stop) {
      return "p2p.relay";
   }
   if (protocol == builtins::dcutr) {
      return "p2p.dcutr";
   }
   if (protocol == builtins::rendezvous) {
      return "p2p.rendezvous";
   }
   if (protocol == builtins::meshsub_v10 || protocol == builtins::meshsub_v11 ||
       protocol == builtins::meshsub_v12 || protocol == builtins::meshsub_v13) {
      return "p2p.gossipsub";
   }
   if (protocol == builtins::peer_exchange) {
      return "p2p.peer-exchange";
   }
   if (protocol == builtins::echo) {
      return "p2p.echo";
   }
   return "p2p.custom:" + protocol.value;
}

} // namespace forge::net::p2p::detail
