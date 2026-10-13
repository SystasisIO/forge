module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.asio.notification;
import forge.asio.runtime;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.protocol_registration;

#include "../../../plugins/net/p2p/node/details/protocol_registry.hxx"
#include "../../../plugins/net/p2p/node/details/protocol_operations.hxx"
#include "details/protocol_registry_fixture.hxx"

namespace forge::plugins::net::p2p::node::test {
using namespace std::chrono_literals;

protocol_registry_fixture::job::job(protocol_registry_fixture& fixture, std::function<void()> body)
    : fixture_{fixture} {
   auto task = std::packaged_task<void()>{std::move(body)};
   done_ = task.get_future();
   thread_ = std::thread{std::move(task)};
}
protocol_registry_fixture::job::~job() {
   fixture_.release_install();
   if (thread_.joinable()) { wait(done_); thread_.join(); }
}
void protocol_registry_fixture::job::finish() {
   wait(done_);
   thread_.join();
   done_.get();
}
void protocol_registry_fixture::wait(std::future<void>& done) {
   if (done.wait_for(10s) != std::future_status::ready) {
      std::cerr << "protocol registry fixture did not join its operation\n";
      std::terminate();
   }
}

protocol_registry_fixture::protocol_registry_fixture(std::size_t max_slots)
    : owner{std::make_shared<detail::protocol_registry>(max_slots)}, state_{std::make_shared<state>()}, runtime_{} {
   operations = std::make_shared<const detail::protocol_operations>(detail::protocol_operations{
       .install = [state = state_](auto protocol, auto handler) { install(state, std::move(protocol), std::move(handler)); },
       .remove = [state = state_](const auto& protocol) { return remove(state, protocol); },
   });
   owner->bind_executor(runtime_.context().get_executor());
}
protocol_registry_fixture::~protocol_registry_fixture() {
   release_install();
   fail_install(point::none);
   fail_remove(point::none);
   owner->request_close();
   run(owner->async_close());
   owner.reset();
   operations.reset();
}

void protocol_registry_fixture::fail_install(point value) {
   const auto lock = std::scoped_lock{state_->mutex};
   state_->install_error = value;
}
void protocol_registry_fixture::fail_remove(point value) {
   const auto lock = std::scoped_lock{state_->mutex};
   state_->remove_error = value;
}
void protocol_registry_fixture::hold_install(point value) {
   const auto lock = std::scoped_lock{state_->mutex};
   state_->pause = value;
   state_->entered = state_->released = false;
}
void protocol_registry_fixture::wait_install() {
   auto lock = std::unique_lock{state_->mutex};
   if (!state_->ready.wait_for(lock, 5s, [&] { return state_->entered; })) {
      throw std::runtime_error{"install did not reach its held boundary"};
   }
}
void protocol_registry_fixture::release_install() noexcept {
   { const auto lock = std::scoped_lock{state_->mutex}; state_->released = true; }
   state_->ready.notify_all();
}
void protocol_registry_fixture::pause(const std::shared_ptr<state>& state, point value) {
   auto lock = std::unique_lock{state->mutex};
   if (state->pause != value) { return; }
   state->entered = true;
   state->ready.notify_all();
   if (!state->ready.wait_for(lock, 10s, [&] { return state->released; })) {
      throw std::runtime_error{"held install was not released"};
   }
}
void protocol_registry_fixture::install(const std::shared_ptr<state>& state,
    forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) {
   {
      const auto lock = std::scoped_lock{state->mutex};
      ++state->installs;
      if (state->install_error == point::before) { throw install_failure{"controlled install failure"}; }
   }
   pause(state, point::before);
   {
      const auto lock = std::scoped_lock{state->mutex};
      if (!state->routes.emplace(std::move(protocol), std::move(handler)).second) {
         throw std::logic_error{"duplicate boundary installation"};
      }
   }
   pause(state, point::after);
   const auto lock = std::scoped_lock{state->mutex};
   if (state->install_error == point::after) { throw install_failure{"controlled install failure"}; }
}
bool protocol_registry_fixture::remove(const std::shared_ptr<state>& state,
    const forge::net::p2p::protocol_id& protocol) {
   auto retired = decltype(state->routes)::node_type{};
   auto failure = false;
   {
      const auto lock = std::scoped_lock{state->mutex};
      ++state->removals;
      if (state->remove_error == point::before) { throw remove_failure{"controlled remove failure"}; }
      retired = state->routes.extract(protocol);
      state->remove_result = !retired.empty();
      failure = state->remove_error == point::after;
   }
   if (failure) { throw remove_failure{"controlled remove failure"}; }
   return !retired.empty();
}
bool protocol_registry_fixture::contains(const forge::net::p2p::protocol_id& protocol) const {
   const auto lock = std::scoped_lock{state_->mutex};
   return state_->routes.contains(protocol);
}
std::size_t protocol_registry_fixture::installs() const {
   const auto lock = std::scoped_lock{state_->mutex}; return state_->installs;
}
std::size_t protocol_registry_fixture::removals() const {
   const auto lock = std::scoped_lock{state_->mutex}; return state_->removals;
}
std::optional<bool> protocol_registry_fixture::last_remove_result() const {
   const auto lock = std::scoped_lock{state_->mutex}; return state_->remove_result;
}
forge::api::p2p::publication protocol_registry_fixture::publish(forge::net::p2p::protocol_id protocol) {
   return owner->publish(forge::api::p2p::api_binding{nullptr,
       forge::api::core::binding().serve(apis_).build(), std::move(protocol), {}, {}, {}});
}
void protocol_registry_fixture::run(boost::asio::awaitable<void> operation) {
   auto done = boost::asio::co_spawn(runtime_.context(), std::move(operation), boost::asio::use_future);
   wait(done);
   done.get();
}
forge::net::p2p::node::protocol_handler protocol_registry_fixture::handler() {
   return [](forge::net::p2p::node::incoming_protocol_stream) -> boost::asio::awaitable<void> { co_return; };
}

}
