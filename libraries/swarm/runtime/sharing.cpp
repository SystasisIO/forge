module;

#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.swarm.runtime.engine;

import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"

namespace forge::swarm::runtime {

sharing::sharing() = default;
sharing::sharing(std::shared_ptr<detail::operation_state> value) : _state{std::move(value)} {}
sharing::~sharing() { cancel(); }
sharing::sharing(sharing&& other) noexcept : _state{std::exchange(other._state, {})} {}
sharing& sharing::operator=(sharing&& other) noexcept {
   if (this != &other) { cancel(); _state = std::exchange(other._state, {}); }
   return *this;
}
void sharing::cancel() noexcept { if (_state) _state->cancel(); }
operation_status sharing::inspect() const { return _state ? _state->inspect() : operation_status{.closed = true}; }
boost::asio::awaitable<void> sharing::async_close() {
   return detail::operation_state::close(_state);
}

} // namespace forge::swarm::runtime
