module;

#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include "quic_p2p/libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.crypto.asymmetric.x25519;
import forge.crypto.pki.x509;
import forge.multiformats.varint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.pnet.protector;
import forge.net.stcp.connection;
import forge.net.tcp.connection;
import forge.net.tcp.connector;
import forge.net.tcp.listener;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../libraries/net/p2p/details/direct_transport.hxx"
#include "../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../libraries/net/p2p/details/identity_signature.hxx"
#include "../libraries/net/p2p/details/libp2p_tls.hxx"
#include "../libraries/net/p2p/details/lifecycle_wakeup.hxx"
#include "../libraries/net/p2p/details/length_delimited.hxx"
#include "../libraries/net/p2p/details/noise_handshake.hxx"
#include "../libraries/net/p2p/details/path_manager.hxx"
#include "../libraries/net/p2p/details/resource_stream.hxx"
#include "../libraries/net/p2p/details/stream_upgrade.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] libp2p_identity_material muxer_identity(std::string_view name) {
   const auto fixture = forge::tests::p2p::make_identity_fixture(name);
   return make_libp2p_identity_material(node::options{.private_key_pem = fixture.private_key_pem});
}

[[nodiscard]] peer_id muxer_peer(const libp2p_identity_material& identity) {
   return make_peer_id(decode_public_key(identity.public_key));
}

[[nodiscard]] endpoint muxer_endpoint() {
   return parse_endpoint("/ip4/127.0.0.1/tcp/0");
}

[[nodiscard]] detail::noise_handshake_payload signed_noise_payload(const libp2p_identity_material& identity,
                                                                   std::span<const std::uint8_t> static_key) {
   constexpr auto prefix = std::string_view{"noise-libp2p-static-key:"};
   auto message = std::vector<std::uint8_t>{prefix.begin(), prefix.end()};
   message.insert(message.end(), static_key.begin(), static_key.end());
   return detail::noise_handshake_payload{
       .identity_key = identity.public_key,
       .identity_signature = sign_identity(require_libp2p_identity_private_key(identity), message),
   };
}

[[nodiscard]] forge::net::stcp::certificate_chain muxer_certificate_chain(const libp2p_identity_material& identity) {
   const auto material = make_libp2p_tls_material(identity);
   const auto certificate = forge::crypto::pki::x509::certificate::from_pem(material.certificate_pem);
   auto chain = forge::net::stcp::certificate_chain{};
   chain.certificates.push_back(forge::net::stcp::peer_certificate{.der = certificate.der()});
   return chain;
}

template <typename Session>
void check_muxer_payload(forge::asio::runtime& runtime, Session& outbound, Session& inbound) {
   auto accepted = std::future<forge::net::transport::stream>{};
   auto sent = forge::net::transport::stream{};
   auto received = forge::net::transport::stream{};
   auto completed = false;
   auto cleanup = boost::scope::scope_exit{[&] {
      if (completed) {
         return;
      }
      outbound.request_cancel();
      inbound.request_cancel();
      if (accepted.valid()) {
         accepted.wait();
         try {
            received = accepted.get();
         } catch (...) {
         }
      }
      for (auto* value : {&sent, &received}) {
         value->request_cancel();
         try {
            forge::asio::blocking::run(runtime, value->async_close());
         } catch (...) {
         }
      }
   }};
   accepted = boost::asio::co_spawn(runtime.context(), inbound.async_accept_stream(), boost::asio::use_future);
   sent = forge::asio::blocking::run(runtime, outbound.async_open_stream());
   // QUIC publishes a peer stream on DATA, not on local stream allocation.
   const auto payload = std::vector<std::uint8_t>{0x01, 0x02, 0x03};
   forge::asio::blocking::run(runtime, sent.async_write(payload));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   received = accepted.get();
   const auto actual = forge::asio::blocking::run(runtime, received.async_read());
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), payload.begin(), payload.end());
   forge::asio::blocking::run(runtime, sent.async_close());
   forge::asio::blocking::run(runtime, received.async_close());
   completed = true;
}

// Pinned Rust TLS offers only "libp2p" and then negotiates Yamux on the
// authenticated stream. Exercise that peer shape, not a second Forge profile.
boost::asio::awaitable<upgraded_session> legacy_tls_peer(forge::net::tcp::connection connection,
                                                         const libp2p_identity_material& identity, bool outbound) {
   auto raw = std::move(connection).into_transport_stream();
   const auto tls_protocol = protocol_id{.value = "/tls/1.0.0"};
   if (outbound) {
      auto selected = co_await protocol_negotiation::async_select(std::move(raw.stream), tls_protocol);
      raw.stream = std::move(selected).into_transport_stream();
   } else {
      auto selected = co_await protocol_negotiation::async_accept(std::move(raw.stream), {tls_protocol});
      raw.stream = std::move(selected.stream).into_transport_stream();
   }
   auto tls = forge::net::stcp::connection{};
   if (outbound) {
      auto options = make_libp2p_tls_client_options(identity);
      options.alpn_protocols = {"libp2p"};
      tls = co_await forge::net::stcp::async_upgrade_client(std::move(raw), std::move(options),
                                                            std::chrono::milliseconds{5000});
   } else {
      auto options = make_libp2p_tls_server_options(identity);
      options.alpn_protocols = {"libp2p"};
      tls = co_await forge::net::stcp::async_upgrade_server(std::move(raw), std::move(options),
                                                            std::chrono::milliseconds{5000});
   }
   BOOST_REQUIRE_EQUAL(tls.selected_alpn(), "libp2p");
   const auto peer = verify_libp2p_tls_chain(tls.peer_certificate_chain(), std::nullopt);
   auto secured = std::move(tls).into_transport_stream();
   auto muxer = protocol_id{.value = "/yamux/1.0.0"};
   if (outbound) {
      auto selected = co_await protocol_negotiation::async_select(std::move(secured.stream), muxer);
      secured.stream = std::move(selected).into_transport_stream();
   } else {
      auto selected = co_await protocol_negotiation::async_accept(std::move(secured.stream), {muxer});
      muxer = std::move(selected.protocol);
      secured.stream = std::move(selected.stream).into_transport_stream();
   }
   co_return upgraded_session{
       .peer = peer,
       .session = std::make_shared<forge::net::yamux::session>(std::move(secured.stream),
                                                               outbound ? forge::net::yamux::side::initiator
                                                                        : forge::net::yamux::side::responder),
       .authentication = peer_authentication::libp2p_tls,
       .muxer = std::move(muxer),
       .used_early_muxer_negotiation = false,
       .role = outbound ? upgrade_role::initiator : upgrade_role::responder,
   };
}

boost::asio::awaitable<upgraded_session> accept_noise_peer(forge::net::tcp::listener& listener,
                                                           const node::options& options,
                                                           const libp2p_identity_material& identity,
                                                           peer_id expected_peer) {
   auto connection = co_await listener.async_accept_connection();
   co_return co_await upgrade_inbound_stream(stream{std::move(connection).into_transport_stream().stream}, options,
                                             identity, std::move(expected_peer));
}

void stop_registry(forge::asio::runtime& runtime, direct::registry& registry) {
   auto teardown = registry.teardown_operation();
   forge::asio::blocking::run(runtime, teardown.close());
}

