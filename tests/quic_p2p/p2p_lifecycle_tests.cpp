module;

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.multiformats.varint;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;

#include "../../libraries/net/p2p/details/lifecycle_tracker.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] node::options make_lifecycle_node_options(std::string name) {
   auto identity = forge::tests::p2p::make_identity_fixture(std::move(name));
   return node::options{
       .certificate_pem = std::move(identity.certificate_pem),
       .private_key_pem = std::move(identity.private_key_pem),
       .peer_state = {.persistence = peer_store::make_memory_persistence()},
   };
}

[[nodiscard]] endpoint unavailable_bootstrap(std::string name) {
   const auto identity = forge::tests::p2p::make_identity_fixture(std::move(name));
   const auto peer = make_peer_id_from_certificate_pem(identity.certificate_pem);
   return parse_endpoint("/ip4/127.0.0.1/tcp/1/p2p/" + peer.to_string());
}

template <typename Predicate> [[nodiscard]] bool eventually(Predicate&& predicate, std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) {
         return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   return true;
}

[[nodiscard]] bool supports(const peer_store::record& record, const protocol_id& protocol) {
   return std::ranges::find(record.protocols, protocol) != record.protocols.end();
}

[[nodiscard]] std::vector<std::uint8_t> length_delimited(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

} // namespace

BOOST_AUTO_TEST_CASE(p2p_lifecycle_stop_is_latched_before_task_cancellation_handler_installation) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto tracker = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(tracker.begin_start());

   auto operation = tracker.track();
   BOOST_REQUIRE(operation.active());
   const auto stop_source = operation.stop_source();

   // Request stop before any cancellation handler is installed. The durable
   // latch is the task-entry guard for this exact registration race.
   tracker.request_stop();
   BOOST_REQUIRE(stop_source);
   BOOST_TEST(stop_source->stop_requested());
   operation.release();
   forge::asio::blocking::run(runtime, tracker.wait());
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_stop_latch_is_sticky_and_waits_for_operation_release) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto tracker = detail::lifecycle_tracker{runtime.context().get_executor()};
   BOOST_REQUIRE(tracker.begin_start());

   auto operation = tracker.track();
   BOOST_REQUIRE(operation.active());
   const auto stop_source = operation.stop_source();

   tracker.request_stop();
   BOOST_REQUIRE(stop_source);
   BOOST_TEST(stop_source->stop_requested());

   auto waiting = boost::asio::co_spawn(runtime.context(), tracker.wait(), boost::asio::use_future);
   BOOST_CHECK(waiting.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   operation.release();
   BOOST_CHECK(waiting.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   waiting.get();
}

BOOST_AUTO_TEST_CASE(p2p_node_optional_bootstrap_reports_degraded_and_keeps_running) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = make_lifecycle_node_options("lifecycle-optional");
   options.lifecycle.bootstrap = {{.address = unavailable_bootstrap("lifecycle-optional-bootstrap").to_multiaddr()}};
   options.lifecycle.startup_budget = std::chrono::milliseconds{100};
   options.lifecycle.connect_timeout = std::chrono::milliseconds{25};
   auto value = node{runtime, std::move(options)};

   const auto status = forge::asio::blocking::run(runtime, value.async_start());
   BOOST_TEST(static_cast<int>(status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   BOOST_TEST(status.configured_bootstrap == 1U);
   BOOST_TEST(status.connected_bootstrap == 0U);
   BOOST_TEST(status.degraded);
   BOOST_TEST(!status.last_bootstrap_failure.empty());
   BOOST_TEST(value.lifecycle_state().last_bootstrap_failure == status.last_bootstrap_failure);

   forge::asio::blocking::run(runtime, value.async_stop());
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
}

BOOST_AUTO_TEST_CASE(p2p_node_strict_bootstrap_retries_until_shared_startup_budget) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto options = make_lifecycle_node_options("lifecycle-strict-failure");
   options.lifecycle.bootstrap = {{.address = unavailable_bootstrap("lifecycle-strict-unavailable").to_multiaddr()}};
   options.lifecycle.requirement = bootstrap_requirement::require_connection;
   options.lifecycle.startup_budget = std::chrono::milliseconds{180};
   options.lifecycle.connect_timeout = std::chrono::milliseconds{25};
   options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{20};
   options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{40};
   options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto value = node{runtime, std::move(options)};

   const auto started = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, value.async_start()), exceptions::timeout);
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed >= std::chrono::milliseconds{140});
   BOOST_TEST(elapsed < std::chrono::seconds{2});
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));
}

