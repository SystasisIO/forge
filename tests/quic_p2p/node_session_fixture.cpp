module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <stop_token>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include "libp2p_identity_fixture.hxx"
#include "pubsub_claim_allocation.hxx"

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.exceptions;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/node_impl.hxx"
#include "../../libraries/net/p2p/details/pubsub_peer_score.hxx"
#include "../../libraries/net/p2p/details/pubsub_router.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;

node_session_fixture::terminal_transport::terminal_transport() = default;

node_session_fixture::terminal_transport::terminal_transport(
    terminal_result result, std::shared_ptr<std::promise<void>> entered,
    std::shared_ptr<forge::asio::notification> barrier, std::shared_ptr<std::atomic_bool> barrier_passed,
    std::shared_ptr<detail::session_teardown::ticket> native_ticket,
    resource_manager::memory_reservation memory, resource_manager::file_descriptor_reservation descriptor)
    : _result(result), _entered(std::move(entered)), _barrier(std::move(barrier)),
      _barrier_epoch(_barrier->epoch()), _barrier_passed(std::move(barrier_passed)),
      _native_ticket(std::move(native_ticket)), _memory(std::move(memory)), _descriptor(std::move(descriptor)) {}

node_session_fixture::terminal_transport::~terminal_transport() = default;

bool node_session_fixture::terminal_transport::valid() const noexcept { return _open.load(); }

boost::asio::awaitable<forge::net::transport::stream> node_session_fixture::terminal_transport::async_open_stream() {
   FORGE_THROW_EXCEPTION(exceptions::closed, "terminal fixture has no streams");
   co_return forge::net::transport::stream{};
}

boost::asio::awaitable<forge::net::transport::stream> node_session_fixture::terminal_transport::async_accept_stream() {
   FORGE_THROW_EXCEPTION(exceptions::closed, "terminal fixture has no streams");
   co_return forge::net::transport::stream{};
}

boost::asio::awaitable<void> node_session_fixture::terminal_transport::async_close() {
   if (_barrier) {
      _entered->set_value();
      co_await _barrier->async_wait(_barrier_epoch);
      _barrier_passed->store(true);
   }
   _open = false;
   switch (_result) {
   case terminal_result::success: break;
   case terminal_result::transport_closed:
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "terminal fixture closed");
   case terminal_result::transport_canceled:
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::canceled, "terminal fixture canceled");
   case terminal_result::yamux_closed:
      FORGE_THROW_EXCEPTION(forge::net::yamux::exceptions::closed, "terminal fixture Yamux closed");
   case terminal_result::yamux_canceled:
      FORGE_THROW_EXCEPTION(forge::net::yamux::exceptions::canceled, "terminal fixture Yamux canceled");
   case terminal_result::yamux_protocol:
      FORGE_THROW_EXCEPTION(forge::net::yamux::exceptions::protocol_error, "terminal fixture Yamux protocol");
   case terminal_result::transport_protocol:
      FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::protocol_error, "terminal fixture transport protocol");
   case terminal_result::io:
      throw std::system_error{std::make_error_code(std::errc::io_error), "terminal fixture I/O"};
   case terminal_result::allocation: throw std::bad_alloc{};
   }
   co_return;
}

void node_session_fixture::terminal_transport::cancel() { _open = false; }

node_session_fixture::message_input::message_input(std::vector<std::uint8_t> bytes) : _bytes(std::move(bytes)) {}

bool node_session_fixture::message_input::valid() const noexcept { return true; }

std::int64_t node_session_fixture::message_input::id() const noexcept { return 7; }

boost::asio::awaitable<void> node_session_fixture::message_input::async_write(std::span<const std::uint8_t>) {
   co_return;
}

boost::asio::awaitable<std::vector<std::uint8_t>> node_session_fixture::message_input::async_read() {
   if (!_bytes.empty()) { co_return std::exchange(_bytes, {}); }
   FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "message fixture exhausted");
   co_return std::vector<std::uint8_t>{};
}

boost::asio::awaitable<void> node_session_fixture::message_input::async_close() { co_return; }

void node_session_fixture::message_input::cancel() {}

