module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <span>
#include <stdexcept>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;

#include "../../libraries/net/p2p/details/direct_transport.hxx"
#include "../../libraries/net/p2p/details/libp2p_identity_material.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] endpoint tcp_endpoint(std::uint16_t port) {
   return endpoint{
       .transport = {
           .host_type = endpoint::host_kind::ip4,
           .protocol = endpoint::protocol_kind::tcp,
           .host = "127.0.0.1",
           .port = port,
       },
   };
}

[[nodiscard]] endpoint quic_endpoint(std::uint16_t port) {
   return endpoint{
       .transport = {
           .host_type = endpoint::host_kind::ip4,
           .protocol = endpoint::protocol_kind::quic_v1,
           .host = "127.0.0.1",
           .port = port,
       },
   };
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_direct_tcp_progress_is_exactly_once_and_failure_neutral) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_fixture = forge::tests::p2p::make_identity_fixture("direct-progress-tcp-server");
   const auto client_fixture = forge::tests::p2p::make_identity_fixture("direct-progress-tcp-client");
   auto server_options = node::options{
       .private_key_pem = server_fixture.private_key_pem,
       .allow_insecure_test_mode = true,
   };
   auto client_options = node::options{
       .private_key_pem = client_fixture.private_key_pem,
       .allow_insecure_test_mode = true,
   };
   auto server_identity = make_libp2p_identity_material(server_options);
   auto client_identity = make_libp2p_identity_material(client_options);
   auto server_resources = resource_manager{};
   auto client_resources = resource_manager{};
   auto server = direct::registry{runtime, server_options, server_identity, server_resources};
   auto client = direct::registry{runtime, client_options, client_identity, client_resources};
   const auto server_endpoint = server.listen(tcp_endpoint(0));
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(server_endpoint), boost::asio::use_future);
   auto progress = std::atomic_size_t{0};
   auto outbound = forge::asio::blocking::run(
       runtime,
       client.async_connect(server_endpoint, node::connect_options{.timeout = std::chrono::seconds{5}}, {}, {}, {},
                            [&progress] {
                               progress.fetch_add(1U, std::memory_order_relaxed);
                               throw std::runtime_error{"scheduler progress callback failure"};
                            }));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = accepted.get();

   BOOST_TEST(progress.load(std::memory_order_relaxed) == 1U);
   outbound.session.request_cancel();
   inbound.session.request_cancel();
   client.stop();
   server.stop();
}

BOOST_AUTO_TEST_CASE(p2p_direct_quic_does_not_emit_tcp_transport_progress) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto server_fixture = forge::tests::p2p::make_identity_fixture("direct-progress-quic-server");
   const auto client_fixture = forge::tests::p2p::make_identity_fixture("direct-progress-quic-client");
   auto server_options = node::options{
       .certificate_pem = server_fixture.certificate_pem,
       .private_key_pem = server_fixture.private_key_pem,
       .allow_insecure_test_mode = true,
   };
   auto client_options = node::options{
       .certificate_pem = client_fixture.certificate_pem,
       .private_key_pem = client_fixture.private_key_pem,
       .allow_insecure_test_mode = true,
   };
   auto server_identity = make_libp2p_identity_material(server_options);
   auto client_identity = make_libp2p_identity_material(client_options);
   auto server_resources = resource_manager{};
   auto client_resources = resource_manager{};
   auto server = direct::registry{runtime, server_options, server_identity, server_resources};
   auto client = direct::registry{runtime, client_options, client_identity, client_resources};
   const auto server_endpoint = server.listen(quic_endpoint(0));
   auto accepted = boost::asio::co_spawn(runtime.context(), server.async_accept(server_endpoint), boost::asio::use_future);
   auto progress = std::atomic_size_t{0};

   auto outbound = forge::asio::blocking::run(
       runtime,
       client.async_connect(server_endpoint, node::connect_options{.timeout = std::chrono::seconds{5}}, {}, {}, {},
                            [&progress] {
                               progress.fetch_add(1U, std::memory_order_relaxed);
                               throw std::runtime_error{"QUIC must not emit TCP progress"};
                            }));
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   auto inbound = accepted.get();

   BOOST_TEST(progress.load(std::memory_order_relaxed) == 0U);
   outbound.session.request_cancel();
   inbound.session.request_cancel();
   client.stop();
   server.stop();
}

} // namespace forge::net::p2p
