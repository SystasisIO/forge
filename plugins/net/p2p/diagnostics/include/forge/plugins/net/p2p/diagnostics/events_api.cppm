module;

#include <forge/api/core/macros.hpp>

export module forge.plugins.net.p2p.diagnostics.events_api;

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

export namespace forge::plugins::net::p2p::diagnostics {

class events_api : public forge::api::core::contract<events_api> {
 public:
   virtual ~events_api() = default;

   [[nodiscard]] virtual forge::net::p2p::host_event reachability_status() const = 0;
   // The issued native subscription outlives this facade, but closes when the node stops.
   [[nodiscard]] virtual forge::net::p2p::host_event_subscription host_events() const = 0;
};

} // namespace forge::plugins::net::p2p::diagnostics

FORGE_EXPORT_API(::forge::plugins::net::p2p::diagnostics::events_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.diagnostics.events", 1, 0))
