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
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.types;

static_assert(std::same_as<
              decltype(std::declval<forge::plugins::net::p2p::node::api&>().publish_api(
                  std::declval<forge::api::core::binding_plan>(), std::declval<forge::net::p2p::protocol_id>())),
              forge::api::p2p::publication>);

namespace p2p = forge::net::p2p;
using dht_api = forge::plugins::net::p2p::node::dht_api;

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
   return descriptor.id.value == "forge.plugins.net.p2p.node" &&
                  config.topology_mode == forge::plugins::net::p2p::node::topology_mode::managed &&
                  config.topology_target == 160 && dht.id.value == "forge.plugins.net.p2p.node.dht" &&
                  dht.version.major == 1 && dht.version.revision == 0 &&
                  dht.supported_surfaces == forge::api::core::surface::local && dht.methods.empty()
              ? 0
              : 1;
}
