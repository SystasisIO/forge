#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.multiformats.varint;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.rendezvous;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.quic.exceptions;
import forge.net.transport.exceptions;
import forge.net.yamux.exceptions;

namespace {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;
constexpr auto namespace_name = std::string_view{"forge.unregister.bound"};
enum class reply { fin, reset, unexpected_data, silent };
enum class peer_termination { unexpected, stream_reset, connection_cleanup, cancellation_cleanup };

bool request_fin(const forge::exceptions::base& error) {
   return dynamic_cast<const forge::net::yamux::exceptions::end_of_stream*>(&error) ||
       dynamic_cast<const forge::net::quic::exceptions::end_of_stream*>(&error) ||
       dynamic_cast<const forge::net::transport::exceptions::end_of_stream*>(&error);
}

peer_termination classify_peer_termination(const forge::exceptions::base& error, bool quic) {
   if (request_fin(error)) { return peer_termination::unexpected; }
   if (quic) {
      if (forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::stream_reset)) {
         return peer_termination::stream_reset;
      }
      if (forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::closed)) {
         return peer_termination::connection_cleanup;
      }
      if (forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::canceled)) {
         return peer_termination::cancellation_cleanup;
      }
   } else {
      if (forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::stream_reset)) {
         return peer_termination::stream_reset;
      }
      if (forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed)) {
         return peer_termination::connection_cleanup;
      }
      if (forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::canceled)) {
         return peer_termination::cancellation_cleanup;
      }
   }
   return peer_termination::unexpected;
}

template<class T> T joined(std::future<T>& result) {
   BOOST_REQUIRE(result.wait_for(5s) == std::future_status::ready);
   return result.get();
}

boost::asio::awaitable<void> require_request_fin(p2p::stream& stream) {
   try {
      static_cast<void>(co_await stream.async_read());
   } catch (const forge::exceptions::base& error) {
      if (request_fin(error)) { co_return; }
      throw;
   }
   throw std::runtime_error{"Rendezvous request must end with typed FIN, not extra data or an empty read"};
}

boost::asio::awaitable<void> cancel_and_join(p2p::stream& stream) {
   stream.request_cancel();
   try { co_await stream.async_close(); }
   catch (const forge::exceptions::base& error) {
      if (!forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::canceled) &&
          !forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::canceled) &&
          !forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::stream_reset) &&
          !forge::net::quic::exceptions::is(error, forge::net::quic::exceptions::code::stream_reset)) { throw; }
   }
}

void require_outbound_resources(const p2p::node& node, std::size_t streams) {
   const auto resources = node.diagnostics().resources;
   BOOST_TEST(resources.system.outbound_streams == streams);
   BOOST_TEST(resources.protocols.outbound_streams == streams);
   BOOST_TEST(resources.protocol_peers.outbound_streams == streams);
   BOOST_TEST(resources.active_protocol_scopes == streams);
   BOOST_TEST(resources.active_protocol_peer_scopes == streams);
}

boost::asio::awaitable<void> wait_for_probe_resource_quiescence(const p2p::node& node) {
   const auto executor = co_await boost::asio::this_coro::executor;
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   while (true) {
      const auto resources = node.diagnostics().resources;
      if (resources.system.outbound_streams != 1U || resources.protocols.outbound_streams != 1U ||
          resources.protocol_peers.outbound_streams != 1U) {
         throw std::runtime_error{"probe reservation changed before write FIN"};
      }
      // Active scopes include inbound Identify/Push and retained write memory.
      // Establish their drain before FIN, not by weakening post-FIN ownership checks.
      if (resources.active_protocol_scopes == 1U && resources.active_protocol_peer_scopes == 1U &&
          resources.system.inbound_streams == 0U && resources.transient.inbound_streams == 0U &&
          resources.transient.outbound_streams == 0U) { co_return; }
      if (std::chrono::steady_clock::now() >= deadline) {
         throw std::runtime_error{"background protocol resources did not drain before write FIN"};
      }
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }
}