BOOST_AUTO_TEST_CASE(p2p_node_stop_cancels_active_bootstrap_dial) {
   namespace asio = boost::asio;
   using tcp = asio::ip::tcp;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto acceptor = std::make_shared<tcp::acceptor>(runtime.context(), tcp::endpoint{tcp::v4(), 0});
   acceptor->listen();
   auto socket = std::make_shared<tcp::socket>(runtime.context());
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   asio::co_spawn(
       runtime.context(),
       [acceptor, socket, accepted]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await acceptor->async_accept(*socket, asio::redirect_error(asio::use_awaitable, error));
          if (!error) {
             accepted->set_value();
          }
       },
       asio::detached);

   const auto bootstrap_identity = forge::tests::p2p::make_identity_fixture("lifecycle-active-dial-peer");
   const auto bootstrap_peer = make_peer_id_from_certificate_pem(bootstrap_identity.certificate_pem);
   auto options = make_lifecycle_node_options("lifecycle-active-dial-node");
   options.lifecycle.bootstrap = {
       {.address = parse_endpoint("/ip4/127.0.0.1/tcp/" + std::to_string(acceptor->local_endpoint().port()) + "/p2p/" +
                                  bootstrap_peer.to_string()).to_multiaddr()}};
   options.lifecycle.requirement = bootstrap_requirement::require_connection;
   options.lifecycle.startup_budget = std::chrono::seconds{5};
   options.lifecycle.connect_timeout = std::chrono::seconds{5};
   auto value = node{runtime, std::move(options)};

   auto startup = asio::co_spawn(runtime.context(), value.async_start(), asio::use_future);
   BOOST_REQUIRE(accepted_future.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   const auto started = std::chrono::steady_clock::now();
   forge::asio::blocking::run(runtime, value.async_stop());
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   BOOST_TEST(elapsed.count() < 750);
   BOOST_REQUIRE(startup.wait_for(std::chrono::milliseconds{100}) == std::future_status::ready);
   auto startup_failed = false;
   try {
      static_cast<void>(startup.get());
   } catch (const std::exception&) {
      startup_failed = true;
   }
   BOOST_TEST(startup_failed);
   BOOST_TEST(static_cast<int>(value.lifecycle_state().phase) == static_cast<int>(lifecycle_phase::stopped));

   auto ignored = boost::system::error_code{};
   socket->close(ignored);
   acceptor->close(ignored);
}

