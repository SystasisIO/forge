#include <concepts>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.types;
import forge.api.p2p.publication;
import forge.chrono.timestamp;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.dht;
import forge.net.p2p.connection_gater;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.events_api;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.pubsub_types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.net.p2p.node;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.types;
import forge.schema.object;

static_assert(std::same_as<
              decltype(std::declval<forge::plugins::net::p2p::node::api&>().publish_api(
                  std::declval<forge::api::core::binding_plan>(), std::declval<forge::net::p2p::protocol_id>())),
              forge::api::p2p::publication>);

namespace p2p = forge::net::p2p;
using node_api = forge::plugins::net::p2p::node::api;
using peer_contribution = forge::plugins::net::p2p::node::peer_contribution;
static_assert(!std::copy_constructible<peer_contribution>);
static_assert(std::movable<peer_contribution>);
static_assert(noexcept(std::declval<peer_contribution&>().close()));
static_assert(std::same_as<decltype(std::declval<node_api&>().connect(
    p2p::endpoint{}, p2p::node::connect_options{})), boost::asio::awaitable<p2p::node::session_info>>);
static_assert(std::same_as<decltype(std::declval<node_api&>().connect(
    std::declval<forge::multiformats::multiaddr>(), p2p::node::connect_options{})),
    boost::asio::awaitable<p2p::node::session_info>>);
static_assert(std::same_as<decltype(std::declval<node_api&>().ping(
    p2p::peer_id{}, p2p::node::open_options{})), boost::asio::awaitable<std::chrono::milliseconds>>);
static_assert(std::same_as<decltype(std::declval<const node_api&>().is_peer_protected(p2p::peer_id{})), bool>);
static_assert(std::same_as<decltype(std::declval<node_api&>().protect_peer(p2p::peer_id{})), peer_contribution>);
static_assert(std::same_as<decltype(std::declval<node_api&>().tag_peer(p2p::peer_id{}, std::int64_t{})), peer_contribution>);
using dht_api = forge::plugins::net::p2p::node::dht_api;
using configuration_api = forge::plugins::net::p2p::node::configuration_api;
static_assert(std::same_as<decltype(std::declval<configuration_api&>().set_pubsub_options(
    p2p::pubsub::options{})), void>);
static_assert(std::same_as<decltype(std::declval<configuration_api&>().set_connection_gater(
    std::shared_ptr<p2p::connection_gater>{})), void>);
static_assert(std::same_as<decltype(std::declval<configuration_api&>().set_dht_value_policies(
    p2p::protocol_id{}, std::vector<p2p::dht::value_policy>{})), void>);
namespace node_plugin = forge::plugins::net::p2p::node;
using events_api = node_plugin::events_api;
using diagnostics_api = node_plugin::diagnostics_api;
static_assert(std::same_as<decltype(std::declval<const diagnostics_api&>().routing_status(
    std::declval<const p2p::protocol_id&>())), p2p::dht::routing_status>);
static_assert(std::same_as<decltype(std::declval<const diagnostics_api&>().pubsub_scores()),
                          p2p::pubsub::score_snapshot>);
using pubsub_api = node_plugin::pubsub_api;
using protocol_api = forge::plugins::net::p2p::node::protocol_api;
using protocol_registration = forge::plugins::net::p2p::node::protocol_registration;
static_assert(!std::copy_constructible<protocol_registration>);
static_assert(std::movable<protocol_registration>);
static_assert(std::same_as<decltype(std::declval<protocol_api&>().register_protocol(
    p2p::protocol_id{}, p2p::node::protocol_handler{})), protocol_registration>);
static_assert(std::same_as<decltype(std::declval<protocol_api&>().open(
    p2p::peer_id{}, p2p::protocol_id{}, p2p::node::open_options{})), boost::asio::awaitable<p2p::stream>>);

static_assert(std::same_as<decltype(std::declval<pubsub_api&>().enable_partial(
    p2p::pubsub::topic{}, node_plugin::handler{}, p2p::pubsub::partial_options{})),
    boost::asio::awaitable<p2p::pubsub::partial_topic>>);
