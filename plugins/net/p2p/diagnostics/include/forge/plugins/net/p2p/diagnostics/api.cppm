module;

#include <forge/api/core/macros.hpp>

#include <vector>

export module forge.plugins.net.p2p.diagnostics.api;

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
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.diagnostics.types;

export namespace forge::plugins::net::p2p::diagnostics {

class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;

   [[nodiscard]] virtual forge::net::p2p::diagnostics::snapshot snapshot() const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::snapshot
   snapshot(forge::net::p2p::diagnostics::options options) const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::network_state network() const = 0;
   [[nodiscard]] virtual forge::net::p2p::resource_manager::snapshot resources() const = 0;
   [[nodiscard]] virtual forge::net::p2p::pubsub::snapshot pubsub() const = 0;
   [[nodiscard]] virtual std::vector<forge::net::p2p::diagnostics::peer> peers(filter value = {}) const = 0;
   [[nodiscard]] virtual forge::net::p2p::diagnostics::peer peer(forge::net::p2p::peer_id value) const = 0;
};

} // namespace forge::plugins::net::p2p::diagnostics

FORGE_EXPORT_API(::forge::plugins::net::p2p::diagnostics::api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.diagnostics", 2, 0))