void check_ordinary_observed_source(bool quic, bool private_profile, std::string_view listener_host) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_fixture = forge::tests::p2p::make_identity_fixture("reuse-observation-server");
   const auto client_fixture = forge::tests::p2p::make_identity_fixture("reuse-observation-client");
   auto server_options = node::options{.certificate_pem = server_fixture.certificate_pem,
                                       .private_key_pem = server_fixture.private_key_pem};
   auto client_options = node::options{.certificate_pem = client_fixture.certificate_pem,
                                       .private_key_pem = client_fixture.private_key_pem};
   server_options.peer_state.persistence = peer_store::make_memory_persistence();
   client_options.peer_state.persistence = peer_store::make_memory_persistence();
   if (private_profile) {
      const auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
      const auto protector = std::make_shared<const forge::net::pnet::protector>(forge::net::pnet::pre_shared_key{key});
      for (auto* options : {&server_options, &client_options}) {
         options->capabilities = capability_set{.bits = capabilities::peer_exchange};
         options->relay_policy.service_enabled = false;
         options->relay_policy.client_enabled = false;
         options->relay_policy.auto_discovery_enabled = false;
         options->path_policy.allow_relay = false;
         options->path_policy.allow_hole_punch = false;
         options->private_network = private_network::options{.protector = protector};
      }
   } else {
      // The observer can serve relay connections, but this is an ordinary
      // authenticated direct dial to it, not a coordinated test bypass.
      server_options.relay_policy.service_enabled = true;
   }
   auto server = node{runtime, std::move(server_options)};
   auto client = node{runtime, std::move(client_options)};
   const auto suffix = quic ? "/udp/0/quic-v1" : "/tcp/0";
   forge::asio::blocking::run(runtime, server.async_listen(parse_endpoint("/ip4/127.0.0.1" + std::string{suffix})));
   forge::asio::blocking::run(runtime,
                              client.async_listen(parse_endpoint("/ip4/" + std::string{listener_host} + suffix)));
   BOOST_REQUIRE(server.local_endpoint().has_value());
   BOOST_REQUIRE(client.local_endpoint().has_value());
   const auto source = *client.local_endpoint();
   const auto connected = forge::asio::blocking::run(
       runtime,
       client.async_connect(*server.local_endpoint(), node::connect_options{.expected_peer = server.local_peer(),
                                                                            .allow_relay = false,
                                                                            .timeout = std::chrono::seconds{5},
                                                                            .allow_hole_punch = false}));
   BOOST_TEST(connected.remote_peer.to_string() == server.local_peer().to_string());
   auto stream =
       forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(), builtins::identify));
   auto buffer = std::vector<std::uint8_t>{};
   const auto limits = identify::limits{};
   // Forge writes one Identify part; the framed reader handles arbitrary read
   // boundaries and retains the varint length, which is not protobuf data.
   const auto framed =
       forge::asio::blocking::run(runtime, async_read_length_delimited(stream, buffer, limits.max_message_size));
   const auto length = forge::multiformats::varint_decode(framed);
   BOOST_REQUIRE_EQUAL(framed.size(), length.size + length.value);
   const auto document = identify::decode(std::span<const std::uint8_t>{framed}.subspan(length.size), limits);
   BOOST_REQUIRE(document.observed_endpoint.has_value());
   BOOST_TEST(document.observed_endpoint->transport.host == "127.0.0.1");
   BOOST_TEST(document.observed_endpoint->transport.port == source.transport.port);
   BOOST_TEST(document.observed_endpoint->is_direct_quic() == quic);
   BOOST_TEST(client.local_endpoint()->transport.port == source.transport.port);
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 0U);
   BOOST_TEST(server.diagnostics().resources.system.file_descriptors == 0U);
}

enum class rst_fixture_failure { none, before_response_notify, before_sync_notify };

class rst_fixture_fault final : public std::runtime_error {
 public:
   rst_fixture_fault() : std::runtime_error{"controlled RST fixture failure before notification"} {}
};

struct rst_fixture_control {
   std::stop_source stop;
   forge::asio::notification response_read;
   forge::asio::notification sync_read;
   forge::asio::notification response_wait_entered;
   forge::asio::notification sync_wait_entered;
   std::shared_ptr<forge::net::yamux::session> initiator;
   std::shared_ptr<forge::net::yamux::session> responder;
   std::weak_ptr<forge::net::yamux::session> initiator_owner;
   std::weak_ptr<forge::net::yamux::session> responder_owner;
   bool native_sessions_joined = false;
   std::exception_ptr cleanup_failure;
   std::shared_ptr<cancellation_latch> owner_cancellation;
   std::shared_ptr<cancellation_latch> peer_cancellation;
   resource_manager resources;
   std::atomic_size_t started{0};
   std::atomic_size_t terminal{0};
   std::atomic_bool response_wait_finished{false};
   std::atomic_bool sync_wait_finished{false};
   std::mutex mutex;
   std::exception_ptr failure;

   void request_stop() noexcept {
      stop.request_stop();
      if (owner_cancellation) {
         owner_cancellation->request_stop();
      }
      if (peer_cancellation) {
         peer_cancellation->request_stop();
      }
      if (initiator) {
         initiator->request_cancel();
      }
      if (responder) {
         responder->request_cancel();
      }
   }

   void failed(std::exception_ptr error) noexcept {
      {
         const auto lock = std::scoped_lock{mutex};
         if (!failure) {
            failure = std::move(error);
         }
      }
      request_stop();
   }

   std::exception_ptr first_failure() {
      const auto lock = std::scoped_lock{mutex};
      return failure;
   }
};

