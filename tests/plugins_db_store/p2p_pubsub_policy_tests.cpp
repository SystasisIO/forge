#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <vector>

import forge.app.exceptions;
import forge.config.core.document;
import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.pubsub_api;
import forge.plugins.net.p2p.node.types;

#include "details/secure_node_fixture.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using forge::tests::p2p::secure_node_fixture;
using namespace std::chrono_literals;

template<class Predicate> boost::asio::awaitable<bool> await_state(Predicate predicate) {
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + 10ms));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
   co_return true;
}

boost::asio::awaitable<bool> await_remote_topic(std::shared_ptr<np::pubsub_api> api) {
   co_return co_await await_state([api] { return api->snapshot().core.peers != 0; });
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_plugin_pubsub_policies,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(configured_signed_and_unsigned_payload_limits_are_delivered_over_authenticated_tcp) {
   constexpr std::size_t payload_size = 1024 * 1024;
   auto payload = std::vector<std::uint8_t>(payload_size);
   for (std::size_t i = 0; i < payload.size(); ++i) { payload[i] = static_cast<std::uint8_t>(i % 251); }
   const auto topic = p2p::pubsub::topic{std::string(255, 'x')};
   for (const auto& backend : secure_node_fixture::backends()) {
      for (const auto sign : {false, true}) {
         BOOST_TEST_CONTEXT(backend << " sign=" << sign) {
            auto config = forge::config::core::document{};
            config.set("plugins.net.p2p.node.pubsub.enabled", true);
            config.set("plugins.net.p2p.node.pubsub.sign-publishes", sign);
            config.set("plugins.net.p2p.node.pubsub.signature-policy", sign ? "strict-sign" : "strict-no-sign");
            config.set("plugins.net.p2p.node.pubsub.preferred-version", "v1-2");
            config.set("plugins.net.p2p.node.pubsub.flood-publish", true);
            auto receiver = secure_node_fixture{backend, {.private_profile = false}, "noise", config};
            auto sender = secure_node_fixture{backend, {.private_profile = false}, "noise", config};
            receiver.start();
            sender.start();
            const auto receiver_api = receiver.api<np::api>();
            const auto sender_api = sender.api<np::api>();
            const auto receiver_pubsub = receiver.api<np::pubsub_api>();
            const auto sender_pubsub = sender.api<np::pubsub_api>();
            auto delivered = std::make_shared<std::promise<np::message>>();
            auto count = std::make_shared<std::atomic<unsigned>>(0);
            auto result = delivered->get_future();
            auto subscription = receiver.run(receiver_pubsub->subscribe(topic,
                [delivered, count](np::message message) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
                   if (count->fetch_add(1) == 0) { delivered->set_value(std::move(message)); }
                   co_return p2p::pubsub::validation_result::accept;
                }));
            auto endpoint = receiver_api->local_endpoint();
            BOOST_REQUIRE(endpoint.has_value());
            endpoint->peer = receiver_api->local_peer();
            const auto session = sender.run(sender_api->connect(*endpoint));
            BOOST_CHECK(session.remote_peer == receiver_api->local_peer());
            const auto diagnostics = sender.api<np::diagnostics_api>()->snapshot();
            BOOST_CHECK(std::ranges::any_of(diagnostics.sessions, [&](const auto& value) {
               return value.remote_peer == receiver_api->local_peer() && !value.closed &&
                   value.authentication == p2p::peer_authentication::noise && value.muxer.value == "/yamux/1.0.0";
            }));
            BOOST_REQUIRE(sender.run(await_remote_topic(sender_pubsub)));
            (void)sender.run(sender_pubsub->publish(topic, payload));
            BOOST_REQUIRE(result.wait_for(5s) == std::future_status::ready);
            const auto message = result.get();
            BOOST_CHECK(message.source == sender_api->local_peer());
            BOOST_TEST(message.author.has_value() == sign);
            if (sign) {
               BOOST_REQUIRE(message.author.has_value());
               BOOST_CHECK(*message.author == sender_api->local_peer());
            }
            BOOST_TEST(message.subject.value == topic.value);
            BOOST_CHECK(message.data == payload);
            BOOST_CHECK_THROW(sender.run(sender_pubsub->publish(topic,
                std::vector<std::uint8_t>(payload_size + 1))), np::exceptions::message_too_large);
            receiver.run(receiver_pubsub->unsubscribe(subscription));
            receiver.request_stop_node();
            receiver.run(receiver.shutdown_node());
            sender.request_stop_node();
            sender.run(sender.shutdown_node());
            BOOST_TEST(count->load() == 1U);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(typed_pubsub_snapshot_keeps_protocol_score_callback_and_limit_across_rejected_yaml_reconfigure) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         auto config = forge::config::core::document{};
         config.set("plugins.net.p2p.node.pubsub.enabled", true);
         config.set("plugins.net.p2p.node.pubsub.preferred-version", "v1-2");
         auto receiver = secure_node_fixture{backend, {.private_profile = false}, "noise", config};
         auto sender = secure_node_fixture{backend, {.private_profile = false}, "noise", config};
         auto score_calls = std::make_shared<std::atomic<unsigned>>(0);
         auto traced_v12 = std::make_shared<std::atomic<bool>>(false);
         auto options = p2p::pubsub::options{};
         options.preferred = p2p::pubsub::version::v1_2;
         options.flood_publish = true;
         options.limits.max_data_size = 32;
         options.scoring.emplace();
         options.scoring->app_specific_weight = 2;
         options.scoring->app_specific_score = [score_calls](const p2p::peer_id&) {
            ++*score_calls;
            return 3.;
         };
         options.tracer = [traced_v12](const p2p::pubsub::trace_event& event) {
            if (event.kind == p2p::pubsub::trace_kind::rpc_write && event.protocol.value == "/meshsub/1.2.0" &&
                !event.framed_rpc.empty()) { *traced_v12 = true; }
         };
         sender.run(sender.shell->initialize());
         const auto configuration = sender.api<np::configuration_api>();
         configuration->set_pubsub_options(options);
         auto invalid = options;
         invalid.limits.max_data_size = 0;
         BOOST_CHECK_THROW(configuration->set_pubsub_options(invalid), np::exceptions::invalid_config);
         auto competing = sender.config;
         competing.set("plugins.net.p2p.node.pubsub.preferred-version", "v1-0");
         competing.set("plugins.net.p2p.node.pubsub.max-message-size", std::uint64_t{64});
         competing.set("plugins.net.p2p.node.pubsub.flood-publish", false);
         BOOST_CHECK_THROW(sender.configure_node(competing), forge::app::exceptions::invalid_state);
         receiver.start();
         sender.start();
         BOOST_CHECK_THROW(configuration->set_pubsub_options(options), np::exceptions::route_conflict);
         const auto receiver_api = receiver.api<np::api>();
         const auto sender_pubsub = sender.api<np::pubsub_api>();
         const auto receiver_pubsub = receiver.api<np::pubsub_api>();
         const auto diagnostics = sender.api<np::diagnostics_api>();
         const auto topic = p2p::pubsub::topic{"typed-snapshot"};
         auto delivered = std::make_shared<std::atomic<unsigned>>(0);
         auto subscription = receiver.run(receiver_pubsub->subscribe(topic,
             [delivered](np::message message) -> boost::asio::awaitable<p2p::pubsub::validation_result> {
                if (message.data == std::vector<std::uint8_t>(32, 0x4a)) { ++*delivered; }
                co_return p2p::pubsub::validation_result::accept;
             }));
         auto endpoint = receiver_api->local_endpoint();
         BOOST_REQUIRE(endpoint.has_value());
         endpoint->peer = receiver_api->local_peer();
         const auto connected = sender.run(sender.api<np::api>()->connect(*endpoint));
         BOOST_CHECK(connected.remote_peer == receiver_api->local_peer());
         BOOST_REQUIRE(sender.run(await_remote_topic(sender_pubsub)));
         (void)sender.run(sender_pubsub->publish(topic, std::vector<std::uint8_t>(32, 0x4a)));
         BOOST_REQUIRE(sender.run(await_state([&] { return delivered->load() == 1 && traced_v12->load(); })));
         BOOST_CHECK_THROW(sender.run(sender_pubsub->publish(topic, std::vector<std::uint8_t>(33, 0x4a))),
                           p2p::exceptions::backpressure_rejected);
         BOOST_REQUIRE(sender.run(await_state([&] {
            const auto scores = diagnostics->pubsub_scores();
            return std::ranges::any_of(scores.peers, [&](const auto& peer) {
               return peer.peer == receiver_api->local_peer() && peer.app_specific_sample == 3. &&
                   peer.app_specific_score == 6. && peer.value == 6.;
            });
         })));
         BOOST_TEST(score_calls->load() > 0U);
         receiver.run(receiver_pubsub->unsubscribe(subscription));
         receiver.request_stop_node();
         receiver.run(receiver.shutdown_node());
         sender.request_stop_node();
         sender.run(sender.shutdown_node());
         BOOST_TEST(delivered->load() == 1U);
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
