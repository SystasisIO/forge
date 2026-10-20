module;

#include <forge/exceptions/macros.hpp>
#include <cstdint>
#include <string>

module forge.plugins.swarm.node.config;

import forge.plugins.swarm.node.exceptions;

forge::schema::object_schema<forge::plugins::swarm::node::network_config>
forge::schema::rules<forge::plugins::swarm::node::network_config>::define() {
   using type = forge::plugins::swarm::node::network_config;
   auto schema = forge::schema::object<type>();
   schema.field<&type::profile>("profile").default_value("/ipfs/kad/1.0.0");
   schema.field<&type::application_protocol>("application-protocol").default_value("/forge/swarm/1.0.0");
   return schema;
}

forge::schema::object_schema<forge::plugins::swarm::node::metadata_config>
forge::schema::rules<forge::plugins::swarm::node::metadata_config>::define() {
   using type = forge::plugins::swarm::node::metadata_config;
   auto schema = forge::schema::object<type>();
   schema.field<&type::mode>("mode").default_value("file");
   schema.field<&type::object_name>("object-name").default_value("swarm");
   schema.field<&type::max_inflight>("max-inflight").default_value(std::uint32_t{8}).range(1, 32);
   return schema;
}

forge::schema::object_schema<forge::plugins::swarm::node::config>
forge::schema::rules<forge::plugins::swarm::node::config>::define() {
   using type = forge::plugins::swarm::node::config;
   auto schema = forge::schema::object<type>();
   schema.field<&type::network>("network");
   schema.field<&type::metadata>("metadata");
   schema.field<&type::free_admission>("free-admission").default_value(false);
   return schema;
}

namespace forge::plugins::swarm::node {

void validate_config(const config& value) {
   if (value.network.profile.empty() || value.network.profile.front() != '/' || value.network.profile.size() > 128U ||
       value.network.profile.find('\0') != std::string::npos || value.network.application_protocol != "/forge/swarm/1.0.0") {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm needs a bounded DHT profile and the supported v1 application protocol");
   }
   if ((value.metadata.mode != "file" && value.metadata.mode != "object") || value.metadata.max_inflight == 0U ||
       value.metadata.max_inflight > 32U || (value.metadata.mode == "object" &&
       (value.metadata.object_name.empty() || value.metadata.object_name.size() > 128U ||
        value.metadata.object_name.find('\0') != std::string::npos))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm metadata needs file/object mode and bounded named-store admission");
   }
}

} // namespace forge::plugins::swarm::node
