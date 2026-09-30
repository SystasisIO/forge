#include <concepts>
#include <utility>

import forge.api.core.binding;
import forge.api.p2p.publication;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.node.types;

static_assert(std::same_as<
              decltype(std::declval<forge::plugins::net::p2p::node::api&>().publish_api(
                  std::declval<forge::api::core::binding_plan>(), std::declval<forge::net::p2p::protocol_id>())),
              forge::api::p2p::publication>);

int main() {
   const auto descriptor = forge::plugins::net::p2p::node::descriptor();
   const auto config = forge::plugins::net::p2p::node::config{};
   return descriptor.id.value == "forge.plugins.net.p2p.node" &&
                  config.topology_mode == forge::plugins::net::p2p::node::topology_mode::managed &&
                  config.topology_target == 160
              ? 0
              : 1;
}
