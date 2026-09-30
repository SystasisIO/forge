#include <concepts>
#include <utility>

import forge.api.core.binding;
import forge.api.p2p.publication;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.managed_api;
import forge.plugins.net.p2p.resolver.plugin;

static_assert(std::same_as<
              decltype(std::declval<forge::plugins::net::p2p::resolver::api&>().publish_api(
                  std::declval<forge::api::core::binding_plan>(), std::declval<forge::net::p2p::protocol_id>())),
              forge::api::p2p::publication>);

int main() {
   const auto descriptor = forge::plugins::net::p2p::resolver::managed_api::describe();
   const auto plugin = forge::plugins::net::p2p::resolver::descriptor();
   return descriptor.id.value == "forge.plugins.net.p2p.resolver.managed" && plugin.id.value == "forge.plugins.net.p2p.resolver"
              ? 0
              : 1;
}
