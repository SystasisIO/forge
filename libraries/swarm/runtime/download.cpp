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
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.engine;

import forge.asio.gate;
import forge.asio.notification;

#include "details/resource_budget.hxx"
#include "details/operation_state.hxx"

namespace forge::swarm::runtime {

download::download() = default;
download::download(std::shared_ptr<detail::operation_state> value) : _state{std::move(value)} {}
download::~download() { cancel(); }
download::download(download&& other) noexcept : _state{std::exchange(other._state, {})} {}
download& download::operator=(download&& other) noexcept {
   if (this != &other) { cancel(); _state = std::exchange(other._state, {}); }
   return *this;
}
void download::cancel() noexcept { if (_state) _state->cancel(); }
operation_status download::inspect() const { return _state ? _state->inspect() : operation_status{.closed = true}; }
boost::asio::awaitable<void> download::async_wait() {
   return detail::operation_state::wait(_state);
}
boost::asio::awaitable<void> download::async_close() {
   return detail::operation_state::close(_state);
}

} // namespace forge::swarm::runtime
