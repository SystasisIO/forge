module;

#include <cstdint>

module forge.plugins.net.tlsnotary.session.config;

import forge.schema.object;
forge::schema::object_schema<forge::plugins::net::tlsnotary::session::config>
forge::schema::rules<forge::plugins::net::tlsnotary::session::config>::define() {
   using config = forge::plugins::net::tlsnotary::session::config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::worker_path>("worker-path")
       .default_value("")
       .description("Absolute path to the trusted Forge TLSNotary native worker");
   schema.field<&config::max_active>("max-active").default_value(std::uint32_t{2}).range(1, 64);
   schema.field<&config::max_queued>("max-queued").default_value(std::uint32_t{8}).range(0, 1024);
   return schema;
}