boost::asio::awaitable<p2p::rendezvous::message> read_request(p2p::stream& stream) {
   constexpr auto maximum = std::size_t{64 * 1024};
   auto bytes = std::vector<std::uint8_t>{};
   auto prefix = std::size_t{};
   auto length = std::optional<std::size_t>{};
   do {
      const auto next = co_await stream.async_read();
      if (next.empty() || next.size() > maximum + 3 - bytes.size()) {
         throw std::runtime_error{"Rendezvous fixture received a truncated or oversized frame"};
      }
      bytes.insert(bytes.end(), next.begin(), next.end());
      while (!length && prefix < bytes.size()) {
         const auto byte = bytes[prefix++];
         if (prefix == 3 && (byte & 128U)) { throw std::runtime_error{"oversized Rendezvous fixture prefix"}; }
         if (!(byte & 128U)) {
            length = forge::multiformats::varint_decode(std::span<const std::uint8_t>{bytes}.first(prefix)).value;
            if (*length == 0 || *length > maximum) { throw std::runtime_error{"invalid Rendezvous fixture length"}; }
         }
      }
   } while (!length || bytes.size() < prefix + *length);
   if (bytes.size() != prefix + *length) { throw std::runtime_error{"concatenated Rendezvous fixture frames"}; }
   co_return p2p::rendezvous::codec::decode(bytes);
}

p2p::node::options options_for(std::string_view name) {
   const auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = p2p::node::options{
       .certificate_pem = identity.certificate_pem, .private_key_pem = identity.private_key_pem};
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   options.capabilities.bits = p2p::capabilities::direct_quic | p2p::capabilities::rendezvous;
   options.stream_security = p2p::node::stream_security::noise;
   options.dht_profiles.clear();
   options.relay_policy.client_enabled = false;
   options.relay_policy.auto_discovery_enabled = false;
   options.path_policy.allow_relay = false;
   options.path_policy.allow_hole_punch = false;
   options.reachability_policy.client_v1_enabled = false;
   options.reachability_policy.client_v2_enabled = false;
   options.reachability_policy.ping_enabled = false;
   options.limits.topology.operating_mode = p2p::topology::mode::static_only;
   options.limits.topology.dht_enabled = false;
   options.limits.topology.peer_exchange_enabled = false;
   return options;
}

struct donor_state {
   reply response;
   p2p::node* point = nullptr;
   std::atomic_size_t unregister_requests{0};
   std::atomic_bool unregister_request_fin{false};
   std::promise<void> received;
   std::promise<void> removed;
   std::promise<peer_termination> cleanup_terminated;
   forge::asio::notification allow_remove;
   forge::asio::notification allow_fin;
   forge::asio::notification allow_cleanup;
   std::mutex failures_mutex;
   std::vector<std::exception_ptr> failures;

   explicit donor_state(reply value) : response(value) {}