boost::asio::awaitable<bool> run_rst_fixture_task(std::shared_ptr<rst_fixture_control> control,
                                                  std::shared_ptr<detail::resource_stream> resource,
                                                  std::function<boost::asio::awaitable<bool>()> work) {
   ++control->started;
   auto terminal = boost::scope::scope_exit{[control] { ++control->terminal; }};
   auto result = false;
   auto failure = std::exception_ptr{};
   try {
      result = co_await work();
   } catch (...) {
      failure = std::current_exception();
      control->failed(failure);
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   try {
      co_await detail::path_manager::async_close_exchange(resource);
   } catch (...) {
      if (!failure) {
         failure = std::current_exception();
         control->failed(failure);
      }
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
   co_return result;
}

void check_native_dcutr_rst(bool complete_sync, bool cancel_after_sync, bool reset_by_initiator = false,
                            rst_fixture_failure fault = rst_fixture_failure::none,
                            std::shared_ptr<rst_fixture_control> control = std::make_shared<rst_fixture_control>()) {
   namespace asio = boost::asio;
   using namespace std::chrono_literals;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto initiator_identity = muxer_identity("dcutr-rst-initiator");
   const auto responder_identity = muxer_identity("dcutr-rst-responder");
   const auto options = node::options{};
   auto listener = forge::net::tcp::listener{runtime.context().get_executor(), muxer_endpoint().transport};
   auto connector = forge::net::tcp::connector{runtime.context().get_executor()};
   auto accepting = std::future<upgraded_session>{};
   auto peer = std::future<bool>{};
   auto result = std::future<bool>{};
   // Installed before any spawn or fatal assertion. Stop owns notification
   // waits and native I/O; every valid future is joined before its state exits.
   auto cleanup = boost::scope::scope_exit{[&] {
      control->request_stop();
      connector.request_cancel();
      try {
         listener.close();
      } catch (...) {
      }
      for (auto* task : {&peer, &result}) {
         if (task->valid()) {
            task->wait();
         }
      }
      if (accepting.valid()) {
         accepting.wait();
         try {
            control->responder = accepting.get().session;
         } catch (...) {
         }
      }
      auto joined_sessions = std::size_t{0};
      for (const auto& session : {control->initiator, control->responder}) {
         if (!session) {
            continue;
         }
         session->request_cancel();
         try {
            forge::asio::blocking::run(runtime, session->async_close());
            ++joined_sessions;
         } catch (const forge::net::yamux::exceptions::canceled&) {
            // Yamux publishes its terminal error after joining native close.
            ++joined_sessions;
         } catch (const forge::net::yamux::exceptions::closed&) {
            ++joined_sessions;
         } catch (...) {
            if (!control->cleanup_failure) {
               control->cleanup_failure = std::current_exception();
            }
         }
      }
      control->native_sessions_joined = joined_sessions == 2;
      // control is a caller-owned argument and outlives this local runtime.
      // Closing a session does not destroy its context-allocated strand.
      control->initiator.reset();
      control->responder.reset();
      control->owner_cancellation.reset();
      control->peer_cancellation.reset();
      try {
         forge::asio::blocking::run(runtime, connector.async_stop());
      } catch (...) {
      }
      try {
         forge::asio::blocking::run(runtime, listener.async_close());
      } catch (...) {
      }
   }};
   accepting = asio::co_spawn(runtime.context(),
                              accept_noise_peer(listener, options, responder_identity, muxer_peer(initiator_identity)),
                              asio::use_future);
   auto native = forge::asio::blocking::run(runtime, connector.async_connect_connection(listener.local_endpoint()));
   // These are actual kernel socket endpoints, not synthetic public/NAT claims.
   const auto initiator_address = endpoint{.transport = native.local_endpoint()};
   const auto responder_address = endpoint{.transport = native.remote_endpoint()};
   auto initiator = forge::asio::blocking::run(
       runtime, upgrade_outbound_stream(stream{std::move(native).into_transport_stream().stream}, options,
                                        initiator_identity, muxer_peer(responder_identity)));
   control->initiator = initiator.session;
   control->initiator_owner = initiator.session;
   BOOST_REQUIRE(accepting.wait_for(5s) == std::future_status::ready);
   auto responder = accepting.get();
   control->responder = responder.session;
   control->responder_owner = responder.session;
   BOOST_CHECK(initiator.authentication == peer_authentication::noise);
   BOOST_CHECK(responder.authentication == peer_authentication::noise);
   BOOST_TEST(initiator.muxer.value == "/yamux/1.0.0");
   // The same authenticated sessions must survive both challenge streams.
   check_muxer_payload(runtime, *initiator.session, *responder.session);

   auto reservation = control->resources.reserve_stream(initiator.peer, resource_manager::session_direction::outbound);
   BOOST_REQUIRE(reservation);
   const auto resource = std::make_shared<detail::resource_stream>(std::move(*reservation));
   auto peer_reservation =
       control->resources.reserve_stream(responder.peer, resource_manager::session_direction::inbound);
   BOOST_REQUIRE(peer_reservation);
   const auto peer_resource = std::make_shared<detail::resource_stream>(std::move(*peer_reservation));
   const auto paths = std::make_shared<detail::path_manager>(std::make_shared<detail::lifecycle_wakeup>());
   const auto now = std::chrono::steady_clock::now();
   const auto owner = paths->begin(initiator.peer, 1, detail::path_manager::role::initiator, now + 5s).owner;
   const auto peer_owner = paths->begin(responder.peer, 2, detail::path_manager::role::responder, now + 5s).owner;
   BOOST_REQUIRE(owner);
   BOOST_REQUIRE(peer_owner);
   control->owner_cancellation = owner->cancellation;
   control->peer_cancellation = peer_owner->cancellation;
   // Coroutine captures own all state. Even setup/launch failure cannot leave
   // references into this fixture frame in a suspended native task.
   const auto exchange = [control, paths, resource, owner, initiator_address, responder_address, complete_sync,
                          cancel_after_sync, reset_by_initiator, fault]() -> asio::awaitable<bool> {
      resource->attach(co_await control->initiator->async_open_stream());
      auto outgoing = stream{forge::net::transport::detail::stream_access::make(resource)};
      auto buffer = std::vector<std::uint8_t>{};
      const auto sent = std::chrono::steady_clock::now();
      co_await outgoing.async_write(hole_punch::codec::encode(hole_punch::message{
          .kind = hole_punch::message::message_kind::connect, .observed_endpoints = {initiator_address}}));
      const auto response = hole_punch::codec::decode(
          co_await async_read_length_delimited(outgoing, buffer, hole_punch::options{}.max_message_size));
      const auto rtt = std::chrono::steady_clock::now() - sent;
      BOOST_CHECK(response.kind == hole_punch::message::message_kind::connect);
      BOOST_REQUIRE_EQUAL(response.observed_endpoints.size(), 1U);
      BOOST_TEST(response.observed_endpoints.front().to_string() == responder_address.to_string());
      BOOST_TEST(buffer.empty());
      if (fault == rst_fixture_failure::before_response_notify) {
         static_cast<void>(co_await control->response_wait_entered.async_wait(0, control->stop.get_token()));
         throw rst_fixture_fault{};
      }
      control->response_read.notify();
      auto delayed = false;
      if (complete_sync) {
         co_await outgoing.async_write(
             hole_punch::codec::encode(hole_punch::message{.kind = hole_punch::message::message_kind::sync}));
         if (reset_by_initiator) {
            // Fixture control waits for a real validated read, not a guessed
            // scheduling delay. No extra protocol bytes are sent.
            control->sync_wait_entered.notify();
            auto waiting = boost::scope::scope_exit{[control] { control->sync_wait_finished = true; }};
            static_cast<void>(co_await control->sync_read.async_wait(0, control->stop.get_token()));
            resource->request_cancel();
            try {
               co_await paths->async_close_exchange(resource);
            } catch (const forge::net::yamux::exceptions::stream_reset&) {
            }
            co_return false;
         }
         delayed = co_await paths->async_delay(owner, std::chrono::steady_clock::now() + rtt / 2);
         BOOST_TEST(delayed);
         if (cancel_after_sync) {
            owner->cancellation->request_stop();
         }
      }
      // Wait for the peer's real RST to be consumed by Yamux before testing
      // terminal close. No injected exception or text-based classification.
      auto reset_observed = false;
      try {
         static_cast<void>(co_await outgoing.async_read());
      } catch (const forge::net::yamux::exceptions::stream_reset&) {
         reset_observed = true;
      }
      BOOST_TEST(reset_observed);
      auto close_reset = false;
      if (delayed) {
         co_await paths->async_close_completed_exchange(resource);
      } else {
         try {
            co_await paths->async_close_exchange(resource);
         } catch (const forge::net::yamux::exceptions::stream_reset&) {
            close_reset = true;
         }
      }
      BOOST_TEST(close_reset == !complete_sync);
      BOOST_TEST(!resource->valid());
      BOOST_TEST(control->resources.current().system.outbound_streams == 0U);
      co_return delayed && !owner->cancellation->stop_requested() && std::chrono::steady_clock::now() < owner->deadline;
   };
   const auto peer_exchange = [control, paths, peer_resource, peer_owner, initiator_address, responder_address,
                               complete_sync, cancel_after_sync, reset_by_initiator, fault]() -> asio::awaitable<bool> {
      peer_resource->attach(co_await control->responder->async_accept_stream());
      auto incoming = stream{forge::net::transport::detail::stream_access::make(peer_resource)};
      auto buffer = std::vector<std::uint8_t>{};
      const auto request = hole_punch::codec::decode(
          co_await async_read_length_delimited(incoming, buffer, hole_punch::options{}.max_message_size));
      BOOST_CHECK(request.kind == hole_punch::message::message_kind::connect);
      BOOST_REQUIRE_EQUAL(request.observed_endpoints.size(), 1U);
      BOOST_TEST(request.observed_endpoints.front().to_string() == initiator_address.to_string());
      co_await incoming.async_write(hole_punch::codec::encode(hole_punch::message{
          .kind = hole_punch::message::message_kind::connect, .observed_endpoints = {responder_address}}));
      if (complete_sync) {
         const auto sync = hole_punch::codec::decode(
             co_await async_read_length_delimited(incoming, buffer, hole_punch::options{}.max_message_size));
         BOOST_CHECK(sync.kind == hole_punch::message::message_kind::sync);
         BOOST_TEST(sync.observed_endpoints.empty());
         BOOST_TEST(buffer.empty());
         if (fault == rst_fixture_failure::before_sync_notify) {
            static_cast<void>(co_await control->sync_wait_entered.async_wait(0, control->stop.get_token()));
            throw rst_fixture_fault{};
         }
         control->sync_read.notify();
         if (reset_by_initiator) {
            if (cancel_after_sync) {
               peer_owner->cancellation->request_stop();
            }
            auto reset_observed = false;
            try {
               static_cast<void>(co_await incoming.async_read());
            } catch (const forge::net::yamux::exceptions::stream_reset&) {
               reset_observed = true;
            }
            BOOST_TEST(reset_observed);
            // Responder has no RTT/2 gate: complete validated SYNC is sufficient.
            co_await paths->async_close_completed_exchange(peer_resource);
            BOOST_TEST(!peer_resource->valid());
            BOOST_TEST(control->resources.current().system.inbound_streams == 0U);
            co_return !peer_owner->cancellation->stop_requested() &&
                std::chrono::steady_clock::now() < peer_owner->deadline;
         }
      } else {
         control->response_wait_entered.notify();
         auto waiting = boost::scope::scope_exit{[control] { control->response_wait_finished = true; }};
         static_cast<void>(co_await control->response_read.async_wait(0, control->stop.get_token()));
      }
      // Matches the pinned Rust behavior: drop/reset the negotiation stream
      // immediately after reading SYNC, without closing its Yamux session.
      peer_resource->request_cancel();
      try {
         co_await paths->async_close_exchange(peer_resource);
      } catch (const forge::net::yamux::exceptions::stream_reset&) {
      }
      co_return false;
   };
   peer =
       asio::co_spawn(runtime.context(), run_rst_fixture_task(control, peer_resource, peer_exchange), asio::use_future);
   result = asio::co_spawn(runtime.context(), run_rst_fixture_task(control, resource, exchange), asio::use_future);
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   if (result.wait_until(deadline) != std::future_status::ready ||
       peer.wait_until(deadline) != std::future_status::ready) {
      control->failed(std::make_exception_ptr(std::runtime_error{"native RST fixture deadline expired"}));
   }
   // Cancellation is a request, not a join. Do not assert/throw until both
   // tasks actually terminate; cleanup repeats this barrier on every exit.
   peer.wait();
   result.wait();
   auto peer_result = false;
   auto initiator_result = false;
   try {
      peer_result = peer.get();
   } catch (...) {
      control->failed(std::current_exception());
   }
   try {
      initiator_result = result.get();
   } catch (...) {
      control->failed(std::current_exception());
   }
   if (const auto failure = control->first_failure()) {
      std::rethrow_exception(failure);
   }
   BOOST_TEST(peer_result == (reset_by_initiator && complete_sync && !cancel_after_sync));
   BOOST_TEST(initiator_result == (!reset_by_initiator && complete_sync && !cancel_after_sync));
   BOOST_TEST(control->terminal.load() == 2U);
   BOOST_TEST(control->resources.current().system.outbound_streams == 0U);
   BOOST_TEST(control->resources.current().system.inbound_streams == 0U);
   // A stream-local RST must not retire the authenticated carrier session.
   check_muxer_payload(runtime, *initiator.session, *responder.session);
   paths->finish(owner,
                 complete_sync && !cancel_after_sync ? hole_punch::status::succeeded : hole_punch::status::failed);
   paths->finish(peer_owner,
                 complete_sync && !cancel_after_sync ? hole_punch::status::succeeded : hole_punch::status::failed);
   BOOST_TEST(paths->active() == 0U);
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_inline_muxer_tests)

BOOST_AUTO_TEST_CASE(native_dcutr_rst_after_sync_keeps_completed_exchange) {
   // Retain control beyond the fixture's runtime deliberately. It must retain
   // receipts only, never executor owners allocated by the destroyed context.
   const auto control = std::make_shared<rst_fixture_control>();
   check_native_dcutr_rst(true, false, false, rst_fixture_failure::none, control);
   BOOST_TEST(control->native_sessions_joined);
   BOOST_CHECK(!control->cleanup_failure);
   BOOST_CHECK(!control->initiator);
   BOOST_CHECK(!control->responder);
   BOOST_TEST(control->initiator_owner.expired());
   BOOST_TEST(control->responder_owner.expired());
}

BOOST_AUTO_TEST_CASE(native_dcutr_rst_before_sync_does_not_complete_exchange) {
   check_native_dcutr_rst(false, false);
}

BOOST_AUTO_TEST_CASE(native_dcutr_cancel_after_sync_still_prevents_dial) {
   check_native_dcutr_rst(true, true);
}

BOOST_AUTO_TEST_CASE(native_dcutr_responder_rst_after_sync_keeps_completed_exchange) {
   check_native_dcutr_rst(true, false, true);
}

BOOST_AUTO_TEST_CASE(native_dcutr_responder_cancel_after_sync_still_prevents_dial) {
   check_native_dcutr_rst(true, true, true);
}

BOOST_AUTO_TEST_CASE(native_dcutr_failure_before_response_notify_cancels_wait_and_joins_both_tasks) {
   const auto control = std::make_shared<rst_fixture_control>();
   BOOST_CHECK_THROW(check_native_dcutr_rst(false, false, false, rst_fixture_failure::before_response_notify, control),
                     rst_fixture_fault);
   BOOST_TEST(control->started.load() == 2U);
   BOOST_TEST(control->terminal.load() == 2U);
   BOOST_TEST(control->response_wait_finished.load());
   BOOST_TEST(control->stop.stop_requested());
   BOOST_TEST(control->resources.current().system.outbound_streams == 0U);
   BOOST_TEST(control->resources.current().system.inbound_streams == 0U);
   BOOST_TEST(control->native_sessions_joined);
   BOOST_CHECK(!control->cleanup_failure);
   BOOST_CHECK(!control->initiator);
   BOOST_CHECK(!control->responder);
   BOOST_TEST(control->initiator_owner.expired());
   BOOST_TEST(control->responder_owner.expired());
}

BOOST_AUTO_TEST_CASE(native_dcutr_failure_before_sync_notify_cancels_wait_and_joins_both_tasks) {
   const auto control = std::make_shared<rst_fixture_control>();
   BOOST_CHECK_THROW(check_native_dcutr_rst(true, false, true, rst_fixture_failure::before_sync_notify, control),
                     rst_fixture_fault);
   BOOST_TEST(control->started.load() == 2U);
   BOOST_TEST(control->terminal.load() == 2U);
   BOOST_TEST(control->sync_wait_finished.load());
   BOOST_TEST(control->stop.stop_requested());
   BOOST_TEST(control->resources.current().system.outbound_streams == 0U);
   BOOST_TEST(control->resources.current().system.inbound_streams == 0U);
   BOOST_TEST(control->native_sessions_joined);
   BOOST_CHECK(!control->cleanup_failure);
   BOOST_CHECK(!control->initiator);
   BOOST_CHECK(!control->responder);
   BOOST_TEST(control->initiator_owner.expired());
   BOOST_TEST(control->responder_owner.expired());
}

BOOST_AUTO_TEST_CASE(noise_early_data_matches_go_protobuf_and_skips_opaque_extensions) {
   // Go NoiseHandshakePayload.extensions = 4; NoiseExtensions.stream_muxers = 2.
   const auto value = detail::noise_handshake_payload{
       .identity_key = {1}, .identity_signature = {2}, .stream_muxers = {"/yamux/1.0.0"}};
   const auto golden = std::vector<std::uint8_t>{
       0x0a, 0x01, 0x01, 0x12, 0x01, 0x02, 0x22, 0x0e, 0x12, 0x0c, 0x2f,
       0x79, 0x61, 0x6d, 0x75, 0x78, 0x2f, 0x31, 0x2e, 0x30, 0x2e, 0x30,
   };
   const auto encoded = detail::encode_noise_payload(value);
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), golden.begin(), golden.end());
   const auto decoded = detail::decode_noise_payload(golden);
   BOOST_REQUIRE_EQUAL(decoded.stream_muxers.size(), 1U);
   BOOST_TEST(decoded.stream_muxers.front() == "/yamux/1.0.0");

   auto opaque_extension = golden;
   opaque_extension[8] = 0x0a; // webtransport_certhashes: opaque bytes, not a muxer list.
   BOOST_TEST(detail::decode_noise_payload(opaque_extension).stream_muxers.empty());
   auto unknown_extension = golden;
   unknown_extension[8] = 0x1a;
   BOOST_TEST(detail::decode_noise_payload(unknown_extension).stream_muxers.empty());
   auto unknown_payload = golden;
   unknown_payload[6] = 0x1a;
   BOOST_TEST(detail::decode_noise_payload(unknown_payload).stream_muxers.empty());
}

