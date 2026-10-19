module;

#include <boost/describe.hpp>
#include <cstdint>
#include <string>

export module forge.plugins.net.tlsnotary.session.config;

import forge.schema.object;

export namespace forge::plugins::net::tlsnotary::session {

struct config {
   std::string worker_path;
   std::uint32_t max_active = 2;
   std::uint32_t max_queued = 8;
};

BOOST_DESCRIBE_STRUCT(config, (), (worker_path, max_active, max_queued))
} // namespace forge::plugins::net::tlsnotary::session
export template <> struct forge::schema::rules<forge::plugins::net::tlsnotary::session::config> {
   static forge::schema::object_schema<forge::plugins::net::tlsnotary::session::config> define();
};