   boost::asio::awaitable<void> handle(p2p::node::incoming_protocol_stream incoming) {
      BOOST_CHECK(incoming.protocol == p2p::builtins::rendezvous);
      BOOST_CHECK(incoming.stream.authentication() == p2p::peer_authentication::noise ||
                  incoming.stream.authentication() == p2p::peer_authentication::quic_tls);
      const auto request = co_await read_request(incoming.stream);
      // Deliberately stricter than pinned Rust: require request EOF before any
      // response/removal to expose ordering errors, not to model donor parsing.
      co_await require_request_fin(incoming.stream);
      if (request.type == p2p::rendezvous::message_type::register_peer && request.register_value) {
         const auto& value = *request.register_value;
         BOOST_TEST(value.namespace_name == namespace_name);
         const auto record = p2p::rendezvous::codec::open_peer_record(
             p2p::signed_envelope::decode(value.signed_peer_record), incoming.session.remote_peer);
         co_await point->peers().async_register_rendezvous({
             .namespace_name = value.namespace_name, .peer = record.peer, .endpoints = record.endpoints,
             .signed_peer_record = value.signed_peer_record, .ttl = 7200s,
             .expires_at = std::chrono::system_clock::now() + 7200s}, 8);
         co_await incoming.stream.async_write(p2p::rendezvous::codec::encode({
             .type = p2p::rendezvous::message_type::register_response,
             .register_response_value = p2p::rendezvous::register_response{
                 .status_value = p2p::rendezvous::status::ok, .ttl = 7200s}}));
      } else if (request.type == p2p::rendezvous::message_type::discover && request.discover_value) {
         co_await incoming.stream.async_write(p2p::rendezvous::codec::encode({
             .type = p2p::rendezvous::message_type::discover_response,
             .discover_response_value = p2p::rendezvous::discover_response{
                 .cookie = p2p::rendezvous::codec::make_cookie(1, namespace_name),
                 .status_value = p2p::rendezvous::status::ok}}));
      } else if (request.type == p2p::rendezvous::message_type::unregister_peer && request.unregister_value) {
         BOOST_TEST(request.unregister_value->namespace_name == namespace_name);
         BOOST_REQUIRE(request.unregister_value->peer);
         BOOST_CHECK(*request.unregister_value->peer == incoming.session.remote_peer);
         BOOST_REQUIRE(unregister_requests.fetch_add(1) == 0U);
         unregister_request_fin.store(true);
         received.set_value();
         static_cast<void>(co_await allow_remove.async_wait(0));
         if (response == reply::fin) {
            co_await point->peers().async_remove_rendezvous(incoming.session.remote_peer, std::string{namespace_name});
            removed.set_value();
            static_cast<void>(co_await allow_fin.async_wait(0));
         } else if (response == reply::reset) {
            co_await cancel_and_join(incoming.stream);
            // Join native cancellation before inspecting it: a second read of
            // the already-consumed request FIN is not reset publication.
            auto termination = peer_termination::unexpected;
            try {
               static_cast<void>(co_await incoming.stream.async_read());
            } catch (const forge::exceptions::base& error) {
               termination = classify_peer_termination(
                   error, incoming.stream.authentication() == p2p::peer_authentication::quic_tls);
            }
            BOOST_REQUIRE_MESSAGE(termination == peer_termination::stream_reset ||
                termination == peer_termination::cancellation_cleanup,
                "reset donor did not publish typed reset/cancellation after its native join");
            cleanup_terminated.set_value(termination);
            co_return;
         } else {
            if (response == reply::unexpected_data) {
               const auto payload = std::vector<std::uint8_t>{0x51};
               co_await incoming.stream.async_write(payload);
            }
            // The test releases this only after client shutdown has joined.
            // Observe local fixture cleanup, not proof of remote cancellation.
            static_cast<void>(co_await allow_cleanup.async_wait(0));
            try { co_await cancel_and_join(incoming.stream); }
            catch (const forge::exceptions::base& error) {
               // Only this post-shutdown gate permits connection teardown;
               // normal request EOF remains explicitly excluded by classification.
               if (classify_peer_termination(error,
                       incoming.stream.authentication() == p2p::peer_authentication::quic_tls) !=
                   peer_termination::connection_cleanup) { throw; }
            }
            auto termination = peer_termination::unexpected;
            try {
               static_cast<void>(co_await incoming.stream.async_read());
            } catch (const forge::exceptions::base& error) {
               termination = classify_peer_termination(
                   error, incoming.stream.authentication() == p2p::peer_authentication::quic_tls);
            }
            cleanup_terminated.set_value(termination);
            co_return;
         }
      } else { throw std::runtime_error{"unexpected Rendezvous fixture request"}; }
      co_await incoming.stream.async_close();
   }
};