BOOST_AUTO_TEST_CASE(noise_early_data_rejects_malformed_and_discards_oversized_muxer_lists) {
   for (const auto& bytes :
        std::vector<std::vector<std::uint8_t>>{{0x20, 0x01}, {0x22, 0x02, 0x10, 0x01}, {0x22, 0x02, 0x12, 0x01}}) {
      BOOST_CHECK_THROW(static_cast<void>(detail::decode_noise_payload(bytes)), exceptions::codec_error);
   }
   auto payload = detail::noise_handshake_payload{.stream_muxers = std::vector<std::string>(100, "/yamux/1.0.0")};
   auto encoded = detail::encode_noise_payload(payload);
   BOOST_TEST(detail::decode_noise_payload(encoded).stream_muxers.size() == 100U);
   encoded.insert(encoded.end(), {0x22, 0x03, 0x12, 0x01, 0x78});
   BOOST_TEST(detail::decode_noise_payload(encoded).stream_muxers.empty());
   payload.stream_muxers.push_back("x");
   payload.stream_muxers.push_back("/yamux/1.0.0");
   BOOST_TEST(detail::decode_noise_payload(detail::encode_noise_payload(payload)).stream_muxers.empty());
}

BOOST_AUTO_TEST_CASE(noise_muxer_is_accepted_only_after_identity_authentication) {
   const auto identity = muxer_identity("inline-noise-proof");
   const auto other = muxer_identity("inline-noise-other");
   const auto static_key = std::array<std::uint8_t, 32>{0x11};
   auto payload = signed_noise_payload(identity, static_key);
   const auto expected = muxer_peer(identity);
   BOOST_TEST(!detail::verify_noise_payload(payload, static_key, expected).muxer.has_value());
   payload.stream_muxers = std::vector<std::string>(101, "/yamux/1.0.0");
   const auto discarded = detail::decode_noise_payload(detail::encode_noise_payload(payload));
   BOOST_TEST(!detail::verify_noise_payload(discarded, static_key, expected).muxer.has_value());
   payload.stream_muxers = {"/unknown/1.0.0", "/yamux/1.0.0"};
   auto verified = detail::verify_noise_payload(payload, static_key, expected);
   BOOST_REQUIRE(verified.muxer.has_value());
   BOOST_TEST(verified.muxer->value == "/yamux/1.0.0");
   BOOST_TEST(verified.peer.to_string() == expected.to_string());
   BOOST_CHECK_THROW(static_cast<void>(detail::verify_noise_payload(payload, static_key, muxer_peer(other))),
                     exceptions::peer_verification_failed);
   payload.stream_muxers = {"/unknown/1.0.0"};
   BOOST_CHECK_THROW(static_cast<void>(detail::verify_noise_payload(payload, static_key, expected)),
                     exceptions::unsupported_protocol);
   payload.identity_signature.back() ^= 1;
   BOOST_CHECK_THROW(static_cast<void>(detail::verify_noise_payload(payload, static_key, expected)),
                     exceptions::peer_verification_failed);
   payload.stream_muxers = {"/yamux/1.0.0"};
   BOOST_CHECK_THROW(static_cast<void>(detail::verify_noise_payload(payload, static_key, expected)),
                     exceptions::peer_verification_failed);
}

