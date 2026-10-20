module;

#include <boost/describe.hpp>
#include <cstdint>
#include <string>

export module forge.plugins.swarm.node.config;

import forge.schema.object;

export namespace forge::plugins::swarm::node {

struct network_config {
   std::string profile = "/ipfs/kad/1.0.0";
   std::string application_protocol = "/forge/swarm/1.0.0";
};

struct metadata_config {
   std::string mode = "file";
   std::string object_name = "swarm";
   std::uint32_t max_inflight = 8;
};

struct config {
   network_config network;
   metadata_config metadata;
   bool free_admission = false;
};

void validate_config(const config&);

BOOST_DESCRIBE_STRUCT(network_config, (), (profile, application_protocol))
BOOST_DESCRIBE_STRUCT(metadata_config, (), (mode, object_name, max_inflight))
BOOST_DESCRIBE_STRUCT(config, (), (network, metadata, free_admission))

} // namespace forge::plugins::swarm::node

export template <> struct forge::schema::rules<forge::plugins::swarm::node::network_config> {
   static forge::schema::object_schema<forge::plugins::swarm::node::network_config> define();
};
export template <> struct forge::schema::rules<forge::plugins::swarm::node::metadata_config> {
   static forge::schema::object_schema<forge::plugins::swarm::node::metadata_config> define();
};
export template <> struct forge::schema::rules<forge::plugins::swarm::node::config> {
   static forge::schema::object_schema<forge::plugins::swarm::node::config> define();
};