struct unregister_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   std::shared_ptr<donor_state> state;
   p2p::node point;
   std::unique_ptr<p2p::node> client;
   std::future<void> received;
   std::future<void> removed;
   std::future<peer_termination> cleanup_terminated;
   bool finished = false;

   unregister_fixture(bool quic, reply response, p2p::protocol_id probe_protocol = {},
                      p2p::node::protocol_handler probe_handler = {}) : state(std::make_shared<donor_state>(response)),
       point(runtime, options_for("unregister-point")) {
      state->point = &point;
      received = state->received.get_future();
      removed = state->removed.get_future();
      cleanup_terminated = state->cleanup_terminated.get_future();
      // Client-only role has no built-in Rendezvous handler to bypass or duplicate.
      point.register_protocol_handler(p2p::builtins::rendezvous,
          [state = state](p2p::node::incoming_protocol_stream incoming) {
             return state->handle(std::move(incoming));
          });
      if (probe_handler) {
         point.register_protocol_handler(std::move(probe_protocol), std::move(probe_handler));
      }
      const auto listen = p2p::parse_endpoint(quic ? "/ip4/127.0.0.1/udp/0/quic-v1" : "/ip4/127.0.0.1/tcp/0");
      run(point.async_listen(listen));
      auto coordinate = point.local_endpoint();
      BOOST_REQUIRE(coordinate);
      coordinate->peer = point.local_peer();
      auto options = options_for("unregister-client");
      auto& topology = options.limits.topology;
      topology.operating_mode = p2p::topology::mode::managed;
      topology.peers = {.low = 1, .target = 1, .high = 3};
      topology.refresh_interval = 60s;
      topology.query_timeout = response == reply::silent ? 1s : 3s;
      topology.max_parallel_queries = 1;
      topology.max_parallel_dials = 1;
      topology.retry_jitter = 0;
      topology.rendezvous_points = {{.endpoint = *coordinate, .namespaces = {std::string{namespace_name}}}};
      client = std::make_unique<p2p::node>(runtime, std::move(options));
      run(client->async_listen(listen));
      static_cast<void>(run(client->async_start()));
      static_cast<void>(run(client->async_refresh_discovery()));
      BOOST_REQUIRE_EQUAL(point.peers().discover_rendezvous(namespace_name, 0, 8).size(), 1U);
      const auto sessions = client->diagnostics().sessions;
      BOOST_REQUIRE_EQUAL(sessions.size(), 1U);
      BOOST_CHECK(sessions.front().remote_peer == point.local_peer());
      BOOST_CHECK(sessions.front().identify_state == p2p::identify::state::identified);
      BOOST_CHECK(sessions.front().authentication == (quic ? p2p::peer_authentication::quic_tls
                                                          : p2p::peer_authentication::noise));
   }

   void finish() {
      state->allow_remove.notify();
      state->allow_fin.notify();
      state->allow_cleanup.notify();
      auto failure = std::exception_ptr{};
      for (auto* node : {client.get(), &point}) {
         if (!node) { continue; }
         node->request_stop();
         try { run(node->async_stop()); }
         catch (...) { if (!failure) { failure = std::current_exception(); } }
      }
      finished = !failure;
      if (failure) { std::rethrow_exception(failure); }
   }

   ~unregister_fixture() noexcept {
      if (finished) { return; }
      state->allow_remove.notify();
      state->allow_fin.notify();
      state->allow_cleanup.notify();
      for (auto* node : {client.get(), &point}) {
         if (!node) { continue; }
         node->request_stop();
         try {
            auto stop = boost::asio::co_spawn(runtime.context(), node->async_stop(), boost::asio::use_future);
            if (stop.wait_for(5s) == std::future_status::ready) { stop.get(); }
         } catch (...) {}
      }
   }

   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }

   std::future<void> stop() {
      client->request_stop();
      return boost::asio::co_spawn(runtime.context(), client->async_stop(), boost::asio::use_future);
   }

   void require_held(std::future<void>& stopping) {
      // Keep the donor barrier held long enough to reject write-then-close completion.
      BOOST_REQUIRE(stopping.wait_for(150ms) == std::future_status::timeout);
      BOOST_TEST(state->unregister_request_fin.load());
      require_outbound_resources(*client, 1);
   }
};