BOOST_AUTO_TEST_CASE(tls_muxer_requires_authenticated_identity_and_reports_legacy_alpn) {
   const auto identity = muxer_identity("inline-tls-proof");
   const auto other = muxer_identity("inline-tls-other");
   const auto expected = muxer_peer(identity);
   const auto chain = muxer_certificate_chain(identity);
   const auto inline_result = verify_libp2p_tls_handshake(chain, "/yamux/1.0.0", expected);
   BOOST_REQUIRE(inline_result.muxer.has_value());
   BOOST_TEST(inline_result.muxer->value == "/yamux/1.0.0");
   BOOST_TEST(!verify_libp2p_tls_handshake(chain, "libp2p", expected).muxer.has_value());
   BOOST_TEST(!verify_libp2p_tls_handshake(chain, "", expected).muxer.has_value());
   BOOST_CHECK_THROW(static_cast<void>(verify_libp2p_tls_handshake(chain, "/unknown/1.0.0", expected)),
                     exceptions::unsupported_protocol);
   BOOST_CHECK_THROW(static_cast<void>(verify_libp2p_tls_handshake(chain, "/yamux/1.0.0", muxer_peer(other))),
                     exceptions::peer_verification_failed);
   BOOST_CHECK_THROW(static_cast<void>(verify_libp2p_tls_handshake(chain, "/unknown/1.0.0", muxer_peer(other))),
                     exceptions::peer_verification_failed);
   BOOST_CHECK_THROW(static_cast<void>(verify_libp2p_tls_handshake({}, "/yamux/1.0.0", expected)),
                     exceptions::peer_verification_failed);

   const auto client = make_libp2p_tls_client_options(identity);
   const auto server = make_libp2p_tls_server_options(identity);
   BOOST_REQUIRE_EQUAL(client.alpn_protocols.size(), 2U);
   BOOST_TEST(client.alpn_protocols.front() == "/yamux/1.0.0");
   BOOST_TEST(client.alpn_protocols.back() == "libp2p");
   BOOST_CHECK_EQUAL_COLLECTIONS(client.alpn_protocols.begin(), client.alpn_protocols.end(),
                                 server.alpn_protocols.begin(), server.alpn_protocols.end());
}

