module;

#include <forge/api/core/macros.hpp>
#include <memory>
#include <vector>

export module forge.plugins.net.p2p.node.configuration_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.net.p2p.connection_gater;
import forge.net.p2p.dht;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;

export namespace forge::plugins::net::p2p::node {

// Local, pre-start composition only. Changes after startup/stop are rejected.
// Gater and DHT policy contributions are unique; PubSub options may be replaced
// before startup. Callbacks retain their own consumer lifetime.
class configuration_api : public forge::api::core::contract<configuration_api> {
 public:
   virtual ~configuration_api() = default;
   virtual void set_connection_gater(std::shared_ptr<forge::net::p2p::connection_gater> gater) = 0;
   // Requires explicit declarative PubSub opt-in. Native options (including
   // scoring/tracer callbacks) replace the complete pre-start options snapshot.
   virtual void set_pubsub_options(forge::net::p2p::pubsub::options options) = 0;
   // One complete set per configured custom values-enabled profile; never Amino.
   virtual void set_dht_value_policies(forge::net::p2p::protocol_id profile,
                                       std::vector<forge::net::p2p::dht::value_policy> policies) = 0;
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::configuration_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.configuration", 1, 0))
