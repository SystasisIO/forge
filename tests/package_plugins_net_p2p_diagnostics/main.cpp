#include <concepts>
#include <string>
#include <utility>

import forge.api.core.descriptor;
import forge.api.core.types;
import forge.net.p2p.dht;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.events_api;
import forge.plugins.net.p2p.diagnostics.plugin;

using events_api = forge::plugins::net::p2p::diagnostics::events_api;
using diagnostics_api = forge::plugins::net::p2p::diagnostics::api;

static_assert(std::same_as<decltype(std::declval<const diagnostics_api&>().routing_status(
    std::declval<const forge::net::p2p::protocol_id&>())), forge::net::p2p::dht::routing_status>);
static_assert(std::same_as<decltype(std::declval<const diagnostics_api&>().pubsub_scores()),
                          forge::net::p2p::pubsub::score_snapshot>);

static_assert(std::same_as<decltype(std::declval<const events_api&>().reachability_status()),
                          forge::net::p2p::host_event>);
static_assert(std::same_as<decltype(std::declval<const events_api&>().host_events()),
                          forge::net::p2p::host_event_subscription>);
static_assert(!std::copy_constructible<forge::net::p2p::host_event_subscription>);

int main() {
   const auto plugin = forge::plugins::net::p2p::diagnostics::descriptor();
   const auto snapshots = forge::plugins::net::p2p::diagnostics::api::describe();
   const auto events = events_api::describe();
   return plugin.id.value == "forge.plugins.net.p2p.diagnostics" &&
                  snapshots.id.value == "forge.plugins.net.p2p.diagnostics" &&
                  forge::plugins::net::p2p::diagnostics::plugin{}.version() == "3.0.0" &&
                  snapshots.version.major == 3 && snapshots.version.revision == 0 &&
                  snapshots.supported_surfaces == forge::api::core::surface::local && snapshots.methods.empty() &&
                  events.id.value == "forge.plugins.net.p2p.diagnostics.events" &&
                  events.version.major == 1 && events.version.revision == 0 &&
                  events.supported_surfaces == forge::api::core::surface::local && events.methods.empty()
              ? 0
              : 1;
}
