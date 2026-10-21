module;

#include <source_location>

#include <string>
#include <utility>

module forge.plugins.swarm.node.exceptions;

namespace forge::plugins::swarm::node {

exceptions::operation_failed::operation_failed(failures causes, std::string message,
   forge::exceptions::fields fields, std::source_location where)
   : coded_exception(std::move(message), std::move(fields), where), _causes{std::move(causes)} {}
const exceptions::failures& exceptions::operation_failed::causes() const noexcept { return _causes; }

} // namespace forge::plugins::swarm::node