static_assert(std::same_as<decltype(std::declval<pubsub_api&>().disable_partial(p2p::pubsub::partial_topic{})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<pubsub_api&>().partial_peers(p2p::pubsub::partial_topic{})),
                          boost::asio::awaitable<std::vector<p2p::peer_id>>>);
static_assert(std::same_as<decltype(std::declval<pubsub_api&>().advertise_partial(p2p::pubsub::partial_topic{}, {1})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<pubsub_api&>().forget_partial(p2p::pubsub::partial_topic{}, {1})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<pubsub_api&>().send_partial(
    p2p::pubsub::partial_topic{}, p2p::peer_id{}, p2p::pubsub::partial_message{}, std::stop_token{})),
                          boost::asio::awaitable<void>>);

static_assert(std::same_as<decltype(std::declval<const events_api&>().reachability_status()), p2p::host_event>);
static_assert(std::same_as<decltype(std::declval<const events_api&>().host_events()), p2p::host_event_subscription>);
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

// Instantiate both typed paths against the installed public dependency closure.
boost::asio::awaitable<void> typed_pubsub_consumer(pubsub_api& api) {
   static_cast<void>(co_await api.publish(p2p::pubsub::topic{"package"}, std::uint64_t{42}));
   const auto token = co_await api.subscribe<std::uint64_t>(p2p::pubsub::topic{"package"},
       [](node_plugin::typed_message<std::uint64_t>) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
          co_return p2p::pubsub::validation_result::accept;
       });
   co_await api.unsubscribe(token);
}

int main() {
   const auto descriptor = forge::plugins::net::p2p::node::descriptor();
   const auto config = forge::plugins::net::p2p::node::config{};
   const auto dht = dht_api::describe();
   const auto configuration = configuration_api::describe();
   const auto events = events_api::describe();
   const auto diagnostics = diagnostics_api::describe();
   const auto pubsub = pubsub_api::describe();
   const auto protocol = protocol_api::describe();
   auto pubsub_config = node_plugin::pubsub_config{};
   pubsub_config.partial_messages = true;
   forge::schema::rules<node_plugin::pubsub_config>::define().apply_defaults(pubsub_config);
   return descriptor.id.value == "forge.plugins.net.p2p.node" &&
                  config.topology_mode == forge::plugins::net::p2p::node::topology_mode::managed &&
                  config.topology_target == 160 && dht.id.value == "forge.plugins.net.p2p.node.dht" &&
                  dht.version.major == 1 && dht.version.revision == 0 &&
                  dht.supported_surfaces == forge::api::core::surface::local && dht.methods.empty() &&
                  configuration.id.value == "forge.plugins.net.p2p.node.configuration" &&
                  configuration.version.major == 1 && configuration.version.revision == 0 &&
                  configuration.supported_surfaces == forge::api::core::surface::local && configuration.methods.empty() &&
                  events.id.value == "forge.plugins.net.p2p.node.events" &&
                  events.version.major == 1 && events.version.revision == 0 &&
                  events.supported_surfaces == forge::api::core::surface::local && events.methods.empty() &&
                  diagnostics.id.value == "forge.plugins.net.p2p.node.diagnostics" &&
                  diagnostics.version.major == 1 && diagnostics.version.revision == 0 &&
                  diagnostics.supported_surfaces == forge::api::core::surface::local && diagnostics.methods.empty() &&
                  pubsub.id.value == "forge.plugins.net.p2p.node.pubsub" &&
                  pubsub.version.major == 1 && pubsub.version.revision == 0 &&
                  pubsub.supported_surfaces == forge::api::core::surface::local && pubsub.methods.empty() &&
                  node_plugin::plugin{}.version() == "8.0.0" &&
                  !pubsub_config.enabled && !pubsub_config.partial_messages &&
                  forge::plugins::net::p2p::node::api::describe().version.major == 3 &&
                  protocol.id.value == "forge.plugins.net.p2p.node.protocol" &&
                  protocol.version.major == 1 && protocol.version.revision == 0 &&
                  protocol.supported_surfaces == forge::api::core::surface::local && protocol.methods.empty()
              ? 0
              : 1;
}