node::options node_session_fixture::options(std::string_view name) {
   const auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto value = node::options{.certificate_pem = identity.certificate_pem,
      .private_key_pem = identity.private_key_pem, .capabilities = {.bits = capabilities::pubsub}};
   value.peer_state.persistence = peer_store::make_memory_persistence();
   value.dht_profiles.clear();
   value.limits.pubsub.scoring.emplace();
   auto& scoring = *value.limits.pubsub.scoring;
   auto topic = pubsub::topic_score_params{};
   topic.time_in_mesh_weight = 1;
   topic.time_in_mesh_quantum = 1s;
   scoring.topics.emplace(pubsub::topic{"forge.pubsub.terminal"}, topic);
   scoring.retain_score = 10s;
   scoring.decay_interval = 1s;
   value.limits.pubsub.limits.history_length = 1;
   value.limits.pubsub.limits.history_gossip = 1;
   value.limits.pubsub.limits.max_messages = 1;
   value.limits.session_grace_period = 0ms;
   value.limits.session_prune_silence = 1ms;
   return value;
}

peer_id node_session_fixture::peer(std::uint8_t value) {
   return make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, value)});
}

void node_session_fixture::seed(node& owner, const peer_id& peer, std::uint64_t id) {
   const auto self = owner.impl_;
   auto session = std::make_shared<node::impl::session_state>();
   session->id = id;
   session->info.id = id;
   session->info.remote_peer = peer;
   session->info.path = path::kind::direct;
   session->authentication = peer_authentication::noise;
   session->connection = forge::net::transport::detail::session_access::make(std::make_shared<terminal_transport>());
   const auto lock = std::scoped_lock{self->mutex};
   const auto now = std::chrono::steady_clock::now();
   const auto past = now - 2s;
   if (self->pubsub_value.peers.empty()) {
      // Test setup uses explicit old timestamps; production clocks and retirement remain unchanged.
      self->pubsub_value.scoring = std::make_shared<detail::pubsub_peer_score>(*self->options.limits.pubsub.scoring, past);
   }
   BOOST_REQUIRE(self->sessions.emplace(id, session).second);
   const auto admitted = self->connections.remember(connection_manager::session_record{
      .id = id, .peer = peer, .opened_at = past, .last_used_at = past}, past);
   BOOST_REQUIRE(admitted.accepted);
   BOOST_REQUIRE(admitted.pruned.empty());
   if (!self->pubsub_value.peers.contains(peer)) {
      BOOST_REQUIRE(self->pubsub_value.scoring->connect(peer, {}, past));
      BOOST_REQUIRE(self->pubsub_value.scoring->graft(peer, std::string_view{"forge.pubsub.terminal"}, past));
      auto& row = self->pubsub_value.peers[peer];
      row.generation = self->pubsub_value.next_peer_generation++;
      self->pubsub_value.scores.try_emplace(peer);
      self->pubsub_value.mesh["forge.pubsub.terminal"].insert(peer);
   }
   // Two legal decay quanta make P1 positive and cross the negative fixture's 1s activation.
   self->pubsub_value.scoring->tick(now);
   self->next_session_id = std::max(self->next_session_id, id + 1);
   self->metrics_value.active_sessions = self->sessions.size();
}

void node_session_fixture::outbound(node& owner, const peer_id& peer, std::uint64_t id) {
   const auto self = owner.impl_;
   const auto lock = std::scoped_lock{self->mutex};
   self->pubsub_value.outbound.emplace(peer, node::impl::pubsub_state::outbound_generation{
      .session_id = id, .generation = 1, .protocol = builtins::meshsub_v11,
      .write_gate = std::make_shared<forge::asio::gate>(), .stream = std::make_shared<stream>()});
}

void node_session_fixture::admit(forge::asio::runtime& runtime, node& owner, const peer_id& peer, std::uint64_t id) {
   const auto self = owner.impl_;
   auto session = std::make_shared<node::impl::session_state>();
   session->id = id;
   session->info.id = id;
   session->info.remote_peer = peer;
   session->info.path = path::kind::direct;
   session->authentication = peer_authentication::noise;
   session->connection = forge::net::transport::detail::session_access::make(std::make_shared<terminal_transport>());
   auto resource = self->resources.reserve_session(resource_manager::session_direction::outbound);
   BOOST_REQUIRE(static_cast<bool>(resource));
   session->resource = std::move(*resource);
   forge::asio::blocking::run(runtime, self->remember_session(std::move(session), connection_manager::direction::outbound));
}

stream node_session_fixture::input(const pubsub::message& value) {
   return stream{forge::net::transport::detail::stream_access::make(
      std::make_shared<message_input>(pubsub::codec::encode(pubsub::rpc{.messages = {value}})))};
}

} // namespace forge::net::p2p
