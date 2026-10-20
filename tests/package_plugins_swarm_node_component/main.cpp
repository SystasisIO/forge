#include <memory>
#include <type_traits>

import forge.plugins.swarm.node.plugin;
import forge.plugins.swarm.node.api;
import forge.plugins.swarm.node.config;
import forge.plugins.swarm.node.exceptions;

int main() {
   namespace swarm = forge::plugins::swarm::node;
   static_assert(std::is_abstract_v<swarm::api>);
   static_assert(!std::is_copy_constructible_v<forge::swarm::runtime::sharing>);
   auto options = swarm::plugin_options{};
   options.initial_config.free_admission = true;
   const auto descriptor = swarm::plugin::descriptor(options);
   if (descriptor.id.value != "forge.plugins.swarm.node" || descriptor.dependencies.size() != 2U) return 1;
   const auto owner = descriptor.factory();
   if (!owner || owner->id() != descriptor.id || owner->version() != "1.0.0") return 2;
   const auto config = owner->describe_config();
   if (!config || config->section != "plugins.swarm.node" || config->fields.empty()) return 3;
   return 0;
}