BOOST_AUTO_TEST_CASE(tls_direct_connections_report_actual_inline_negotiation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = muxer_identity("inline-tls-server");
   const auto client_identity = muxer_identity("inline-tls-client");
   const auto options = node::options{};
   auto server = direct::registry{runtime, options, server_identity, resource_manager{}};
   auto client = direct::registry{runtime, options, client_identity, resource_manager{}};
   const auto local = server.listen(muxer_endpoint());
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(local), boost::asio::use_future);
   auto outbound = forge::asio::blocking::run(
       runtime, client.async_connect(local, node::connect_options{.expected_peer = muxer_peer(server_identity),
                                                                  .timeout = std::chrono::seconds{5}}));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = accepted.get();
   BOOST_TEST(outbound.muxer.value == "/yamux/1.0.0");
   BOOST_TEST(inbound.muxer.value == outbound.muxer.value);
   BOOST_TEST(outbound.used_early_muxer_negotiation);
   BOOST_TEST(inbound.used_early_muxer_negotiation);
   BOOST_REQUIRE(outbound.role.has_value());
   BOOST_REQUIRE(inbound.role.has_value());
   BOOST_TEST(static_cast<int>(*outbound.role) == static_cast<int>(upgrade_role::initiator));
   BOOST_TEST(static_cast<int>(*inbound.role) == static_cast<int>(upgrade_role::responder));
   BOOST_TEST(static_cast<int>(outbound.authentication) == static_cast<int>(peer_authentication::libp2p_tls));
   BOOST_TEST(static_cast<int>(inbound.authentication) == static_cast<int>(peer_authentication::libp2p_tls));
   BOOST_TEST(outbound.peer.to_string() == muxer_peer(server_identity).to_string());
   BOOST_TEST(inbound.peer.to_string() == muxer_peer(client_identity).to_string());
   check_muxer_payload(runtime, outbound.session, inbound.session);
   client.stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(noise_direct_connections_report_actual_inline_negotiation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = muxer_identity("inline-noise-server");
   const auto client_identity = muxer_identity("inline-noise-client");
   const auto options = node::options{};
   auto listener = forge::net::tcp::listener{runtime.context().get_executor(), muxer_endpoint().transport};
   auto client = direct::registry{runtime, options, client_identity, resource_manager{}};
   auto upgraded = boost::asio::co_spawn(
       runtime.context(), accept_noise_peer(listener, options, server_identity, muxer_peer(client_identity)),
       boost::asio::use_future);
   const auto connect_options =
       node::connect_options{.expected_peer = muxer_peer(server_identity), .timeout = std::chrono::seconds{5}};
   auto outbound = forge::asio::blocking::run(
       runtime, client.async_connect(endpoint{.transport = listener.local_endpoint()}, connect_options));
   BOOST_REQUIRE(upgraded.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = upgraded.get();
   BOOST_TEST(outbound.muxer.value == "/yamux/1.0.0");
   BOOST_TEST(inbound.muxer.value == outbound.muxer.value);
   BOOST_TEST(outbound.used_early_muxer_negotiation);
   BOOST_TEST(inbound.used_early_muxer_negotiation);
   BOOST_REQUIRE(outbound.role.has_value());
   BOOST_REQUIRE(inbound.role.has_value());
   BOOST_TEST(static_cast<int>(*outbound.role) == static_cast<int>(upgrade_role::initiator));
   BOOST_TEST(static_cast<int>(*inbound.role) == static_cast<int>(upgrade_role::responder));
   BOOST_TEST(static_cast<int>(outbound.authentication) == static_cast<int>(peer_authentication::noise));
   BOOST_TEST(static_cast<int>(inbound.authentication) == static_cast<int>(peer_authentication::noise));
   BOOST_TEST(outbound.peer.to_string() == muxer_peer(server_identity).to_string());
   BOOST_TEST(inbound.peer.to_string() == muxer_peer(client_identity).to_string());
   auto inbound_session = std::move(*inbound.session).as_transport();
   check_muxer_payload(runtime, outbound.session, inbound_session);
   client.stop();
   listener.close();
}

BOOST_AUTO_TEST_CASE(coordinated_tcp_role_is_independent_of_socket_connect_or_accept) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto accepting_identity = muxer_identity("coordinated-accepting-client");
   const auto connecting_identity = muxer_identity("coordinated-connecting-server");
   const auto options = node::options{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, upgrade_tcp({}, options, connecting_identity, std::nullopt,
                                                                     static_cast<upgrade_role>(255))),
                     exceptions::invalid_options);
   auto listener = forge::net::tcp::listener{runtime.context().get_executor(), muxer_endpoint().transport};
   auto connector = forge::net::tcp::connector{runtime.context().get_executor()};
   auto accepted =
       boost::asio::co_spawn(runtime.context(), listener.async_accept_connection(), boost::asio::use_future);
   auto connecting = forge::asio::blocking::run(runtime, connector.async_connect_connection(listener.local_endpoint()));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto accepting = accepted.get();
   auto initiated = boost::asio::co_spawn(runtime.context(),
                                          upgrade_tcp(std::move(accepting), options, accepting_identity,
                                                      muxer_peer(connecting_identity), upgrade_role::initiator),
                                          boost::asio::use_future);
   auto responder =
       forge::asio::blocking::run(runtime, upgrade_tcp(std::move(connecting), options, connecting_identity,
                                                       muxer_peer(accepting_identity), upgrade_role::responder));
   BOOST_REQUIRE(initiated.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto initiator = initiated.get();
   BOOST_REQUIRE(initiator.role.has_value());
   BOOST_REQUIRE(responder.role.has_value());
   BOOST_TEST(static_cast<int>(*initiator.role) == static_cast<int>(upgrade_role::initiator));
   BOOST_TEST(static_cast<int>(*responder.role) == static_cast<int>(upgrade_role::responder));
   BOOST_TEST(initiator.muxer.value == "/yamux/1.0.0");
   BOOST_TEST(responder.muxer.value == initiator.muxer.value);
   BOOST_TEST(initiator.used_early_muxer_negotiation);
   BOOST_TEST(responder.used_early_muxer_negotiation);
   BOOST_TEST(initiator.peer.to_string() == muxer_peer(connecting_identity).to_string());
   BOOST_TEST(responder.peer.to_string() == muxer_peer(accepting_identity).to_string());
   auto initiator_session = std::move(*initiator.session).as_transport();
   auto responder_session = std::move(*responder.session).as_transport();
   check_muxer_payload(runtime, initiator_session, responder_session);
   listener.close();
}

