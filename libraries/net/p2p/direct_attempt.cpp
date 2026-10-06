module;

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <optional>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;

#include "details/direct_attempt.hxx"
#include "details/session_lifecycle.hxx"

namespace forge::net::p2p::detail {

direct_attempt::direct_attempt(direct_attempt&& other) noexcept
    : connection(std::move(other.connection)), resources(std::move(other.resources)), target(std::move(other.target)),
      started_at(other.started_at) {}

direct_attempt& direct_attempt::operator=(direct_attempt&& other) noexcept {
   if (this != &other) {
      reset();
      connection = std::move(other.connection);
      resources = std::move(other.resources);
      target = std::move(other.target);
      started_at = other.started_at;
   }
   return *this;
}

direct_attempt::~direct_attempt() {
   reset();
}

void direct_attempt::reset() noexcept {
   request_session_cancel(connection.session);
   // Dropping the session after its cancel request transfers the final native
   // cleanup to the transport. Its shared resource state retains the teardown
   // ticket until that terminal worker releases it.
   connection.session = {};
   connection.native_lifetime.reset();
   resources.reset();
}

} // namespace forge::net::p2p::detail
