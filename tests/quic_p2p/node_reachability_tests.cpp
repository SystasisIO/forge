#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <future>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

import forge.asio.runtime;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.stream;
import forge.net.p2p.topology;

namespace {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

template <typename T>
T bounded(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   BOOST_REQUIRE(future.wait_for(5s) == std::future_status::ready);
   return future.get();
}

p2p::node::options options_for(std::string name, bool service = false) {
   auto identity = forge::tests::p2p::make_identity_fixture(std::move(name));
   auto options = p2p::node::options{};
   options.certificate_pem = std::move(identity.certificate_pem);
   options.private_key_pem = std::move(identity.private_key_pem);
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   options.limits.topology.operating_mode = p2p::topology::mode::static_only;
   options.reachability_policy.service_v1_enabled = service;
   options.reachability_policy.service_v2_enabled = service;
   options.reachability_policy.timeout = 500ms;
   options.reachability_policy.ping_timeout = 500ms;
   options.lifecycle.listen.push_back(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0"));
   return options;
}

bool supports(const p2p::peer_store::record& record, const p2p::protocol_id& protocol) {
   return std::ranges::find(record.protocols, protocol) != record.protocols.end();
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_node_reachability)

BOOST_AUTO_TEST_CASE(identify_advertises_opt_in_service_roles_not_legacy_capability_bits) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto client = p2p::node{runtime, options_for("reachability-role-client")};
   auto service = p2p::node{runtime, options_for("reachability-role-server", true)};
   static_cast<void>(bounded(runtime, client.async_start()));
   static_cast<void>(bounded(runtime, service.async_start()));
   static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   const auto remote = client.peers().find(service.local_peer());
   BOOST_REQUIRE(remote);
   BOOST_TEST(supports(*remote, p2p::builtins::autonat_v1));
   BOOST_TEST(supports(*remote, p2p::builtins::autonat_v2_dial_request));
   BOOST_TEST(supports(*remote, p2p::builtins::autonat_v2_dial_back));
   static_cast<void>(bounded(runtime, service.async_connect(client.local_endpoints().front())));
   const auto local = service.peers().find(client.local_peer());
   BOOST_REQUIRE(local);
   BOOST_TEST(!supports(*local, p2p::builtins::autonat_v1));
   BOOST_TEST(!supports(*local, p2p::builtins::autonat_v2_dial_request));
   BOOST_TEST(supports(*local, p2p::builtins::autonat_v2_dial_back));
   BOOST_CHECK(client.reachability_status().effective == p2p::reachability::state::unknown);
   bounded(runtime, client.async_stop());
   bounded(runtime, service.async_stop());
}

BOOST_AUTO_TEST_CASE(node_shutdown_closes_surviving_host_subscription) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto value = p2p::node{runtime, options_for("reachability-event-stop")};
   auto subscription = value.host_events();
   const auto initial = bounded(runtime, subscription.async_read());
   BOOST_REQUIRE(initial);
   BOOST_CHECK(initial->effective == p2p::reachability::state::unknown);
   static_cast<void>(bounded(runtime, value.async_start()));
   bounded(runtime, value.async_stop());
   BOOST_TEST(!bounded(runtime, subscription.async_read()).has_value());
   BOOST_TEST(!subscription.active());
}

BOOST_AUTO_TEST_SUITE_END()
