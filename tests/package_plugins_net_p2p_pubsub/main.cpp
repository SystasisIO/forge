#include <boost/asio/awaitable.hpp>
#include <concepts>
#include <cstdint>
#include <stop_token>
#include <utility>
#include <vector>

import forge.api.core.descriptor;
import forge.api.core.types;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.pubsub.api;
import forge.plugins.net.p2p.pubsub.plugin;
import forge.plugins.net.p2p.pubsub.types;
import forge.schema.object;

namespace facade = forge::plugins::net::p2p::pubsub;
namespace core = forge::net::p2p::pubsub;

static_assert(std::same_as<decltype(std::declval<facade::api&>().enable_partial(
    core::topic{}, facade::handler{}, core::partial_options{})), boost::asio::awaitable<core::partial_topic>>);
static_assert(std::same_as<decltype(std::declval<facade::api&>().disable_partial(core::partial_topic{})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<facade::api&>().partial_peers(core::partial_topic{})),
                          boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>>>);
static_assert(std::same_as<decltype(std::declval<facade::api&>().advertise_partial(core::partial_topic{}, {1})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<facade::api&>().forget_partial(core::partial_topic{}, {1})),
                          boost::asio::awaitable<void>>);
static_assert(std::same_as<decltype(std::declval<facade::api&>().send_partial(
    core::partial_topic{}, forge::net::p2p::peer_id{}, core::partial_message{}, std::stop_token{})),
                          boost::asio::awaitable<void>>);

int main() {
   const auto api = facade::api::describe();
   auto config = facade::config{};
   config.partial_messages = true;
   forge::schema::rules<facade::config>::define().apply_defaults(config);
   return facade::plugin{}.version() == "2.0.0" && !config.partial_messages &&
       api.id.value == "forge.plugins.net.p2p.pubsub" && api.version.major == 2 && api.version.revision == 0 &&
       api.supported_surfaces == forge::api::core::surface::local && api.methods.empty() ? 0 : 1;
}
