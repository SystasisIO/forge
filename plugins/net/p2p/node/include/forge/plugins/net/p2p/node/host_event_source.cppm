module;

#include <forge/api/core/macros.hpp>

export module forge.plugins.net.p2p.node.host_event_source;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;

export namespace forge::plugins::net::p2p::node {

class host_event_source : public forge::api::core::contract<host_event_source> {
 public:
   virtual ~host_event_source() = default;

   [[nodiscard]] virtual forge::net::p2p::host_event reachability_status() const = 0;
   [[nodiscard]] virtual forge::net::p2p::host_event_subscription host_events() const = 0;
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::host_event_source,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.host_event_source", 1, 0))
