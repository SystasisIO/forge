#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.p2p.publication;
import forge.api.transport.connection;
import forge.api.transport.options;
import forge.app.application_shell;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.app.plugin_registry;
import forge.asio.task;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.resolver.plugin;

#include "details/resolver_publication_race.hxx"

namespace forge::tests::plugins {

void resolver_publish_barrier::bind_owner_executor(boost::asio::any_io_executor executor) {
   const auto lock = std::scoped_lock{mutex_};
   owner_executor_ = std::move(executor);
}

void resolver_publish_barrier::arm() {
   const auto lock = std::scoped_lock{mutex_};
   armed_ = true;
   entered_ = false;
   released_ = false;
}

bool resolver_publish_barrier::wait_until_entered(std::chrono::milliseconds timeout) {
   auto lock = std::unique_lock{mutex_};
   return changed_.wait_for(lock, timeout, [this] { return entered_; });
}

void resolver_publish_barrier::release() {
   {
      const auto lock = std::scoped_lock{mutex_};
      released_ = true;
   }
   changed_.notify_all();
}

void resolver_publish_barrier::wait_if_armed() {
   auto lock = std::unique_lock{mutex_};
   if (!armed_) {
      return;
   }
   armed_ = false;
   entered_ = true;
   changed_.notify_all();
   changed_.wait(lock, [this] { return released_; });
}

boost::asio::any_io_executor resolver_publish_barrier::owner_executor() const {
   const auto lock = std::scoped_lock{mutex_};
   return owner_executor_;
}

void resolver_publish_barrier::track_protocol(std::string protocol) {
   const auto lock = std::scoped_lock{mutex_};
   ++publish_calls_[protocol];
   drain_calls_.try_emplace(protocol, 0U);
   close_calls_.try_emplace(std::move(protocol), 0U);
}

void resolver_publish_barrier::record_close(const std::string& protocol) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   if (const auto found = close_calls_.find(protocol); found != close_calls_.end()) {
      ++found->second;
   }
}

std::size_t resolver_publish_barrier::close_calls(const std::string& protocol) const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   const auto found = close_calls_.find(protocol);
   return found == close_calls_.end() ? 0U : found->second;
}

void resolver_publish_barrier::record_drain(const std::string& protocol) noexcept {
   const auto lock = std::scoped_lock{mutex_};
   if (const auto found = drain_calls_.find(protocol); found != drain_calls_.end()) {
      ++found->second;
   }
}

std::size_t resolver_publish_barrier::drain_calls(const std::string& protocol) const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   const auto found = drain_calls_.find(protocol);
   return found == drain_calls_.end() ? 0U : found->second;
}

std::size_t resolver_publish_barrier::publish_calls(const std::string& protocol) const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   const auto found = publish_calls_.find(protocol);
   return found == publish_calls_.end() ? 0U : found->second;
}

resolver_publication_race_node::resolver_publication_race_node(std::shared_ptr<resolver_publish_barrier> barrier)
    : barrier_{std::move(barrier)} {}

forge::net::p2p::peer_id resolver_publication_race_node::local_peer() const {
   return {.value = "resolver-publication-race"};
}

std::optional<forge::net::p2p::endpoint> resolver_publication_race_node::local_endpoint() const {
   return std::nullopt;
}

std::vector<forge::net::p2p::endpoint> resolver_publication_race_node::local_endpoints() const {
   return {};
}

forge::plugins::net::p2p::node::info resolver_publication_race_node::network_info() const {
   return {.local_peer = local_peer(), .local_endpoints = {}, .started = true};
}

forge::api::p2p::publication
resolver_publication_race_node::publish_api(forge::api::core::binding_plan plan,
                                            forge::net::p2p::protocol_id protocol) {
   return publish_api(std::move(plan), std::move(protocol), {});
}

forge::api::p2p::publication
resolver_publication_race_node::publish_api(forge::api::core::binding_plan plan,
                                            forge::net::p2p::protocol_id protocol,
                                            forge::api::transport::options options) {
   static_cast<void>(plan);
   static_cast<void>(options);
   auto published_protocol = protocol.value;
   barrier_->track_protocol(published_protocol);
   barrier_->wait_if_armed();
   return forge::api::p2p::detail::publication_access::make(
      barrier_->owner_executor(),
      [barrier = barrier_, published_protocol] {
         barrier->record_close(published_protocol);
      },
      [barrier = barrier_, published_protocol]() -> boost::asio::awaitable<void> {
         barrier->record_drain(published_protocol);
         co_return;
      },
      [] { return true; });
}

void resolver_publication_race_node::publish_protocol(forge::net::p2p::protocol_id protocol,
                                                       forge::net::p2p::node::protocol_handler handler) {
   static_cast<void>(protocol);
   static_cast<void>(handler);
}

boost::asio::awaitable<forge::api::transport::connection>
resolver_publication_race_node::open_api_connection(forge::net::p2p::peer_id peer,
                                                     forge::net::p2p::protocol_id protocol,
                                                     forge::plugins::net::p2p::node::remote_options options) {
   static_cast<void>(peer);
   static_cast<void>(protocol);
   static_cast<void>(options);
   throw std::logic_error{"resolver publication race fixture does not open connections"};
   co_return forge::api::transport::connection{};
}

resolver_publication_race_node_plugin::resolver_publication_race_node_plugin(
   std::shared_ptr<resolver_publish_barrier> barrier)
    : barrier_{std::move(barrier)} {}

forge::app::plugin_id resolver_publication_race_node_plugin::id() const {
   return {.value = "forge.plugins.net.p2p.node"};
}

std::string resolver_publication_race_node_plugin::version() const {
   return "test";
}

boost::asio::awaitable<void>
resolver_publication_race_node_plugin::provide(forge::api::core::provider& provider) {
   provider.install<forge::plugins::net::p2p::node::api>(std::make_shared<resolver_publication_race_node>(barrier_));
   co_return;
}

boost::asio::awaitable<void>
resolver_publication_race_node_plugin::initialize(forge::app::plugin_context& context) {
   barrier_->bind_owner_executor(context.scheduler().runtime_context().context().get_executor());
   co_return;
}

boost::asio::awaitable<void> resolver_publication_race_node_plugin::startup() {
   co_return;
}

boost::asio::awaitable<void> resolver_publication_race_node_plugin::shutdown() {
   co_return;
}

resolver_publication_race_application::resolver_publication_race_application(
   std::shared_ptr<resolver_publish_barrier> barrier,
   forge::app::application_shell_options options)
    : application_shell{std::move(options)}, barrier_{std::move(barrier)} {}

void resolver_publication_race_application::on_register_plugins(forge::app::plugin_registry& registry) {
   registry.register_plugin(forge::app::plugin_descriptor{
       .id = {.value = "forge.plugins.net.p2p.node"},
       .factory = [barrier = barrier_] { return std::make_unique<resolver_publication_race_node_plugin>(barrier); },
   });
   registry.register_plugin(forge::plugins::net::p2p::resolver::descriptor());
}

} // namespace forge::tests::plugins
