module;

#include <forge/api/core/macros.hpp>

#include <vector>

export module forge.plugins.net.p2p.node.diagnostics_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.net.p2p.identity;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.node.types;

export namespace forge::plugins::net::p2p::node {

class diagnostics_api : public forge::api::core::contract<diagnostics_api> {
 public:
   virtual ~diagnostics_api() = default;

   [[nodiscard]] virtual forge::net::p2p::diagnostics::snapshot snapshot() const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::snapshot
   snapshot(forge::net::p2p::diagnostics::options options) const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::network_state network() const = 0;
   [[nodiscard]] virtual forge::net::p2p::resource_manager::snapshot resources() const = 0;
   [[nodiscard]] virtual forge::net::p2p::pubsub::snapshot pubsub() const = 0;
   [[nodiscard]] virtual forge::net::p2p::dht::routing_status
   routing_status(const forge::net::p2p::protocol_id& profile) const = 0;
   [[nodiscard]] virtual forge::net::p2p::pubsub::score_snapshot pubsub_scores() const = 0;
   [[nodiscard]] virtual std::vector<forge::net::p2p::diagnostics::peer> peers(peer_filter value = {}) const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::peer peer(forge::net::p2p::peer_id value) const = 0;
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::diagnostics_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.diagnostics", 1, 0))
