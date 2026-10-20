module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stop_token>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.admission;

namespace forge::swarm::runtime {

admission::~admission() = default;
boost::asio::awaitable<std::shared_ptr<quota>> free_admission::async_authorize(
   core::swarm_id content, forge::api::auth::authenticated_caller caller, access_kind, std::stop_token stop,
   std::chrono::steady_clock::time_point deadline) {
   if (!caller.transport_authenticated() || stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm free policy requires trusted live invocation");
   }
   co_return std::make_shared<quota>(quota::options{content, caller, std::numeric_limits<std::uint64_t>::max()});
}

} // namespace forge::swarm::runtime