auto capture_unregister_failures(const std::shared_ptr<donor_state>& state) {
   forge::exceptions::set_log_sink([state](std::string_view message) {
      if (message.starts_with("P2P topology rendezvous unregister failed")) {
         const auto lock = std::scoped_lock{state->failures_mutex};
         state->failures.push_back(std::current_exception());
      }
   });
   return boost::scope::scope_exit{[] { forge::exceptions::set_log_sink({}); }};
}

struct half_close_probe {
   std::promise<void> request_finished;
   std::promise<void> finished;
   forge::asio::notification allow_response;

   boost::asio::awaitable<void> handle(p2p::node::incoming_protocol_stream incoming, bool cancel) {
      const auto request = co_await incoming.stream.async_read();
      BOOST_REQUIRE_EQUAL(request.size(), 1U);
      BOOST_TEST(request.front() == 0x41U);
      co_await require_request_fin(incoming.stream);
      request_finished.set_value();
      static_cast<void>(co_await allow_response.async_wait(0));
      if (cancel) { co_await cancel_and_join(incoming.stream); }
      else {
         const auto response = std::vector<std::uint8_t>{0x52};
         co_await incoming.stream.async_write(response);
         co_await incoming.stream.async_close();
      }
      finished.set_value();
   }
};
} // namespace

