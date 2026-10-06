module;

#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/scope/scope_exit.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.session;
import forge.net.tcp.connection;
import forge.net.tls.options;
import forge.net.transport.stream;

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/libp2p_identity_material.hxx"
#include "../../libraries/net/p2p/details/length_delimited.hxx"
#include "../../libraries/net/p2p/details/lifecycle_wakeup.hxx"
#include "../../libraries/net/p2p/details/operation_deadline.hxx"
#include "../../libraries/net/p2p/details/path_manager.hxx"
#include "../../libraries/net/p2p/details/relay_transport.hxx"
#include "../../libraries/net/p2p/details/resource_stream.hxx"
#include "../../libraries/net/p2p/details/stream_upgrade.hxx"

namespace {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

p2p::node::options path_session_options(std::string_view name) {
   const auto identity = forge::tests::p2p::make_identity_fixture(name);
   return {.certificate_pem = identity.certificate_pem,
           .private_key_pem = identity.private_key_pem,
           .capabilities = {.bits = p2p::capabilities::hole_punching},
           .allow_insecure_test_mode = true};
}

boost::asio::awaitable<void> rejected_direct_exchange(forge::asio::runtime& runtime, p2p::node& client,
    p2p::peer_id peer, std::shared_ptr<std::atomic_bool> deadline_fired) {
   auto stream = std::make_shared<p2p::stream>(co_await client.async_open_protocol_stream(peer, p2p::builtins::dcutr,
       p2p::node::open_options{.allow_relay = false, .timeout = 2s, .allow_hole_punch = false}));
   auto timer = std::make_shared<boost::asio::steady_timer>(runtime.context());
   timer->expires_after(2s);
   timer->async_wait([stream, deadline_fired](auto error) {
      if (!error) { *deadline_fired = true; stream->request_cancel(); }
   });
   auto failure = std::exception_ptr{};
   try {
      co_await stream->async_write(p2p::hole_punch::codec::encode(p2p::hole_punch::message{}));
      static_cast<void>(co_await stream->async_read());
   } catch (...) { failure = std::current_exception(); }
   timer->cancel();
   stream->request_cancel();
   co_await stream->async_close();
   if (failure) { std::rethrow_exception(failure); }
}

boost::asio::awaitable<bool> cancel_admitted_path(p2p::node& client, p2p::peer_id peer) {
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   do {
      if (co_await client.async_cancel_hole_punch(peer)) { co_return true; }
      timer.expires_after(1ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

enum class ping_reply { split, stalled, mismatch, truncated, oversized };

struct ping_peer_state {
   std::promise<void> received;
   std::promise<void> terminal;
   std::atomic_size_t writes = 0;
};

boost::asio::awaitable<void> controlled_ping_peer(p2p::node::incoming_protocol_stream incoming,
    std::shared_ptr<ping_peer_state> state, ping_reply reply) {
   try {
      auto payload = std::vector<std::uint8_t>{};
      while (payload.size() != 32) {
         const auto part = co_await incoming.stream.async_read();
         if (part.empty() || part.size() > 32 - payload.size()) {
            throw std::runtime_error{"controlled Ping peer expected exactly 32 request bytes"};
         }
         payload.insert(payload.end(), part.begin(), part.end());
      }
      state->received.set_value();
      if (reply == ping_reply::stalled) {
         // Wait on the native stream rather than a test timer: only client
         // timeout/reset or session shutdown can terminate this read.
         static_cast<void>(co_await incoming.stream.async_read());
      } else if (reply == ping_reply::split) {
         auto pause = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         const auto bytes = std::span<const std::uint8_t>{payload};
         co_await incoming.stream.async_write(bytes.first(7));
         ++state->writes;
         pause.expires_after(40ms);
         co_await pause.async_wait(boost::asio::use_awaitable);
         co_await incoming.stream.async_write(bytes.subspan(7, 10));
         ++state->writes;
         pause.expires_after(40ms);
         co_await pause.async_wait(boost::asio::use_awaitable);
         co_await incoming.stream.async_write(bytes.subspan(17));
         ++state->writes;
      } else {
         if (reply == ping_reply::mismatch) { payload.front() ^= 1; }
         if (reply == ping_reply::truncated) { payload.resize(7); }
         if (reply == ping_reply::oversized) { payload.push_back(0); }
         co_await incoming.stream.async_write(payload);
         ++state->writes;
      }
   } catch (...) {
      // A timeout/shutdown is expected to wake the stalled peer's real read.
      // Tests also require the complete request observation before cancellation.
   }
   try { co_await incoming.stream.async_close(); }
   catch (...) { incoming.stream.request_cancel(); }
   state->terminal.set_value();
}

p2p::node::options ping_session_options(std::string_view name) {
   auto options = path_session_options(name);
   options.allow_insecure_test_mode = false;
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   return options;
}

struct native_ping_pair {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   p2p::node server{runtime, ping_session_options("path-ping-server")};
   p2p::node client{runtime, ping_session_options("path-ping-client")};
   std::shared_ptr<ping_peer_state> state = std::make_shared<ping_peer_state>();
   std::future<void> received = state->received.get_future();
   std::future<void> terminal = state->terminal.get_future();
   std::shared_ptr<boost::asio::cancellation_signal> cancellation =
       std::make_shared<boost::asio::cancellation_signal>();

   explicit native_ping_pair(ping_reply reply) {
      server.register_protocol_handler(p2p::builtins::ping, [state = state, reply](auto incoming) {
         return controlled_ping_peer(std::move(incoming), state, reply);
      });
      forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
      static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(server.local_endpoints().front(),
          p2p::node::connect_options{.expected_peer = server.local_peer(), .allow_relay = false,
                                    .timeout = 3s, .allow_hole_punch = false})));
   }

   ~native_ping_pair() {
      try { forge::asio::blocking::run(runtime, client.async_stop()); } catch (...) {}
      try { forge::asio::blocking::run(runtime, server.async_stop()); } catch (...) {}
   }

   auto start(std::chrono::milliseconds timeout) {
      return boost::asio::co_spawn(runtime.context(), client.async_ping(server.local_peer(),
          p2p::node::open_options{.allow_relay = false, .timeout = timeout, .allow_hole_punch = false}),
          boost::asio::use_future);
   }
};

void require_ping_failure(std::future<std::chrono::milliseconds>& result, p2p::exceptions::code expected) {
   BOOST_REQUIRE(result.wait_for(2s) == std::future_status::ready);
   try { static_cast<void>(result.get()); BOOST_FAIL("controlled Ping response should fail"); }
   catch (const forge::exceptions::base& error) {
      BOOST_TEST_MESSAGE("Ping exception: what=" << error.what() << "; category="
                         << error.code().category().name() << "; value=" << error.code().value());
      BOOST_TEST_CONTEXT("Ping exception: what=" << error.what() << "; category="
                         << error.code().category().name() << "; value=" << error.code().value()) {
         const auto code = p2p::exceptions::code_of(error);
         BOOST_REQUIRE(code);
         BOOST_CHECK(*code == expected);
      }
   }
}

void require_ping_session_reusable(native_ping_pair& pair, std::uint64_t session_id) {
   BOOST_REQUIRE(pair.server.unregister_protocol_handler(p2p::builtins::ping));
   const auto snapshot = pair.client.diagnostics();
   BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
   BOOST_TEST(snapshot.sessions.front().id == session_id);
   BOOST_TEST(!snapshot.sessions.front().closed);
   BOOST_CHECK(snapshot.sessions.front().authentication != p2p::peer_authentication::unverified);
   auto fresh = pair.start(2s);
   BOOST_REQUIRE(fresh.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(static_cast<void>(fresh.get()));
}

void require_authenticated_circuit_facts(const p2p::diagnostics::snapshot& snapshot, const p2p::peer_id& peer,
                                          const p2p::peer_id& relay) {
   const auto* circuit = static_cast<const p2p::diagnostics::session*>(nullptr);
   for (const auto& session : snapshot.sessions) {
      if (session.remote_peer == peer && session.path == p2p::path::kind::relay && !session.closed) {
         BOOST_REQUIRE(!circuit);
         circuit = &session;
      }
   }
   BOOST_REQUIRE(circuit);
   BOOST_CHECK(circuit->authentication != p2p::peer_authentication::unverified);
   BOOST_REQUIRE(circuit->circuit_endpoint);
   BOOST_REQUIRE(circuit->circuit_endpoint->relayed);
   BOOST_REQUIRE(circuit->circuit_endpoint->peer);
   BOOST_CHECK(*circuit->circuit_endpoint->peer == relay);
   BOOST_CHECK(circuit->circuit_endpoint->relayed->target == peer);
   BOOST_REQUIRE(circuit->carrier_session_id);
   // An inner stream has a logical route, not independent native socket names.
   BOOST_TEST(!circuit->local_endpoint);
   BOOST_TEST(!circuit->remote_endpoint);
   const auto* carrier = static_cast<const p2p::diagnostics::session*>(nullptr);
   for (const auto& session : snapshot.sessions) {
      if (session.id == *circuit->carrier_session_id) { carrier = &session; }
   }
   BOOST_REQUIRE(carrier);
   BOOST_CHECK(carrier->remote_peer == relay);
   BOOST_CHECK(carrier->path == p2p::path::kind::direct);
   BOOST_REQUIRE(carrier->local_endpoint);
   BOOST_REQUIRE(carrier->remote_endpoint);
   BOOST_TEST(carrier->local_endpoint->transport.port != 0U);
   BOOST_TEST(carrier->remote_endpoint->transport.port != 0U);
   BOOST_TEST(circuit->circuit_endpoint->transport.authority() == carrier->remote_endpoint->transport.authority());
}

using collision_manager = p2p::detail::path_manager;
const auto collision_protocol = p2p::protocol_id{.value = "/forge/cross-circuit-exchange/1.0.0"};
const auto collision_echo_protocol = p2p::protocol_id{.value = "/forge/cross-circuit-echo/1.0.0"};

p2p::node::options collision_options(std::string_view name, bool service = false) {
   auto value = ping_session_options(name);
   value.capabilities.bits = p2p::capabilities::relay_reservation;
   value.relay_policy.service_enabled = service;
   value.relay_policy.auto_discovery_enabled = false;
   value.path_policy.allow_hole_punch = false; // The fixture scripts two raw exchanges, not a NAT result.
   return value;
}

boost::asio::awaitable<void>
retain_collision_stream(p2p::node::incoming_protocol_stream incoming,
                        std::shared_ptr<std::promise<p2p::node::incoming_protocol_stream>> receipt) {
   receipt->set_value(std::move(incoming));
   co_return;
}

boost::asio::awaitable<void> echo_collision_stream(p2p::node::incoming_protocol_stream incoming,
                                                   std::shared_ptr<std::atomic_uint64_t> expected) {
   if (incoming.session.id != expected->load() || incoming.stream.authentication() != p2p::peer_authentication::noise) {
      throw std::runtime_error{"fresh raw stream did not retain its authenticated circuit owner"};
   }
   const auto bytes = co_await incoming.stream.async_read_frame();
   co_await incoming.stream.async_write_frame(bytes);
   co_await incoming.stream.async_close();
}

boost::asio::awaitable<p2p::upgraded_session> open_collision_circuit(forge::asio::runtime& runtime, p2p::node& source,
                                                                     p2p::peer_id relay, p2p::peer_id target,
                                                                     const p2p::node::options& options,
                                                                     const p2p::libp2p_identity_material& identity) {
   auto deadline = p2p::operation_deadline{runtime.context(), 5s};
   deadline.arm([&source] { source.request_stop(); });
   auto stream = co_await source.async_open_protocol_stream(
       relay, p2p::builtins::relay_hop,
       p2p::node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
   co_await stream.async_write(p2p::relay::codec::encode_hop(
       {.kind = p2p::relay::hop_message::message_kind::connect, .target = p2p::relay::peer{.id = target}}));
   auto buffered = std::vector<std::uint8_t>{};
   const auto response =
       p2p::relay::codec::decode_hop(co_await p2p::async_read_length_delimited(stream, buffered, 4096));
   if (response.kind != p2p::relay::hop_message::message_kind::status || response.status != p2p::relay::status::ok) {
      throw std::runtime_error{"real Relay HOP did not admit the cross-circuit fixture"};
   }
   auto upgraded = co_await p2p::upgrade_relay_outbound_session(
       p2p::detail::stream_access::with_buffer(std::move(stream), std::move(buffered)), options, identity, target, {});
   if (!deadline.finish()) {
      co_await upgraded.session->async_close();
      throw std::runtime_error{"cross-circuit HOP/Noise admission expired"};
   }
   co_return upgraded;
}

struct collision_wire {
   std::shared_ptr<p2p::detail::resource_stream> resource;
   p2p::stream stream;
   std::vector<std::uint8_t> buffered;
};

boost::asio::awaitable<p2p::hole_punch::message> read_collision_message(forge::asio::runtime& runtime,
                                                                        collision_wire& wire) {
   auto deadline = p2p::operation_deadline{runtime.context(), 3s};
   deadline.arm([resource = wire.resource] { resource->request_cancel(); });
   const auto bytes = co_await p2p::async_read_length_delimited(wire.stream, wire.buffered, 4096);
   if (!deadline.finish()) {
      throw std::runtime_error{"cross-circuit native message read expired"};
   }
   co_return p2p::hole_punch::codec::decode(bytes);
}

boost::asio::awaitable<p2p::stream> open_collision_stream(forge::asio::runtime& runtime,
                                                          std::shared_ptr<forge::net::yamux::session> session) {
   auto deadline = p2p::operation_deadline{runtime.context(), 3s};
   deadline.arm([session] { session->request_cancel(); });
   auto stream =
       co_await p2p::protocol_negotiation::async_select(co_await session->async_open_stream(), collision_protocol);
   if (!deadline.finish()) {
      co_await stream.async_close();
      throw std::runtime_error{"cross-circuit native stream admission expired"};
   }
   co_return stream;
}

struct collision_close_gate {
   forge::asio::notification changed;
   std::atomic_bool released = false;
   std::atomic_bool native_joined = false;
   std::promise<void> entered;
   std::future<void> entered_receipt = entered.get_future();

   void release() {
      released = true;
      changed.notify();
   }
};

boost::asio::awaitable<void> close_native_collision_exchange(std::shared_ptr<collision_manager> manager,
                                                             std::shared_ptr<collision_manager::operation> owner,
                                                             std::shared_ptr<collision_manager::exchange> ticket,
                                                             std::shared_ptr<p2p::detail::resource_stream> resource,
                                                             std::shared_ptr<collision_close_gate> gate) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto terminal = boost::scope::scope_exit{[manager, owner, ticket] { manager->end_exchange(owner, ticket); }};
   gate->entered.set_value();
   while (!gate->released.load()) {
      const auto epoch = gate->changed.epoch();
      if (!gate->released.load()) {
         co_await gate->changed.async_wait(epoch);
      }
   }
   try {
      co_await collision_manager::async_close_exchange(resource);
   } catch (const forge::net::yamux::exceptions::stream_reset&) {
      // The canceled child has actually crossed resource_stream's native close barrier.
   }
   gate->native_joined = true;
}

boost::asio::awaitable<bool> accept_collision_exchange(std::shared_ptr<collision_manager> manager,
                                                       std::shared_ptr<collision_manager::operation> owner,
                                                       std::shared_ptr<collision_manager::exchange> incoming,
                                                       p2p::peer_id local) {
   co_return co_await manager->async_accept_exchange(owner, incoming, local);
}

struct collision_side {
   p2p::node::options options;
   p2p::libp2p_identity_material identity;
   std::unique_ptr<p2p::node> node;
   p2p::upgraded_session outgoing;
   p2p::node::incoming_protocol_stream received;
   collision_wire original_wire, incoming_wire;
   std::shared_ptr<std::promise<p2p::node::incoming_protocol_stream>> receipt =
       std::make_shared<std::promise<p2p::node::incoming_protocol_stream>>();
   std::future<p2p::node::incoming_protocol_stream> incoming_receipt = receipt->get_future();
   std::shared_ptr<std::atomic_uint64_t> echo_owner = std::make_shared<std::atomic_uint64_t>(0);
   std::shared_ptr<collision_manager> manager =
       std::make_shared<collision_manager>(std::make_shared<p2p::detail::lifecycle_wakeup>());
   std::shared_ptr<collision_manager::operation> owner;
   std::shared_ptr<collision_manager::exchange> original, incoming;
   std::shared_ptr<std::promise<void>> child_stopped = std::make_shared<std::promise<void>>();
   std::future<void> child_stop_receipt = child_stopped->get_future();
   p2p::cancellation_latch::subscription child_cancel;
   std::shared_ptr<collision_close_gate> old_gate = std::make_shared<collision_close_gate>();
   std::shared_ptr<collision_close_gate> incoming_gate = std::make_shared<collision_close_gate>();
   std::future<bool> admission, cancellation;
   std::future<p2p::hole_punch::status> waiter;
   std::future<void> old_close, incoming_close, joined;
};

struct raw_collision_pair {
   // Every session, strand, waiter and notification below dies before its context.
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   p2p::node relay{runtime, collision_options("collision-relay", true)};
   std::array<collision_side, 2> sides;
   p2p::resource_manager resources;
   std::exception_ptr cleanup_failure;
   bool closed = false;

   raw_collision_pair() {
      for (auto index = 0U; index != sides.size(); ++index) {
         auto& side = sides[index];
         side.options = collision_options(index == 0 ? "collision-first" : "collision-second");
         side.identity = p2p::make_libp2p_identity_material(side.options);
         side.node = std::make_unique<p2p::node>(runtime, side.options);
         side.node->register_protocol_handler(collision_protocol, [receipt = side.receipt](auto incoming) {
            return retain_collision_stream(std::move(incoming), receipt);
         });
         side.node->register_protocol_handler(collision_echo_protocol, [expected = side.echo_owner](auto incoming) {
            return echo_collision_stream(std::move(incoming), expected);
         });
      }
   }

   ~raw_collision_pair() {
      close();
   }

   void attach(collision_wire& wire, p2p::stream stream, const p2p::peer_id& peer,
               p2p::resource_manager::session_direction direction) {
      auto reserved = resources.reserve_stream(peer, direction);
      if (!reserved) {
         throw std::runtime_error{"cross-circuit stream reservation refused"};
      }
      wire.resource = std::make_shared<p2p::detail::resource_stream>(std::move(*reserved));
      wire.resource->attach(std::move(stream).into_transport_stream());
      wire.stream = p2p::stream{forge::net::transport::detail::stream_access::make(wire.resource)};
   }

   void prepare() {
      forge::asio::blocking::run(runtime, relay.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
      for (auto& side : sides) {
         forge::asio::blocking::run(runtime, side.node->async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
         side.node->peers().learn_endpoint(
             relay.local_peer(), relay.local_endpoints().front(),
             p2p::capability_set{.bits = p2p::capabilities::relay | p2p::capabilities::relay_reservation});
         static_cast<void>(forge::asio::blocking::run(runtime, side.node->async_reserve_relay(relay.local_peer())));
      }
      for (auto index = 0U; index != sides.size(); ++index) {
         auto& side = sides[index];
         side.outgoing = forge::asio::blocking::run(
             runtime, open_collision_circuit(runtime, *side.node, relay.local_peer(),
                                             sides[1 - index].node->local_peer(), side.options, side.identity));
         auto stream = forge::asio::blocking::run(runtime, open_collision_stream(runtime, side.outgoing.session));
         attach(side.incoming_wire, std::move(stream), side.outgoing.peer,
                p2p::resource_manager::session_direction::outbound);
      }
      for (auto index = 0U; index != sides.size(); ++index) {
         auto& side = sides[index];
         if (side.incoming_receipt.wait_for(3s) != std::future_status::ready) {
            throw std::runtime_error{"native STOP circuit did not deliver the collision stream"};
         }
         side.received = side.incoming_receipt.get();
         side.echo_owner->store(side.received.session.id);
         if (side.received.stream.authentication() != p2p::peer_authentication::noise) {
            throw std::runtime_error{"native STOP circuit lacks Noise authentication"};
         }
         attach(side.original_wire, std::move(side.received.stream), sides[1 - index].node->local_peer(),
                p2p::resource_manager::session_direction::inbound);
      }
   }

   template <typename F> void cleanup(F&& action) noexcept {
      try {
         action();
      } catch (...) {
         if (!cleanup_failure) {
            cleanup_failure = std::current_exception();
         }
      }
   }

   template <typename T> void join(std::future<T>& task) noexcept {
      if (task.valid()) {
         cleanup([&] { static_cast<void>(task.get()); });
      }
   }

   void close() noexcept {
      if (closed) {
         return;
      }
      closed = true;
      for (auto& side : sides) {
         side.old_gate->release();
         side.incoming_gate->release();
         side.manager->request_stop();
         for (auto* wire : {&side.original_wire, &side.incoming_wire}) {
            if (wire->resource) {
               wire->resource->request_cancel();
            }
         }
      }
      for (auto& side : sides) {
         join(side.old_close);
         join(side.incoming_close);
         join(side.admission);
         for (auto* wire : {&side.original_wire, &side.incoming_wire}) {
            if (wire->resource) {
               cleanup([&] {
                  forge::asio::blocking::run(runtime,
                                             collision_manager::async_close_completed_exchange(wire->resource));
               });
            }
            wire->stream = {};
            wire->resource.reset();
         }
         side.child_cancel.reset();
         if (side.owner) {
            side.manager->end_exchange(side.owner, side.original);
            side.manager->end_exchange(side.owner, side.incoming);
            side.manager->finish(side.owner, p2p::hole_punch::status::failed);
         }
         join(side.cancellation);
         join(side.waiter);
         join(side.joined);
      }
      for (auto& side : sides) {
         if (side.outgoing.session) {
            cleanup([&] { forge::asio::blocking::run(runtime, side.outgoing.session->async_close()); });
            side.outgoing.session.reset();
         }
         if (side.node) {
            cleanup([&] { forge::asio::blocking::run(runtime, side.node->async_stop()); });
         }
      }
      cleanup([&] { forge::asio::blocking::run(runtime, relay.async_stop()); });
   }
};

boost::asio::awaitable<bool> probe_raw_circuit(forge::asio::runtime& runtime,
                                               std::shared_ptr<forge::net::yamux::session> session) {
   auto deadline = p2p::operation_deadline{runtime.context(), 3s};
   deadline.arm([session] { session->request_cancel(); });
   auto stream =
       co_await p2p::protocol_negotiation::async_select(co_await session->async_open_stream(), collision_echo_protocol);
   const auto payload = std::vector<std::uint8_t>{'c', 'i', 'r', 'c', 'u', 'i', 't'};
   co_await stream.async_write_frame(payload);
   const auto reply = co_await stream.async_read_frame();
   co_await stream.async_close();
   co_return deadline.finish() && reply == payload;
}

void require_raw_circuit_reusable(raw_collision_pair& pair, std::size_t index) {
   auto& side = pair.sides[index];
   BOOST_CHECK(forge::asio::blocking::run(pair.runtime, probe_raw_circuit(pair.runtime, side.outgoing.session)));
   require_authenticated_circuit_facts(pair.sides[1 - index].node->diagnostics(), side.node->local_peer(),
                                       pair.relay.local_peer());
}

void require_collision_endpoints(const std::vector<p2p::endpoint>& received,
                                 const std::vector<p2p::endpoint>& offered) {
   BOOST_REQUIRE_EQUAL(received.size(), offered.size());
   for (auto index = std::size_t{}; index != offered.size(); ++index) {
      BOOST_TEST_CONTEXT("CONNECT endpoint " << index) {
         BOOST_TEST(received[index].to_string() == offered[index].to_string());
      }
   }
}

void check_raw_cross_circuit_collision(bool cancel_handover) {
   auto pair = raw_collision_pair{};
   pair.prepare();
   auto connect_offers = std::array<std::vector<p2p::endpoint>, 2>{};
   const auto deadline = std::chrono::steady_clock::now() + 10s;
   const auto winner_index = p2p::peer_id::from_bytes(pair.sides[0].node->local_peer().to_bytes()) <
                                     p2p::peer_id::from_bytes(pair.sides[1].node->local_peer().to_bytes())
                                 ? 0U
                                 : 1U;
   auto& winner = pair.sides[winner_index];
   auto& loser = pair.sides[1 - winner_index];
   for (auto index = 0U; index != pair.sides.size(); ++index) {
      auto& side = pair.sides[index];
      const auto& received = side.received.session;
      BOOST_REQUIRE(received.id != 0U);
      BOOST_CHECK(received.remote_peer == side.outgoing.peer);
      BOOST_CHECK(received.path == p2p::path::kind::relay);
      BOOST_REQUIRE(received.relay_peer);
      BOOST_CHECK(*received.relay_peer == pair.relay.local_peer());
      BOOST_CHECK(received.security_role == forge::net::tls::endpoint_role::server);
      BOOST_CHECK(received.yamux_role == forge::net::yamux::side::responder);
      BOOST_CHECK(side.outgoing.authentication == p2p::peer_authentication::noise);
      BOOST_CHECK(side.outgoing.role == p2p::upgrade_role::initiator);
      BOOST_CHECK(side.outgoing.security_role == forge::net::tls::endpoint_role::client);
      BOOST_CHECK(side.outgoing.yamux_role == forge::net::yamux::side::initiator);
      BOOST_CHECK(side.outgoing.peer == pair.sides[1 - index].node->local_peer());
      side.owner =
          side.manager->begin(side.outgoing.peer, received.id, collision_manager::role::initiator, deadline).owner;
      BOOST_REQUIRE(side.owner);
      side.original = side.manager->start_exchange(side.owner);
      BOOST_REQUIRE(side.original);
      // The raw outgoing session is deliberately unpublished. Its object key is
      // a private manager fixture key, NOT a node/native connection-ID receipt.
      const auto raw_key = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(side.outgoing.session.get()));
      BOOST_REQUIRE(raw_key != received.id);
      side.incoming = std::make_shared<collision_manager::exchange>(collision_manager::exchange{
          raw_key, collision_manager::role::responder, std::make_shared<p2p::cancellation_latch>()});
      side.child_cancel = p2p::cancellation_latch::subscribe(
          side.original->cancellation, [resource = side.original_wire.resource, stopped = side.child_stopped] {
             resource->request_cancel();
             stopped->set_value();
          });
      // Each CONNECT really traverses its opposite HOP/STOP + Noise/Yamux circuit.
      // local_endpoints includes advertised reservation routes, not only the
      // listener. Compare exactly the snapshot actually encoded on this stream.
      connect_offers[index] = side.node->local_endpoints();
      BOOST_REQUIRE(!connect_offers[index].empty());
      forge::asio::blocking::run(pair.runtime, side.original_wire.stream.async_write(p2p::hole_punch::codec::encode(
                                                   {.observed_endpoints = connect_offers[index]})));
   }
   for (auto index = 0U; index != pair.sides.size(); ++index) {
      const auto message = forge::asio::blocking::run(
          pair.runtime, read_collision_message(pair.runtime, pair.sides[index].incoming_wire));
      BOOST_CHECK(message.kind == p2p::hole_punch::message::message_kind::connect);
      require_collision_endpoints(message.observed_endpoints, connect_offers[1 - index]);
   }
   // Both native CONNECT reads complete before arbitration can reset either
   // child: a scheduling accident must not turn this into a one-sided attempt.
   for (auto& side : pair.sides) {
      side.admission = boost::asio::co_spawn(
          pair.runtime.context(),
          accept_collision_exchange(side.manager, side.owner, side.incoming, side.node->local_peer()),
          boost::asio::use_future);
   }
   BOOST_REQUIRE(winner.admission.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK(!winner.admission.get());
   BOOST_REQUIRE(loser.child_stop_receipt.wait_for(2s) == std::future_status::ready);
   loser.old_close =
       boost::asio::co_spawn(pair.runtime.context(),
                             close_native_collision_exchange(loser.manager, loser.owner, loser.original,
                                                             loser.original_wire.resource, loser.old_gate),
                             boost::asio::use_future);
   BOOST_REQUIRE(loser.old_gate->entered_receipt.wait_for(2s) == std::future_status::ready);
   BOOST_REQUIRE(loser.original->cancellation->stop_requested());
   BOOST_CHECK(!winner.original->cancellation->stop_requested());
   BOOST_CHECK(!loser.owner->cancellation->stop_requested());
   BOOST_CHECK(!loser.incoming->cancellation->stop_requested());
   BOOST_CHECK(!loser.old_gate->native_joined.load());
   BOOST_CHECK(loser.admission.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(pair.resources.current().system.inbound_streams == 2U);
   loser.waiter = boost::asio::co_spawn(pair.runtime.context(), loser.manager->async_wait(loser.owner, deadline),
                                        boost::asio::use_future);
   const auto coalesced = loser.manager->begin(loser.outgoing.peer, loser.incoming->session_id,
                                               collision_manager::role::responder, deadline + 1h);
   BOOST_CHECK(coalesced.owner == loser.owner);
   BOOST_CHECK(!coalesced.leader);
   BOOST_CHECK(loser.owner->deadline == deadline);

   if (cancel_handover) {
      loser.cancellation = boost::asio::co_spawn(
          pair.runtime.context(), loser.manager->async_cancel(loser.outgoing.peer), boost::asio::use_future);
      BOOST_REQUIRE(loser.admission.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK(!loser.admission.get());
      BOOST_CHECK(loser.owner->cancellation->stop_requested());
      loser.incoming_close =
          boost::asio::co_spawn(pair.runtime.context(),
                                close_native_collision_exchange(loser.manager, loser.owner, loser.incoming,
                                                                loser.incoming_wire.resource, loser.incoming_gate),
                                boost::asio::use_future);
      BOOST_REQUIRE(loser.incoming_gate->entered_receipt.wait_for(2s) == std::future_status::ready);
      loser.manager->finish(loser.owner, p2p::hole_punch::status::failed);
      loser.joined =
          boost::asio::co_spawn(pair.runtime.context(), loser.manager->async_join(), boost::asio::use_future);
      BOOST_CHECK(loser.cancellation.wait_for(0ms) != std::future_status::ready);
      BOOST_CHECK(loser.joined.wait_for(0ms) != std::future_status::ready);
      loser.old_gate->release();
      BOOST_REQUIRE(loser.old_close.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(loser.old_close.get());
      BOOST_CHECK(loser.old_gate->native_joined.load());
      BOOST_TEST(loser.manager->active() == 1U);
      BOOST_CHECK(loser.cancellation.wait_for(0ms) != std::future_status::ready);
      BOOST_CHECK(loser.joined.wait_for(0ms) != std::future_status::ready);
      loser.incoming_gate->release();
      BOOST_REQUIRE(loser.incoming_close.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(loser.incoming_close.get());
      BOOST_CHECK(loser.incoming_gate->native_joined.load());
      BOOST_REQUIRE(loser.cancellation.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK(loser.cancellation.get());
      BOOST_REQUIRE(loser.joined.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(loser.joined.get());
      BOOST_TEST(loser.manager->active() == 0U);
      BOOST_TEST(loser.manager->inspect(loser.owner).attempts == 1U);
   } else {
      loser.old_gate->release();
      BOOST_REQUIRE(loser.old_close.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(loser.old_close.get());
      BOOST_REQUIRE(loser.admission.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK(loser.admission.get());
      BOOST_CHECK(loser.old_gate->native_joined.load());
      BOOST_TEST(loser.manager->inspect(loser.owner).attempts == 2U);
      BOOST_TEST(loser.owner->session_id == loser.received.session.id);
      BOOST_CHECK(loser.owner->side == collision_manager::role::initiator);
      BOOST_CHECK(loser.incoming->side == collision_manager::role::responder);
      loser.manager->end_exchange(loser.owner, loser.original);
      BOOST_CHECK(loser.manager->inspect(loser.owner).exchanging);
      BOOST_CHECK(!loser.manager->seal(loser.owner, true));
      BOOST_CHECK(loser.waiter.wait_for(0ms) != std::future_status::ready);
      const auto response_offer = loser.node->local_endpoints();
      BOOST_REQUIRE(!response_offer.empty());
      forge::asio::blocking::run(pair.runtime, loser.incoming_wire.stream.async_write(p2p::hole_punch::codec::encode(
                                                   {.observed_endpoints = response_offer})));
      const auto response =
          forge::asio::blocking::run(pair.runtime, read_collision_message(pair.runtime, winner.original_wire));
      BOOST_CHECK(response.kind == p2p::hole_punch::message::message_kind::connect);
      require_collision_endpoints(response.observed_endpoints, response_offer);
      forge::asio::blocking::run(pair.runtime, winner.original_wire.stream.async_write(p2p::hole_punch::codec::encode(
                                                   {.kind = p2p::hole_punch::message::message_kind::sync})));
      const auto sync =
          forge::asio::blocking::run(pair.runtime, read_collision_message(pair.runtime, loser.incoming_wire));
      BOOST_CHECK(sync.kind == p2p::hole_punch::message::message_kind::sync);
      BOOST_CHECK(sync.observed_endpoints.empty());
      forge::asio::blocking::run(pair.runtime,
                                 collision_manager::async_close_completed_exchange(loser.incoming_wire.resource));
      loser.manager->end_exchange(loser.owner, loser.incoming);
      // No direct dial was run: negotiation alone must not manufacture succeeded.
      loser.manager->finish(loser.owner, p2p::hole_punch::status::failed);
   }
   BOOST_REQUIRE(loser.waiter.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK(loser.waiter.get() == p2p::hole_punch::status::failed);
   BOOST_TEST(loser.manager->active() == 0U);
   BOOST_CHECK(loser.owner->deadline == deadline);
   BOOST_TEST(winner.manager->inspect(winner.owner).attempts == 1U);
   forge::asio::blocking::run(pair.runtime,
                              collision_manager::async_close_completed_exchange(winner.incoming_wire.resource));
   forge::asio::blocking::run(pair.runtime,
                              collision_manager::async_close_completed_exchange(winner.original_wire.resource));
   winner.manager->end_exchange(winner.owner, winner.original);
   winner.manager->finish(winner.owner, p2p::hole_punch::status::failed);
   BOOST_TEST(winner.manager->active() == 0U);
   BOOST_TEST(pair.resources.current().system.inbound_streams == 0U);
   BOOST_TEST(pair.resources.current().system.outbound_streams == 0U);
   require_raw_circuit_reusable(pair, 0);
   require_raw_circuit_reusable(pair, 1);
   pair.close();
   BOOST_CHECK(!pair.cleanup_failure);
   BOOST_TEST(winner.manager->retained() == 0U);
   BOOST_TEST(loser.manager->retained() == 0U);
}

} // namespace

BOOST_AUTO_TEST_CASE(path_raw_cross_circuits_keep_canonical_initiator_after_native_loser_join) {
   check_raw_cross_circuit_collision(false);
}

BOOST_AUTO_TEST_CASE(path_raw_cross_circuits_cancel_handover_joins_both_native_streams) {
   check_raw_cross_circuit_collision(true);
}

BOOST_AUTO_TEST_CASE(path_direct_negotiation_facts_survive_session_publication_and_diagnostics) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = p2p::node{runtime, path_session_options("path-receipt-server")};
   auto client = p2p::node{runtime, path_session_options("path-receipt-client")};
   forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto addresses = server.local_endpoints();
   BOOST_REQUIRE_EQUAL(addresses.size(), 1U);
   const auto session = forge::asio::blocking::run(runtime,
       client.async_connect(addresses.front(), p2p::node::connect_options{
           .expected_peer = server.local_peer(), .allow_relay = false, .allow_hole_punch = false}));
   BOOST_CHECK(session.remote_peer == server.local_peer());
   BOOST_CHECK(session.muxer == p2p::protocol_id{.value = "/yamux/1.0.0"});
   forge::asio::blocking::run(runtime, client.async_ping(server.local_peer()));
   const auto snapshot = client.diagnostics();
   BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
   BOOST_CHECK(snapshot.sessions.front().muxer == session.muxer);
   BOOST_TEST(snapshot.sessions.front().used_early_muxer_negotiation == session.used_early_muxer_negotiation);
   const auto inbound = server.diagnostics();
   BOOST_REQUIRE_EQUAL(inbound.sessions.size(), 1U);
   const auto& outgoing = snapshot.sessions.front();
   const auto& incoming = inbound.sessions.front();
   BOOST_REQUIRE(outgoing.local_endpoint);
   BOOST_REQUIRE(outgoing.remote_endpoint);
   BOOST_REQUIRE(incoming.local_endpoint);
   BOOST_REQUIRE(incoming.remote_endpoint);
   BOOST_TEST(outgoing.local_endpoint->transport.port != 0U);
   BOOST_TEST(outgoing.local_endpoint->transport.port == incoming.remote_endpoint->transport.port);
   BOOST_TEST(outgoing.local_endpoint->transport.host == incoming.remote_endpoint->transport.host);
   BOOST_TEST(incoming.local_endpoint->transport.port == outgoing.remote_endpoint->transport.port);
   BOOST_TEST(incoming.local_endpoint->transport.host == outgoing.remote_endpoint->transport.host);
   BOOST_TEST(incoming.local_endpoint->transport.port == addresses.front().transport.port);
   BOOST_CHECK(outgoing.authentication == p2p::peer_authentication::libp2p_tls);
   BOOST_CHECK(incoming.authentication == outgoing.authentication);
   BOOST_CHECK(outgoing.direction == p2p::diagnostics::session_direction::outbound);
   BOOST_CHECK(incoming.direction == p2p::diagnostics::session_direction::inbound);
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(path_dcutr_on_authenticated_direct_connection_is_rejected_without_retiring_it) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto server = p2p::node{runtime, path_session_options("path-guard-server")};
   auto client = p2p::node{runtime, path_session_options("path-guard-client")};
   forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto addresses = server.local_endpoints();
   BOOST_REQUIRE_EQUAL(addresses.size(), 1U);
   static_cast<void>(forge::asio::blocking::run(runtime,
       client.async_connect(addresses.front(), p2p::node::connect_options{
           .expected_peer = server.local_peer(), .allow_relay = false, .allow_hole_punch = false})));
   const auto deadline_fired = std::make_shared<std::atomic_bool>(false);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime,
       rejected_direct_exchange(runtime, client, server.local_peer(), deadline_fired)), std::exception);
   BOOST_TEST(!deadline_fired->load());
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(runtime, client.async_ping(server.local_peer())));
   BOOST_TEST(server.diagnostics().metrics.hole_punch_attempts == 0U);
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(path_explicit_owner_cancel_keeps_authenticated_relay_and_existing_stream_usable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto relay_options = path_session_options("path-cancel-relay");
   relay_options.allow_insecure_test_mode = false;
   relay_options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   relay_options.capabilities.bits = p2p::capabilities::relay | p2p::capabilities::relay_reservation;
   relay_options.relay_policy.service_enabled = true;
   auto target_options = path_session_options("path-cancel-target");
   target_options.allow_insecure_test_mode = false;
   target_options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   target_options.capabilities.bits = p2p::capabilities::relay_reservation;
   target_options.path_policy.allow_hole_punch = false;
   auto source_options = path_session_options("path-cancel-source");
   source_options.allow_insecure_test_mode = false;
   source_options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   // This fixture owns RESERVE explicitly; autonomous selection has separate coverage.
   for (auto* options : {&relay_options, &target_options, &source_options}) {
      options->relay_policy.auto_discovery_enabled = false;
   }
   auto relay = p2p::node{runtime, std::move(relay_options)};
   auto target = p2p::node{runtime, std::move(target_options)};
   auto source = p2p::node{runtime, std::move(source_options)};
   forge::asio::blocking::run(runtime, relay.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   forge::asio::blocking::run(runtime, target.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   forge::asio::blocking::run(runtime, source.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto relay_addresses = relay.local_endpoints();
   BOOST_REQUIRE_EQUAL(relay_addresses.size(), 1U);
   const auto relay_capabilities = p2p::capability_set{
       .bits = p2p::capabilities::relay | p2p::capabilities::relay_reservation};
   source.peers().learn_endpoint(relay.local_peer(), relay_addresses.front(), relay_capabilities);
   target.peers().learn_endpoint(relay.local_peer(), relay_addresses.front(), relay_capabilities);
   // Both carriers are physically inbound at the peers. Their direct_endpoint
   // is the local listener, while the circuit route must use the remote relay socket.
   for (auto* peer : {&target, &source}) {
      static_cast<void>(forge::asio::blocking::run(runtime, relay.async_connect(peer->local_endpoints().front(),
          p2p::node::connect_options{.expected_peer = peer->local_peer(), .allow_relay = false,
                                    .timeout = 3s, .allow_hole_punch = false})));
      const auto snapshot = peer->diagnostics();
      BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
      const auto& carrier = snapshot.sessions.front();
      BOOST_CHECK(carrier.remote_peer == relay.local_peer());
      BOOST_CHECK(carrier.direction == p2p::diagnostics::session_direction::inbound);
      BOOST_REQUIRE(carrier.direct_endpoint);
      BOOST_REQUIRE(carrier.local_endpoint);
      BOOST_REQUIRE(carrier.remote_endpoint);
      BOOST_TEST(carrier.direct_endpoint->transport.authority() == carrier.local_endpoint->transport.authority());
      BOOST_TEST(carrier.remote_endpoint->transport.authority() != carrier.local_endpoint->transport.authority());
   }
   static_cast<void>(forge::asio::blocking::run(runtime, target.async_reserve_relay(relay.local_peer())));
   const auto protocol = p2p::protocol_id{.value = "/forge/path-cancel-echo/1.0.0"};
   const auto callback_owner = std::make_shared<std::atomic<std::uint64_t>>(0);
   target.register_protocol_handler(protocol,
       [callback_owner](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          if (incoming.session.id == 0) { throw std::runtime_error{"callback lacks admitted native owner"}; }
          auto previous = std::uint64_t{};
          if (!callback_owner->compare_exchange_strong(previous, incoming.session.id) && previous != incoming.session.id) {
             throw std::runtime_error{"retained relay was replaced before the fresh stream"};
          }
          for (;;) {
             auto payload = co_await incoming.stream.async_read_frame();
             co_await incoming.stream.async_write_frame(payload);
          }
       });
   const auto open = p2p::node::open_options{
       .allow_relay = true, .relay_peer = relay.local_peer(), .timeout = 3s, .allow_hole_punch = false};
   auto stream = forge::asio::blocking::run(runtime,
       source.async_open_protocol_stream(target.local_peer(), protocol, open));
   const auto payload = std::vector<std::uint8_t>{'r', 'e', 'l', 'a', 'y'};
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, stream.async_read_frame()) == payload,
              boost::test_tools::per_element());
   const auto before = source.diagnostics();
   require_authenticated_circuit_facts(before, target.local_peer(), relay.local_peer());
   require_authenticated_circuit_facts(target.diagnostics(), source.local_peer(), relay.local_peer());
   const auto target_before = target.diagnostics();
   auto callback_bound = false;
   for (const auto& session : target_before.sessions) {
      if (session.id == callback_owner->load()) {
         callback_bound = session.remote_peer == source.local_peer() && session.path == p2p::path::kind::relay;
      }
   }
   BOOST_TEST(callback_bound);
   auto relay_id = std::uint64_t{};
   for (const auto& session : before.sessions) {
      if (session.remote_peer == target.local_peer()) {
         BOOST_CHECK(session.path == p2p::path::kind::relay);
         BOOST_CHECK(session.authentication != p2p::peer_authentication::unverified);
         relay_id = session.id;
      }
   }
   BOOST_REQUIRE(relay_id != 0U);
   BOOST_TEST(!forge::asio::blocking::run(runtime, source.async_cancel_hole_punch(target.local_peer())));
   auto attempt = boost::asio::co_spawn(runtime.context(),
       source.async_attempt_hole_punch(target.local_peer(), relay.local_peer(), 10s), boost::asio::use_future);
   BOOST_TEST(forge::asio::blocking::run(runtime, cancel_admitted_path(source, target.local_peer())));
   BOOST_REQUIRE(attempt.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK(attempt.get() == p2p::hole_punch::status::failed);
   BOOST_TEST(!forge::asio::blocking::run(runtime, source.async_cancel_hole_punch(target.local_peer())));
   // No host stop or relay cancellation occurs before this same-stream proof.
   forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, stream.async_read_frame()) == payload,
              boost::test_tools::per_element());
   auto fresh = forge::asio::blocking::run(runtime,
       source.async_open_protocol_stream(target.local_peer(), protocol, open));
   forge::asio::blocking::run(runtime, fresh.async_write_frame(payload));
   BOOST_TEST(forge::asio::blocking::run(runtime, fresh.async_read_frame()) == payload,
              boost::test_tools::per_element());
   const auto after = source.diagnostics();
   require_authenticated_circuit_facts(after, target.local_peer(), relay.local_peer());
   require_authenticated_circuit_facts(target.diagnostics(), source.local_peer(), relay.local_peer());
   auto retained = false;
   for (const auto& session : after.sessions) {
      if (session.id == relay_id) {
         retained = !session.closed;
         BOOST_CHECK(session.path == p2p::path::kind::relay);
         BOOST_CHECK(session.authentication != p2p::peer_authentication::unverified);
      }
      if (session.remote_peer == target.local_peer()) { BOOST_CHECK(session.path == p2p::path::kind::relay); }
   }
   BOOST_TEST(retained);
   forge::asio::blocking::run(runtime, fresh.async_close());
   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, target.async_stop());
   forge::asio::blocking::run(runtime, relay.async_stop());
}

BOOST_AUTO_TEST_CASE(path_ping_collects_split_native_reply_before_comparing_all_32_bytes) {
   auto pair = native_ping_pair{ping_reply::split};
   auto result = pair.start(2s);
   BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
   BOOST_REQUIRE(result.wait_for(2s) == std::future_status::ready);
   const auto rtt = result.get();
   BOOST_TEST(rtt.count() >= 70);
   BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   BOOST_TEST(pair.state->writes.load() == 3U);
}

BOOST_AUTO_TEST_CASE(path_ping_overall_deadline_resets_stalled_native_read_without_retiring_session) {
   auto pair = native_ping_pair{ping_reply::stalled};
   const auto before = pair.client.diagnostics();
   BOOST_REQUIRE_EQUAL(before.sessions.size(), 1U);
   auto result = pair.start(250ms);
   BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
   require_ping_failure(result, p2p::exceptions::code::timeout);
   BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   BOOST_TEST(pair.state->writes.load() == 0U);
   require_ping_session_reusable(pair, before.sessions.front().id);
}

BOOST_AUTO_TEST_CASE(path_ping_mismatched_native_reply_is_protocol_error_without_retiring_session) {
   auto pair = native_ping_pair{ping_reply::mismatch};
   const auto before = pair.client.diagnostics();
   BOOST_REQUIRE_EQUAL(before.sessions.size(), 1U);
   auto result = pair.start(2s);
   BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
   require_ping_failure(result, p2p::exceptions::code::protocol_error);
   BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   require_ping_session_reusable(pair, before.sessions.front().id);
}

BOOST_AUTO_TEST_CASE(path_ping_rejects_truncated_and_oversized_native_replies_before_deadline) {
   for (const auto mode : {ping_reply::truncated, ping_reply::oversized}) {
      auto pair = native_ping_pair{mode};
      auto result = pair.start(2s);
      BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
      if (mode == ping_reply::truncated) {
         BOOST_REQUIRE(result.wait_for(1s) == std::future_status::ready);
         try { static_cast<void>(result.get()); BOOST_FAIL("truncated native Ping reply should fail at EOF"); }
         catch (const forge::exceptions::base& error) {
            BOOST_TEST_MESSAGE("Truncated Ping exception: what=" << error.what() << "; category="
                               << error.code().category().name() << "; value=" << error.code().value());
            BOOST_TEST_CONTEXT("Truncated Ping exception: what=" << error.what() << "; category="
                               << error.code().category().name() << "; value=" << error.code().value()) {
               // Native Yamux EOF is a typed foreign error, not an empty read.
               BOOST_CHECK(forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed));
            }
         }
      } else {
         require_ping_failure(result, p2p::exceptions::code::protocol_error);
      }
      BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   }
}

BOOST_AUTO_TEST_CASE(path_ping_shutdown_wakes_stalled_native_read_without_waiting_for_ping_deadline) {
   auto pair = native_ping_pair{ping_reply::stalled};
   auto result = pair.start(10s);
   BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
   auto shutdown = boost::asio::co_spawn(pair.runtime.context(), pair.client.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(shutdown.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(shutdown.get());
   BOOST_REQUIRE(result.wait_for(1s) == std::future_status::ready);
   try { static_cast<void>(result.get()); BOOST_FAIL("Ping cannot succeed after shutdown of its waiting session"); }
   catch (const forge::exceptions::base& error) {
      BOOST_CHECK(!p2p::exceptions::is(error, p2p::exceptions::code::timeout));
   }
   BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   BOOST_TEST(pair.client.diagnostics().sessions.empty());
}

BOOST_AUTO_TEST_CASE(path_ping_caller_cancellation_joins_native_stream_without_retiring_session) {
   auto pair = native_ping_pair{ping_reply::stalled};
   const auto before = pair.client.diagnostics();
   BOOST_REQUIRE_EQUAL(before.sessions.size(), 1U);
   auto strand = boost::asio::make_strand(pair.runtime.context());
   auto cancellation = pair.cancellation;
   auto result = boost::asio::co_spawn(strand, pair.client.async_ping(pair.server.local_peer(),
       p2p::node::open_options{.allow_relay = false, .timeout = 10s, .allow_hole_punch = false}),
       boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));
   BOOST_REQUIRE(pair.received.wait_for(1s) == std::future_status::ready);
   boost::asio::post(strand, [cancellation] { cancellation->emit(boost::asio::cancellation_type::all); });
   const auto canceled_in_time = result.wait_for(2s) == std::future_status::ready;
   if (!canceled_in_time) {
      // Join before reporting failure so the coroutine cannot retain a slot
      // whose signal was destroyed during test unwinding.
      forge::asio::blocking::run(pair.runtime, pair.client.async_stop());
      result.wait();
   }
   BOOST_REQUIRE(canceled_in_time);
   try { static_cast<void>(result.get()); BOOST_FAIL("Canceled Ping must not succeed"); }
   catch (const boost::system::system_error& error) {
      BOOST_CHECK(error.code() == boost::asio::error::operation_aborted);
   } catch (const forge::exceptions::base& error) {
      BOOST_CHECK(p2p::exceptions::is(error, p2p::exceptions::code::canceled));
   }
   BOOST_REQUIRE(pair.terminal.wait_for(1s) == std::future_status::ready);
   require_ping_session_reusable(pair, before.sessions.front().id);
}
