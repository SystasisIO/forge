module;

#include <string>
#include <tuple>
#include <utility>

module forge.plugins.swarm.node.plugin;

import forge.swarm.core.identity;
import forge.net.p2p.dht;
import forge.multiformats.multihash;
import forge.raw.raw;
#include "details/provider_key.hxx"

namespace forge::plugins::swarm::node {

forge::net::p2p::dht::key provider_key(forge::swarm::core::swarm_id id, std::string realm) {
   const auto bytes = forge::raw::pack(std::tuple{std::string{"forge.swarm.providers/v1"}, std::move(realm), id});
   return {forge::multiformats::multihash::sha2_256(bytes).encode()};
}

} // namespace forge::plugins::swarm::node
