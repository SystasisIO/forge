#pragma once

namespace forge::plugins::swarm::node {

// Immutable shared dependencies only. No peer cache or operation registry.
struct capability_context {
   std::shared_ptr<forge::plugins::net::p2p::node::api> node;
   std::shared_ptr<forge::plugins::net::p2p::node::dht_api> dht;
   forge::net::p2p::protocol_id profile;
   forge::net::p2p::protocol_id application;
   std::stop_token lifetime;
};

} // namespace forge::plugins::swarm::node
