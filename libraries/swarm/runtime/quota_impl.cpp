module;

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

module forge.swarm.runtime.quota;

#include "details/quota_impl.hxx"

namespace forge::swarm::runtime {

quota::impl::impl(options value) : config{std::move(value)} { state.available = config.bytes; }

} // namespace forge::swarm::runtime