BOOST_AUTO_TEST_CASE(p2p_node_lifecycle_bootstrap_connects_and_dynamic_removal_unprotects_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   auto server_options = make_lifecycle_node_options("lifecycle-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   const auto server_status = forge::asio::blocking::run(runtime, server.async_start());
   BOOST_TEST(static_cast<int>(server_status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   BOOST_REQUIRE(address->peer.has_value());

   auto client_options = make_lifecycle_node_options("lifecycle-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.connect_timeout = std::chrono::seconds{2};
   auto client = node{runtime, std::move(client_options)};

   const auto client_status = forge::asio::blocking::run(runtime, client.async_start());
   BOOST_TEST(client_status.connected_bootstrap == 1U);
   BOOST_TEST(!client_status.degraded);
   BOOST_TEST(client.is_peer_protected(server.local_peer()));

   forge::asio::blocking::run(runtime, client.async_set_bootstrap({}));
   BOOST_TEST(client.lifecycle_state().configured_bootstrap == 0U);
   BOOST_TEST(!client.is_peer_protected(server.local_peer()));

   forge::asio::blocking::run(runtime, client.async_stop());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, client.async_set_bootstrap({})), exceptions::closed);
   BOOST_TEST(client.lifecycle_state().configured_bootstrap == 0U);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_learns_authenticated_peer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server_options = make_lifecycle_node_options("lifecycle-peerless-bootstrap-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   address->peer.reset();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-bootstrap-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   auto client = node{runtime, std::move(client_options)};
   const auto status = forge::asio::blocking::run(runtime, client.async_start());

   BOOST_TEST(status.connected_bootstrap == 1U);
   BOOST_TEST(client.is_peer_protected(server.local_peer()));
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_replaces_authenticated_peer_protection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first_options = make_lifecycle_node_options("lifecycle-peerless-rotation-first");
   first_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto first = node{runtime, std::move(first_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, first.async_start()));
   auto address = first.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   const auto first_peer = first.local_peer();
   address->peer.reset();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-rotation-client");
   client_options.lifecycle.bootstrap = {{.address = address->to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.maintenance_interval = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{50};
   client_options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, first.async_stop());
   auto second_options = make_lifecycle_node_options("lifecycle-peerless-rotation-second");
   second_options.lifecycle.listen = {*address};
   auto second = node{runtime, std::move(second_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, second.async_start()));
   const auto second_peer = second.local_peer();
   BOOST_TEST(static_cast<bool>(first_peer != second_peer));

   BOOST_REQUIRE(
       eventually([&] { return client.is_peer_protected(second_peer) && !client.is_peer_protected(first_peer); },
                  std::chrono::seconds{3}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_peerless_bootstrap_alias_removal_preserves_shared_peer_protection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first_options = make_lifecycle_node_options("lifecycle-peerless-alias-first");
   first_options.lifecycle.listen = {
       parse_endpoint("/ip4/127.0.0.1/tcp/0"),
       parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1"),
   };
   auto first = node{runtime, std::move(first_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, first.async_start()));
   auto addresses = first.local_endpoints();
   BOOST_REQUIRE_EQUAL(addresses.size(), 2U);
   for (auto& address : addresses) {
      address.peer.reset();
   }
   const auto tcp = std::ranges::find_if(addresses, [](const auto& address) { return address.is_direct_tcp(); });
   const auto quic = std::ranges::find_if(addresses, [](const auto& address) { return address.is_direct_quic(); });
   BOOST_REQUIRE(tcp != addresses.end());
   BOOST_REQUIRE(quic != addresses.end());
   const auto tcp_address = *tcp;
   const auto quic_address = *quic;
   const auto first_peer = first.local_peer();

   auto client_options = make_lifecycle_node_options("lifecycle-peerless-alias-client");
   client_options.lifecycle.bootstrap = {
       {.address = tcp_address.to_multiaddr()},
       {.address = quic_address.to_multiaddr()},
   };
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.maintenance_interval = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_initial_delay = std::chrono::milliseconds{25};
   client_options.lifecycle.bootstrap_retry_max_delay = std::chrono::milliseconds{50};
   client_options.lifecycle.bootstrap_retry_jitter = 0.0;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));

   BOOST_REQUIRE(
       eventually([&] { return client.lifecycle_state().connected_bootstrap == 2U; }, std::chrono::seconds{3}));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, first.async_stop());
   auto second_options = make_lifecycle_node_options("lifecycle-peerless-alias-second");
   second_options.lifecycle.listen = {tcp_address};
   auto second = node{runtime, std::move(second_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, second.async_start()));
   const auto second_peer = second.local_peer();
   BOOST_REQUIRE(first_peer != second_peer);

   BOOST_REQUIRE(eventually([&] { return client.is_peer_protected(second_peer); }, std::chrono::seconds{3}));
   BOOST_TEST(client.is_peer_protected(first_peer));

   forge::asio::blocking::run(runtime, client.async_set_bootstrap({bootstrap_peer{.address = tcp_address.to_multiaddr()}}));
   BOOST_REQUIRE(
       eventually([&] { return !client.is_peer_protected(first_peer) && client.is_peer_protected(second_peer); },
                  std::chrono::seconds{3}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_strict_bootstrap_cancellation_does_not_report_loser_failure) {
   namespace asio = boost::asio;
   using tcp = asio::ip::tcp;

   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto stalled_acceptor = std::make_shared<tcp::acceptor>(runtime.context(), tcp::endpoint{tcp::v4(), 0});
   stalled_acceptor->listen();
   auto stalled_socket = std::make_shared<tcp::socket>(runtime.context());
   asio::co_spawn(
       runtime.context(),
       [stalled_acceptor, stalled_socket]() -> asio::awaitable<void> {
          auto error = boost::system::error_code{};
          co_await stalled_acceptor->async_accept(*stalled_socket, asio::redirect_error(asio::use_awaitable, error));
       },
       asio::detached);

   auto server_options = make_lifecycle_node_options("lifecycle-bootstrap-race-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto available = server.local_endpoint();
   BOOST_REQUIRE(available.has_value());

   const auto stalled_identity = forge::tests::p2p::make_identity_fixture("lifecycle-bootstrap-race-stalled");
   const auto stalled_peer = make_peer_id_from_certificate_pem(stalled_identity.certificate_pem);
   const auto stalled =
       parse_endpoint("/ip4/127.0.0.1/tcp/" + std::to_string(stalled_acceptor->local_endpoint().port()) + "/p2p/" +
                      stalled_peer.to_string());

   auto client_options = make_lifecycle_node_options("lifecycle-bootstrap-race-client");
   client_options.lifecycle.bootstrap = {{.address = available->to_multiaddr()}, {.address = stalled.to_multiaddr()}};
   client_options.lifecycle.requirement = bootstrap_requirement::require_connection;
   client_options.lifecycle.startup_budget = std::chrono::seconds{3};
   client_options.lifecycle.connect_timeout = std::chrono::seconds{2};
   client_options.lifecycle.max_parallel_bootstrap = 2;
   auto client = node{runtime, std::move(client_options)};

   const auto status = forge::asio::blocking::run(runtime, client.async_start());
   BOOST_TEST(status.connected_bootstrap == 1U);
   BOOST_TEST(status.last_bootstrap_failure.empty());
   BOOST_TEST(client.lifecycle_state().last_bootstrap_failure.empty());

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   auto ignored = boost::system::error_code{};
   stalled_socket->close(ignored);
   stalled_acceptor->close(ignored);
}

BOOST_AUTO_TEST_CASE(p2p_node_destructor_requests_maintenance_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   {
      auto value = node{runtime, make_lifecycle_node_options("lifecycle-destructor-stop")};
      static_cast<void>(forge::asio::blocking::run(runtime, value.async_start()));
   }
   std::this_thread::sleep_for(std::chrono::milliseconds{50});

   auto replacement = node{runtime, make_lifecycle_node_options("lifecycle-after-destructor")};
   const auto status = forge::asio::blocking::run(runtime, replacement.async_start());
   BOOST_TEST(static_cast<int>(status.phase) == static_cast<int>(lifecycle_phase::maintenance));
   forge::asio::blocking::run(runtime, replacement.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_connect_waits_for_identify_and_push_replaces_protocol_snapshot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto product_protocol = protocol_id{.value = "/product/lifecycle-identify/1"};

   auto server_options = make_lifecycle_node_options("lifecycle-identify-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};
   server.register_protocol_handler(product_protocol,
                                    [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                       co_await incoming.stream.async_close();
                                    });
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));

   auto client = node{runtime, make_lifecycle_node_options("lifecycle-identify-client")};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::identified));

   auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(supports(*record, product_protocol));

   BOOST_TEST(server.unregister_protocol_handler(product_protocol));
   BOOST_REQUIRE(eventually(
       [&] {
          const auto updated = client.peers().find(server.local_peer());
          return updated && !supports(*updated, product_protocol);
       },
       std::chrono::seconds{2}));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_protocol_registration_rejects_unrepresentable_identify_snapshot) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto invalid_base_options = make_lifecycle_node_options("lifecycle-identify-invalid-base");
   invalid_base_options.identify.max_own_message_size = 1;
   BOOST_CHECK_THROW(node(runtime, std::move(invalid_base_options)), exceptions::invalid_options);
   auto invalid_base_protocols = make_lifecycle_node_options("lifecycle-identify-invalid-base-protocols");
   invalid_base_protocols.identify.max_protocols = 1;
   BOOST_CHECK_THROW(node(runtime, std::move(invalid_base_protocols)), exceptions::invalid_options);

   auto server_options = make_lifecycle_node_options("lifecycle-identify-capacity-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto server = node{runtime, std::move(server_options)};

   BOOST_CHECK_THROW(
       server.register_protocol_handler(protocol_id{.value = "missing-leading-slash"},
                                        [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                           co_await incoming.stream.async_close();
                                        }),
       exceptions::invalid_options);
   BOOST_CHECK_THROW(server.register_protocol_handler(
                         protocol_id{.value = "/" + std::string(identify::limits{}.max_protocol_size, 'x')},
                         [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                            co_await incoming.stream.async_close();
                         }),
                     exceptions::invalid_options);
   auto count_options = make_lifecycle_node_options("lifecycle-identify-count-limit");
   count_options.identify.max_protocols = 9;
   const auto count_limit = count_options.identify.max_protocols;
   count_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   auto count_limited = node{runtime, std::move(count_options)};
   auto count_rejected = std::optional<protocol_id>{};
   // Service opt-in changes the builtin count. Fill the configured capacity,
   // then verify the actual advertised count rather than assuming nine builtins.
   for (auto index = std::size_t{}; index <= count_limit; ++index) {
      auto protocol = protocol_id{.value = "/product/count-limit/" + std::to_string(index)};
      try {
         count_limited.register_protocol_handler(protocol,
             [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                co_await incoming.stream.async_close();
             });
      } catch (const exceptions::backpressure_rejected&) {
         count_rejected = std::move(protocol);
         break;
      }
   }
   BOOST_REQUIRE(count_rejected);
   BOOST_CHECK_THROW(count_limited.register_protocol_handler(
                         *count_rejected,
                         [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                            co_await incoming.stream.async_close();
                         }),
                     exceptions::backpressure_rejected);

   auto accepted = std::vector<protocol_id>{};
   auto rejected = std::optional<protocol_id>{};
   for (auto index = 0U; index < 64U; ++index) {
      auto protocol = protocol_id{
          .value = "/product/identify-capacity/" + std::to_string(index) + "/" + std::string(180, 'x'),
      };
      try {
         server.register_protocol_handler(protocol,
                                          [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                             co_await incoming.stream.async_close();
                                          });
         accepted.push_back(std::move(protocol));
      } catch (const exceptions::backpressure_rejected&) {
         rejected = std::move(protocol);
         break;
      }
   }
   BOOST_REQUIRE(rejected.has_value());
   BOOST_REQUIRE(!accepted.empty());
   BOOST_TEST(!server.unregister_protocol_handler(*rejected));

   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());
   auto client = node{runtime, make_lifecycle_node_options("lifecycle-identify-capacity-client")};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::identified));

   const auto record = client.peers().find(server.local_peer());
   BOOST_REQUIRE(record.has_value());
   BOOST_TEST(!supports(*record, *rejected));
   BOOST_TEST(std::ranges::all_of(accepted, [&](const auto& protocol) { return supports(*record, protocol); }));

   static_cast<void>(forge::asio::blocking::run(runtime, count_limited.async_start()));
   const auto count_address = count_limited.local_endpoint();
   BOOST_REQUIRE(count_address);
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_connect(*count_address)));
   const auto count_record = client.peers().find(count_limited.local_peer());
   BOOST_REQUIRE(count_record);
   BOOST_TEST(count_record->protocols.size() == count_limit);
   BOOST_TEST(!supports(*count_record, *count_rejected));

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
   forge::asio::blocking::run(runtime, count_limited.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_identify_push_fanout_covers_multiple_bounded_batches) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto product_protocol = protocol_id{.value = "/product/lifecycle-fanout/1"};

   auto server_options = make_lifecycle_node_options("lifecycle-fanout-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   server_options.identify.max_push_operations = 2;
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   auto clients = std::vector<std::unique_ptr<node>>{};
   for (auto index = 0U; index < 5U; ++index) {
      auto client = std::make_unique<node>(
          runtime, make_lifecycle_node_options("lifecycle-fanout-client-" + std::to_string(index)));
      static_cast<void>(forge::asio::blocking::run(runtime, client->async_start()));
      static_cast<void>(forge::asio::blocking::run(
          runtime, client->async_connect(*address, node::connect_options{.expected_peer = server.local_peer()})));
      clients.push_back(std::move(client));
   }

   server.register_protocol_handler(product_protocol,
                                    [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
                                       co_await incoming.stream.async_close();
                                    });
   BOOST_REQUIRE(eventually(
       [&] {
          return std::ranges::all_of(clients, [&](const auto& client) {
             const auto record = client->peers().find(server.local_peer());
             return record && supports(*record, product_protocol);
          });
       },
       std::chrono::seconds{3}));

   BOOST_TEST(server.unregister_protocol_handler(product_protocol));
   BOOST_REQUIRE(eventually(
       [&] {
          return std::ranges::all_of(clients, [&](const auto& client) {
             const auto record = client->peers().find(server.local_peer());
             return record && !supports(*record, product_protocol);
          });
       },
       std::chrono::seconds{3}));

   for (auto& client : clients) {
      forge::asio::blocking::run(runtime, client->async_stop());
   }
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(p2p_node_identify_failure_keeps_authenticated_session_usable) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};

   auto server_options = make_lifecycle_node_options("lifecycle-identify-failure-server");
   server_options.lifecycle.listen = {parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   server_options.agent_version = std::string(128, 'a');
   auto server = node{runtime, std::move(server_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, server.async_start()));

   auto client_options = make_lifecycle_node_options("lifecycle-identify-failure-client");
   client_options.identify.max_version_size = 32;
   auto client = node{runtime, std::move(client_options)};
   static_cast<void>(forge::asio::blocking::run(runtime, client.async_start()));
   const auto address = server.local_endpoint();
   BOOST_REQUIRE(address.has_value());

   const auto session = forge::asio::blocking::run(
       runtime, client.async_connect(*address, node::connect_options{.expected_peer = server.local_peer()}));
   BOOST_TEST(static_cast<int>(session.identify_state) == static_cast<int>(identify::state::failed));
   BOOST_TEST(session.capabilities.bits == 0U);

   const auto pushed_protocol = protocol_id{.value = "/product/push-after-failed-identify/1"};
   auto push = forge::asio::blocking::run(
       runtime, server.async_open_protocol_stream(client.local_peer(), builtins::identify_push));
   const auto document = identify::document{
       .protocol_version = "/push/1",
       .agent_version = "push/1",
       .protocols = std::vector<protocol_id>{builtins::ping, builtins::identify_push, pushed_protocol},
   };
   forge::asio::blocking::run(runtime, push.async_write(length_delimited(identify::encode(document))));
   forge::asio::blocking::run(runtime, push.async_close());
   BOOST_REQUIRE(eventually(
       [&] {
          const auto record = client.peers().find(server.local_peer());
          return record && supports(*record, pushed_protocol);
       },
       std::chrono::seconds{2}));
   const auto diagnostics = client.diagnostics();
   const auto active = std::ranges::find_if(
       diagnostics.sessions, [&](const auto& value) { return value.remote_peer == server.local_peer(); });
   BOOST_REQUIRE(active != diagnostics.sessions.end());
   BOOST_TEST(static_cast<int>(active->identify_state) == static_cast<int>(identify::state::failed));
   BOOST_TEST(active->capabilities.bits == 0U);

   const auto latency = forge::asio::blocking::run(runtime, client.async_ping(server.local_peer()));
   BOOST_TEST(latency >= std::chrono::milliseconds{0});

   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

} // namespace forge::net::p2p
