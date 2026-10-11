#include <concepts>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.types;
import forge.api.p2p.publication;
import forge.chrono.timestamp;
import forge.net.p2p.dht;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.host_event_source;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.types;

static_assert(std::same_as<
              decltype(std::declval<forge::plugins::net::p2p::node::api&>().publish_api(
                  std::declval<forge::api::core::binding_plan>(), std::declval<forge::net::p2p::protocol_id>())),
              forge::api::p2p::publication>);

namespace p2p = forge::net::p2p;
using dht_api = forge::plugins::net::p2p::node::dht_api;
using host_event_source = forge::plugins::net::p2p::node::host_event_source;
using pubsub_source = forge::plugins::net::p2p::node::pubsub_source;

static_assert(std::same_as<decltype(std::declval<pubsub_source&>().async_enable_partial(
    p2p::pubsub::topic{}, p2p::pubsub::handler{}, p2p::pubsub::partial_options{})),
    boost::asio::awaitable<p2p::pubsub::partial_topic>>);

static_assert(std::same_as<decltype(std::declval<const host_event_source&>().reachability_status()), p2p::host_event>);
static_assert(std::same_as<decltype(std::declval<const host_event_source&>().host_events()), p2p::host_event_subscription>);
static_assert(!std::copy_constructible<p2p::host_event_subscription>);

static_assert(std::same_as<decltype(std::declval<dht_api&>().find_peer(
                              std::declval<p2p::protocol_id>(), std::declval<p2p::peer_id>())),
                          boost::asio::awaitable<p2p::dht::query_result>>);
static_assert(std::same_as<decltype(std::declval<dht_api&>().provide(
                              std::declval<p2p::protocol_id>(), std::declval<p2p::dht::key>())),
                          boost::asio::awaitable<p2p::provider_registration>>);
static_assert(std::same_as<decltype(std::declval<dht_api&>().find_providers(
                              std::declval<p2p::protocol_id>(), std::declval<p2p::dht::key>())),
                          boost::asio::awaitable<std::vector<p2p::dht::peer>>>);
static_assert(std::same_as<decltype(std::declval<dht_api&>().put_value(
                              std::declval<p2p::protocol_id>(), std::declval<p2p::dht::record>())),
                          boost::asio::awaitable<p2p::dht::value_put_result>>);
static_assert(std::same_as<decltype(std::declval<dht_api&>().get_value(
                              std::declval<p2p::protocol_id>(), std::declval<p2p::dht::key>())),
                          boost::asio::awaitable<p2p::dht::value_get_result>>);
static_assert(std::same_as<decltype(std::declval<const dht_api&>().create_ipns_record(
                              std::declval<std::span<const std::uint8_t>>(), std::uint64_t{},
                              forge::chrono::timestamp{}, std::chrono::nanoseconds{})), p2p::ipns::record>);

int main() {
   const auto descriptor = forge::plugins::net::p2p::node::descriptor();
   const auto config = forge::plugins::net::p2p::node::config{};
   const auto dht = dht_api::describe();
   const auto events = host_event_source::describe();
   const auto pubsub = pubsub_source::describe();
   return descriptor.id.value == "forge.plugins.net.p2p.node" &&
                  config.topology_mode == forge::plugins::net::p2p::node::topology_mode::managed &&
                  config.topology_target == 160 && dht.id.value == "forge.plugins.net.p2p.node.dht" &&
                  dht.version.major == 1 && dht.version.revision == 0 &&
                  dht.supported_surfaces == forge::api::core::surface::local && dht.methods.empty() &&
                  events.id.value == "forge.plugins.net.p2p.node.host_event_source" &&
                  events.version.major == 1 && events.version.revision == 0 &&
                  events.supported_surfaces == forge::api::core::surface::local && events.methods.empty() &&
                  pubsub.id.value == "forge.plugins.net.p2p.node.pubsub_source" &&
                  pubsub.version.major == 2 && pubsub.version.revision == 0 &&
                  pubsub.supported_surfaces == forge::api::core::surface::local && pubsub.methods.empty()
              ? 0
              : 1;
}