BOOST_AUTO_TEST_CASE(p2p_write_fin_retains_read_cancellation_and_resource_ownership) {
   for (const auto quic : {false, true}) {
      for (const auto cancel : {false, true}) {
         BOOST_TEST_CONTEXT("quic=" << quic << " cancel=" << cancel) {
            auto probe = std::make_shared<half_close_probe>();
            auto request_finished = probe->request_finished.get_future();
            auto finished = probe->finished.get_future();
            const auto protocol = p2p::protocol_id{.value = "/forge/test/write-fin/1"};
            auto fixture = unregister_fixture{quic, reply::fin, protocol,
                [probe, cancel](p2p::node::incoming_protocol_stream incoming) {
                   return probe->handle(std::move(incoming), cancel);
                }};
            auto release = boost::scope::scope_exit{[probe] { probe->allow_response.notify(); }};
            auto stream = fixture.run(fixture.client->async_open_protocol_stream(fixture.point.local_peer(), protocol));
            fixture.run(wait_for_probe_resource_quiescence(*fixture.client));
            require_outbound_resources(*fixture.client, 1);
            const auto request = std::vector<std::uint8_t>{0x41};
            fixture.run(stream.async_write(request));
            fixture.run(stream.async_close_write());
            joined(request_finished);
            BOOST_TEST(stream.valid());
            require_outbound_resources(*fixture.client, 1);
            const auto write_closed = [](const forge::exceptions::base& error) {
               return !request_fin(error) &&
                   (forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::closed) ||
                    forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed));
            };
            BOOST_CHECK_EXCEPTION(fixture.run(stream.async_write(request)), forge::exceptions::base, write_closed);
            BOOST_CHECK_EXCEPTION(fixture.run(stream.async_write_frame(request)), forge::exceptions::base, write_closed);
            auto reading = boost::asio::co_spawn(fixture.runtime.context(), stream.async_read(), boost::asio::use_future);
            BOOST_REQUIRE(reading.wait_for(150ms) == std::future_status::timeout);
            require_outbound_resources(*fixture.client, 1);
            if (cancel) {
               stream.request_cancel();
               BOOST_CHECK_EXCEPTION(joined(reading), forge::exceptions::base, [quic](const auto& error) {
                  const auto termination = classify_peer_termination(error, quic);
                  return termination == peer_termination::stream_reset ||
                      termination == peer_termination::cancellation_cleanup;
               });
               require_outbound_resources(*fixture.client, 1);
               fixture.run(cancel_and_join(stream));
            } else {
               probe->allow_response.notify();
               BOOST_TEST(joined(reading) == std::vector<std::uint8_t>{0x52}, boost::test_tools::per_element());
               BOOST_CHECK_EXCEPTION(fixture.run(stream.async_read()), forge::exceptions::base, request_fin);
               require_outbound_resources(*fixture.client, 1);
               fixture.run(stream.async_close());
            }
            require_outbound_resources(*fixture.client, 0);
            probe->allow_response.notify();
            joined(finished);
            fixture.finish();
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_unregister_retains_operation_and_stream_until_processed_remote_fin) {
   for (const auto quic : {false, true}) {
      BOOST_TEST_CONTEXT("quic=" << quic) {
         auto fixture = unregister_fixture{quic, reply::fin};
         auto capture = capture_unregister_failures(fixture.state);
         auto stopping = fixture.stop();
         joined(fixture.received);
         fixture.require_held(stopping);
         BOOST_REQUIRE_EQUAL(fixture.point.peers().discover_rendezvous(namespace_name, 0, 8).size(), 1U);
         fixture.state->allow_remove.notify();
         joined(fixture.removed);
         BOOST_TEST(fixture.point.peers().discover_rendezvous(namespace_name, 0, 8).empty());
         fixture.require_held(stopping);
         fixture.state->allow_fin.notify();
         joined(stopping);
         require_outbound_resources(*fixture.client, 0);
         BOOST_TEST(fixture.state->unregister_requests.load() == 1U);
         {
            const auto lock = std::scoped_lock{fixture.state->failures_mutex};
            BOOST_TEST(fixture.state->failures.empty());
         }
         fixture.finish();
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_rendezvous_unregister_timeout_reset_and_payload_preserve_failure_and_release_resources) {
   for (const auto quic : {false, true}) {
      for (const auto response : {reply::silent, reply::reset, reply::unexpected_data}) {
         BOOST_TEST_CONTEXT("quic=" << quic << " response=" << static_cast<int>(response)) {
            auto fixture = unregister_fixture{quic, response};
            auto capture = capture_unregister_failures(fixture.state);
            const auto started = std::chrono::steady_clock::now();
            auto stopping = fixture.stop();
            joined(fixture.received);
            fixture.require_held(stopping);
            if (response != reply::silent) { fixture.state->allow_remove.notify(); }
            joined(stopping);
            BOOST_TEST((std::chrono::steady_clock::now() - started < 4s));
            fixture.state->allow_remove.notify();
            fixture.state->allow_cleanup.notify();
            const auto termination = joined(fixture.cleanup_terminated);
            BOOST_CHECK_MESSAGE(termination == peer_termination::stream_reset ||
                termination == peer_termination::connection_cleanup ||
                termination == peer_termination::cancellation_cleanup,
                "unexpected joined donor cleanup: " << static_cast<int>(termination));
            require_outbound_resources(*fixture.client, 0);
            BOOST_TEST(fixture.client->diagnostics().sessions.empty());
            BOOST_TEST(fixture.point.diagnostics().resources.system.inbound_streams == 0U);
            BOOST_TEST(fixture.state->unregister_requests.load() == 1U);
            BOOST_REQUIRE_EQUAL(fixture.point.peers().discover_rendezvous(namespace_name, 0, 8).size(), 1U);
            auto failure = std::exception_ptr{};
            {
               const auto lock = std::scoped_lock{fixture.state->failures_mutex};
               BOOST_REQUIRE_EQUAL(fixture.state->failures.size(), 1U);
               failure = fixture.state->failures.front();
            }
            BOOST_REQUIRE(failure);
            try { std::rethrow_exception(failure); }
            catch (const forge::exceptions::base& error) {
               if (response == reply::silent) { BOOST_CHECK(p2p::exceptions::is(error, p2p::exceptions::code::timeout)); }
               else if (response == reply::unexpected_data) {
                  BOOST_CHECK(p2p::exceptions::is(error, p2p::exceptions::code::protocol_error));
               } else {
                  BOOST_CHECK(forge::net::yamux::exceptions::code_of(error) == forge::net::yamux::exceptions::code::stream_reset ||
                              forge::net::quic::exceptions::code_of(error) == forge::net::quic::exceptions::code::stream_reset);
               }
            }
            fixture.finish();
         }
      }
   }
}