BOOST_AUTO_TEST_CASE(tls_old_peer_fallback_reports_multistream_in_both_directions) {
   for (const auto forge_outbound : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      const auto server_identity = muxer_identity("fallback-tls-server");
      const auto client_identity = muxer_identity("fallback-tls-client");
      const auto options = node::options{};
      auto listener = forge::net::tcp::listener{runtime.context().get_executor(), muxer_endpoint().transport};
      auto connector = forge::net::tcp::connector{runtime.context().get_executor()};
      auto accepted =
          boost::asio::co_spawn(runtime.context(), listener.async_accept_connection(), boost::asio::use_future);
      auto tcp = forge::asio::blocking::run(runtime, connector.async_connect_connection(listener.local_endpoint()));
      BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      auto incoming = accepted.get();
      auto upgraded =
          boost::asio::co_spawn(runtime.context(),
                                forge_outbound ? legacy_tls_peer(std::move(incoming), server_identity, false)
                                               : upgrade_inbound_tcp(std::move(incoming), options, server_identity,
                                                                     muxer_peer(client_identity)),
                                boost::asio::use_future);
      auto outbound = forge::asio::blocking::run(
          runtime, forge_outbound
                       ? upgrade_outbound_tcp(std::move(tcp), options, client_identity, muxer_peer(server_identity))
                       : legacy_tls_peer(std::move(tcp), client_identity, true));
      BOOST_REQUIRE(upgraded.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
      auto inbound = upgraded.get();
      BOOST_TEST(outbound.muxer.value == "/yamux/1.0.0");
      BOOST_TEST(inbound.muxer.value == outbound.muxer.value);
      BOOST_TEST(!outbound.used_early_muxer_negotiation);
      BOOST_TEST(!inbound.used_early_muxer_negotiation);
      auto outbound_session = std::move(*outbound.session).as_transport();
      auto inbound_session = std::move(*inbound.session).as_transport();
      check_muxer_payload(runtime, outbound_session, inbound_session);
      listener.close();
   }
}

BOOST_AUTO_TEST_CASE(coordinated_tcp_late_inbound_keeps_role_after_outgoing_refused) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto accepting_identity = muxer_identity("late-accepting-client");
   const auto connecting_identity = muxer_identity("late-connecting-server");
   const auto options = node::options{};
   auto accepting = direct::registry{runtime, options, accepting_identity, resource_manager{}};
   const auto local = accepting.listen(muxer_endpoint());
   auto batch = std::make_shared<cancellation_latch>();

   // Own the remote TCP port without listening: SYN is refused, with no
   // established socket/TIME_WAIT or sleep before the later reverse dial.
   auto reserved = boost::asio::ip::tcp::socket{runtime.context()};
   reserved.open(boost::asio::ip::tcp::v4());
   reserved.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
   auto remote = muxer_endpoint();
   remote.transport.port = reserved.local_endpoint().port();
   auto failed = boost::asio::co_spawn(runtime.context(),
                                       accepting.async_connect_coordinated(remote, muxer_peer(connecting_identity),
                                                                           upgrade_role::initiator,
                                                                           std::chrono::seconds{15}, batch),
                                       boost::asio::use_future);
   BOOST_REQUIRE(failed.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   // FORGE_THROW_CODE preserves the typed category/code, but throws a runtime
   // coded exception rather than the compile-time peer_not_found alias.
   BOOST_CHECK_EXCEPTION(
       static_cast<void>(failed.get()), forge::exceptions::base,
       [](const forge::exceptions::base& error) { return exceptions::is(error, exceptions::code::peer_not_found); });
   BOOST_TEST(!batch->stop_requested());
   reserved.close();

   auto source =
       forge::net::tcp::listener{runtime.context().get_executor(), remote.transport,
                                 forge::net::transport::listen_options{}, forge::net::tcp::options{.reuse_port = true}};
   auto connector = source.make_coordinated_connector(source.local_endpoint());
   auto accepted = boost::asio::co_spawn(runtime.context(), accepting.async_accept(local), boost::asio::use_future);
   auto native = forge::asio::blocking::run(runtime, connector.async_connect_connection(local.transport));
   BOOST_TEST(native.local_endpoint().port == remote.transport.port);
   auto responder = forge::asio::blocking::run(
       runtime, upgrade_tcp(std::move(native), options, connecting_identity, muxer_peer(accepting_identity),
                            upgrade_role::responder,
                            tcp_upgrade_deadline{.context = &runtime.context(), .timeout = std::chrono::seconds{5}}));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto initiator = accepted.get();
   BOOST_REQUIRE(initiator.role.has_value());
   BOOST_REQUIRE(responder.role.has_value());
   BOOST_TEST(static_cast<int>(*initiator.role) == static_cast<int>(upgrade_role::initiator));
   BOOST_TEST(static_cast<int>(*responder.role) == static_cast<int>(upgrade_role::responder));
   BOOST_TEST(initiator.peer.to_string() == muxer_peer(connecting_identity).to_string());
   BOOST_TEST(responder.peer.to_string() == muxer_peer(accepting_identity).to_string());
   BOOST_REQUIRE(initiator.local_endpoint.has_value());
   BOOST_REQUIRE(initiator.remote_endpoint.has_value());
   BOOST_TEST(initiator.local_endpoint->transport.port == local.transport.port);
   BOOST_TEST(initiator.remote_endpoint->transport.port == remote.transport.port);
   BOOST_TEST(initiator.muxer.value == "/yamux/1.0.0");
   BOOST_TEST(initiator.muxer.value == responder.muxer.value);
   auto responding = direct::connection{.session = std::move(*responder.session).as_transport()};
   check_muxer_payload(runtime, initiator.session, responding.session);
   batch->request_stop();
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(initiator));
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(responding));
   forge::asio::blocking::run(runtime, connector.async_stop());
   forge::asio::blocking::run(runtime, source.async_close());
   stop_registry(runtime, accepting);
}

