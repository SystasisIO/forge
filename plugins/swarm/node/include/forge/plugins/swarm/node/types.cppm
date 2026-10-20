module;

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <optional>

export module forge.plugins.swarm.node.types;

export import forge.plugins.swarm.node.config;
export import forge.swarm.runtime.engine;

export namespace forge::plugins::swarm::node {

struct plugin_options {
   config initial_config;
   forge::swarm::runtime::engine::options engine_options;
   std::shared_ptr<forge::swarm::runtime::admission> admission;
   std::optional<forge::asio::compute::executor> cpu;
   // Required in object mode: caller-owned isolated executor for blocking DB flush.
   boost::asio::any_io_executor database;
};

} // namespace forge::plugins::swarm::node
