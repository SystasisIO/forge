#pragma once

namespace forge::plugins::swarm::node {

[[nodiscard]] forge::net::p2p::dht::key provider_key(forge::swarm::core::swarm_id, std::string realm);

} // namespace forge::plugins::swarm::node