BOOST_AUTO_TEST_CASE(coordinated_quic_second_candidate_probes_and_receives_its_authenticated_inbound) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto accepting_identity = muxer_identity("quic-candidate-accepting");
   const auto connecting_identity = muxer_identity("quic-candidate-connecting");
   const auto accepting_tls = make_libp2p_tls_material(accepting_identity);
   const auto connecting_tls = make_libp2p_tls_material(connecting_identity);
   const auto accepting_options = node::options{.certificate_pem = accepting_tls.certificate_pem,
                                                .private_key_pem = accepting_tls.private_key_pem};
   const auto connecting_options = node::options{.certificate_pem = connecting_tls.certificate_pem,
                                                 .private_key_pem = connecting_tls.private_key_pem};
   auto accepting = direct::registry{runtime, accepting_options, accepting_identity, resource_manager{}};
   auto connecting = direct::registry{runtime, connecting_options, connecting_identity, resource_manager{}};
   const auto local = accepting.listen(parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1"));
   auto batch = std::make_shared<cancellation_latch>();
   auto accepted = boost::asio::co_spawn(runtime.context(), accepting.async_accept(local), boost::asio::use_future);
   auto first_socket = boost::asio::ip::udp::socket{runtime.context(), {boost::asio::ip::make_address("127.0.0.1"), 0}};
   auto second_socket =
       boost::asio::ip::udp::socket{runtime.context(), {boost::asio::ip::make_address("127.0.0.1"), 0}};
   auto first_remote = local;
   first_remote.transport.port = first_socket.local_endpoint().port();
   auto second_remote = local;
   second_remote.transport.port = second_socket.local_endpoint().port();
   auto first_payload = std::array<std::uint8_t, 256>{};
   auto second_payload = std::array<std::uint8_t, 256>{};
   auto first_sender = boost::asio::ip::udp::endpoint{};
   auto second_sender = boost::asio::ip::udp::endpoint{};
   auto first_probe =
       first_socket.async_receive_from(boost::asio::buffer(first_payload), first_sender, boost::asio::use_future);
   auto second_probe =
       second_socket.async_receive_from(boost::asio::buffer(second_payload), second_sender, boost::asio::use_future);
   auto first_admissions = std::atomic_size_t{0};
   auto second_admissions = std::atomic_size_t{0};
   auto first = boost::asio::co_spawn(
       runtime.context(),
       accepting.async_connect_coordinated(first_remote, muxer_peer(connecting_identity), upgrade_role::responder,
                                           std::chrono::seconds{15}, batch,
                                           [&first_admissions](const peer_id&) { ++first_admissions; }),
       boost::asio::use_future);
   BOOST_REQUIRE(first_probe.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_TEST(first_probe.get() > 0U);
   BOOST_TEST(first_sender.port() == local.transport.port);

   auto second = boost::asio::co_spawn(
       runtime.context(),
       accepting.async_connect_coordinated(second_remote, muxer_peer(connecting_identity), upgrade_role::responder,
                                           std::chrono::seconds{15}, batch,
                                           [&second_admissions](const peer_id&) { ++second_admissions; }),
       boost::asio::use_future);
   // Observing the actual second probe is the registration barrier; no
   // source-only assertion, private map inspection, or timing sleep.
   BOOST_REQUIRE(second_probe.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   BOOST_TEST(second_probe.get() > 0U);
   BOOST_TEST(second_sender.port() == local.transport.port);
   second_socket.close();
   const auto source = connecting.listen(second_remote);
   auto outbound = forge::asio::blocking::run(
       runtime, connecting.async_connect_coordinated(local, muxer_peer(accepting_identity), upgrade_role::initiator,
                                                     std::chrono::seconds{5}, {}, {}, source));
   BOOST_REQUIRE(second.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = second.get();
   BOOST_CHECK(first.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   BOOST_TEST(first_admissions.load() == 0U);
   BOOST_TEST(second_admissions.load() == 1U);
   BOOST_TEST(inbound.peer.to_string() == muxer_peer(connecting_identity).to_string());
   BOOST_TEST(outbound.peer.to_string() == muxer_peer(accepting_identity).to_string());
   BOOST_REQUIRE(inbound.remote_endpoint.has_value());
   BOOST_REQUIRE(outbound.local_endpoint.has_value());
   BOOST_TEST(inbound.remote_endpoint->transport.port == source.transport.port);
   BOOST_TEST(outbound.local_endpoint->transport.port == source.transport.port);
   BOOST_REQUIRE(inbound.role.has_value());
   BOOST_TEST(static_cast<int>(*inbound.role) == static_cast<int>(upgrade_role::responder));
   BOOST_TEST(static_cast<int>(inbound.authentication) == static_cast<int>(peer_authentication::quic_tls));
   BOOST_REQUIRE(inbound.admission.has_value());
   BOOST_TEST(inbound.admission->active());
   batch->request_stop();
   BOOST_REQUIRE(first.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(first.get()), exceptions::canceled);
   check_muxer_payload(runtime, outbound.session, inbound.session);
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(outbound));
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(inbound));
   stop_registry(runtime, connecting);
   stop_registry(runtime, accepting);
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(accepted.get()), forge::exceptions::base);
}

BOOST_AUTO_TEST_CASE(ordinary_quic_identify_observation_belongs_to_concrete_or_wildcard_listener) {
   for (const auto host : {"127.0.0.1", "0.0.0.0"}) {
      BOOST_TEST_CONTEXT("listener=" << host) {
         check_ordinary_observed_source(true, false, host);
      }
   }
}

BOOST_AUTO_TEST_CASE(ordinary_tcp_identify_observation_belongs_to_listener_including_private_profile) {
   for (const auto private_profile : {false, true}) {
      for (const auto host : {"127.0.0.1", "0.0.0.0"}) {
         BOOST_TEST_CONTEXT("private=" << private_profile << " listener=" << host) {
            check_ordinary_observed_source(false, private_profile, host);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(ordinary_quic_borrowed_listener_has_no_per_connection_descriptor_and_still_accepts) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_identity = muxer_identity("ordinary-quic-fd-server");
   const auto client_identity = muxer_identity("ordinary-quic-fd-client");
   const auto server_tls = make_libp2p_tls_material(server_identity);
   const auto client_tls = make_libp2p_tls_material(client_identity);
   const auto server_options =
       node::options{.certificate_pem = server_tls.certificate_pem, .private_key_pem = server_tls.private_key_pem};
   const auto client_options =
       node::options{.certificate_pem = client_tls.certificate_pem, .private_key_pem = client_tls.private_key_pem};
   auto resources = resource_manager{};
   auto server = direct::registry{runtime, server_options, server_identity, resource_manager{}};
   auto client = direct::registry{runtime, client_options, client_identity, resources};
   const auto remote = server.listen(parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1"));
   const auto source = client.listen(parse_endpoint("/ip4/0.0.0.0/udp/0/quic-v1"));
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(remote), boost::asio::use_future);
   auto counts = std::vector<std::size_t>{};
   auto outbound = forge::asio::blocking::run(
       runtime,
       client.async_connect(
           remote,
           node::connect_options{.expected_peer = muxer_peer(server_identity), .timeout = std::chrono::seconds{5}}, {},
           {}, {}, {}, [&counts](std::size_t descriptors) { counts.push_back(descriptors); }));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = accepted.get();
   BOOST_REQUIRE_EQUAL(counts.size(), 1U);
   BOOST_TEST(counts.front() == 0U);
   BOOST_TEST(resources.current().system.file_descriptors == 1U);
   BOOST_REQUIRE(outbound.local_endpoint.has_value());
   BOOST_REQUIRE(inbound.remote_endpoint.has_value());
   BOOST_TEST(outbound.local_endpoint->transport.port == source.transport.port);
   BOOST_TEST(inbound.remote_endpoint->to_string() == outbound.local_endpoint->to_string());
   BOOST_REQUIRE(outbound.role.has_value());
   BOOST_TEST(static_cast<int>(*outbound.role) == static_cast<int>(upgrade_role::initiator));
   check_muxer_payload(runtime, outbound.session, inbound.session);
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(outbound));
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(inbound));

   // Closing a dial must not close its borrowed UDP owner: accept a reverse
   // ordinary connection through the same wildcard listener's actual port.
   auto dialable_source = source;
   dialable_source.transport.host = "127.0.0.1";
   auto reverse_accepted =
       boost::asio::co_spawn(runtime.context(), client.async_accept(source), boost::asio::use_future);
   auto reverse_outbound = forge::asio::blocking::run(
       runtime,
       server.async_connect(dialable_source, node::connect_options{.expected_peer = muxer_peer(client_identity),
                                                                   .timeout = std::chrono::seconds{5}}));
   BOOST_REQUIRE(reverse_accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto reverse_inbound = reverse_accepted.get();
   BOOST_TEST(reverse_inbound.peer.to_string() == muxer_peer(server_identity).to_string());
   check_muxer_payload(runtime, reverse_outbound.session, reverse_inbound.session);
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(reverse_outbound));
   forge::asio::blocking::run(runtime, direct::async_discard_unpublished(reverse_inbound));
   stop_registry(runtime, client);
   stop_registry(runtime, server);
   BOOST_TEST(resources.current().system.file_descriptors == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
