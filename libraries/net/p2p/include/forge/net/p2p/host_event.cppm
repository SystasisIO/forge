module;

#include <cstdint>
#include <vector>
#include <boost/describe.hpp>

export module forge.net.p2p.host_event;

import forge.net.p2p.endpoint;
import forge.net.p2p.lifecycle;
import forge.net.p2p.reachability;

export namespace forge::net::p2p {

struct host_event {
   struct address_status {
      endpoint address;
      reachability::state value = reachability::state::unknown;
   };

   std::uint64_t generation = 0;
   bool resync_required = false;
   lifecycle_phase phase = lifecycle_phase::idle;
   reachability::state effective = reachability::state::unknown;
   reachability::state autonat_v1 = reachability::state::unknown;
   std::vector<address_status> autonat_v2;
   std::vector<endpoint> confirmed_addresses;
};

} // namespace forge::net::p2p

BOOST_DESCRIBE_STRUCT(forge::net::p2p::host_event::address_status, (), (address, value))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::host_event, (),
                      (generation, resync_required, phase, effective, autonat_v1, autonat_v2, confirmed_addresses))
