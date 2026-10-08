module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.crypto.pki.pem;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;

#include "gossipsub_test_receipts.hxx"
#include "gossipsub_test_shutdown.hxx"
#include "pubsub_router_fixture.hxx"
#include "gossipsub_validation_tests.hxx"

namespace forge::tests::p2p::gossipsub_validation_tests {
using namespace forge::net::p2p;
using namespace std::chrono_literals;
using forge::tests::p2p::make_identity_fixture;

node::options options_for(const identity_fixture& identity, capability_set capabilities) {
   auto options = node::options{.certificate_pem = identity.certificate_pem,
       .private_key_pem = identity.private_key_pem, .capabilities = capabilities};
   options.peer_state.persistence = peer_store::make_memory_persistence();
   options.relay_policy.service_enabled = capabilities.has(capabilities::relay);
   options.relay_policy.auto_discovery_enabled = false;
   options.dht_profiles.clear();
   return options;
}

node::options pubsub_options_for(const identity_fixture& identity) {
   return options_for(identity);
}

endpoint listen(node& owner, forge::asio::runtime& runtime) {
   forge::asio::blocking::run(runtime, owner.async_listen(parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1")));
   return owner.local_endpoints().front();
}

endpoint listen_tcp(node& owner, forge::asio::runtime& runtime) {
   forge::asio::blocking::run(runtime, owner.async_listen(parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   return owner.local_endpoints().front();
}

void wait_on_runtime(forge::asio::runtime& runtime, std::chrono::milliseconds delay, std::string_view) {
   auto wait = [delay]() -> boost::asio::awaitable<void> {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      timer.expires_after(delay);
      co_await timer.async_wait(boost::asio::use_awaitable);
   };
   forge::asio::blocking::run(runtime, wait());
}

void wait_for_gossipsub(forge::asio::runtime& runtime, std::string_view label,
                       const std::function<bool()>& ready) {
   const auto deadline = std::chrono::steady_clock::now() + 5s;
   while (!ready()) {
      if (std::chrono::steady_clock::now() >= deadline) {
         throw std::runtime_error{std::string{label} + " did not finish"};
      }
      wait_on_runtime(runtime, 5ms, label);
   }
}

bool authenticated_gossipsub_peer(const node& owner, const peer_id& peer) {
   return std::ranges::any_of(owner.diagnostics().sessions, [&](const auto& row) {
      return row.id != 0 && !row.closed && row.remote_peer == peer &&
             row.authentication != peer_authentication::unverified;
   });
}

bool gossipsub_mesh_peer(const node& owner, const peer_id& peer, const pubsub::topic& subject) {
   return std::ranges::any_of(owner.pubsub_scores().peers, [&](const auto& row) {
      return row.peer == peer && row.connected && std::ranges::any_of(row.topics, [&](const auto& topic) {
         return topic.subject == subject && topic.in_mesh;
      });
   });
}

pubsub::options native_options(const pubsub::topic& topic) {
   auto result = pubsub::options{};
   result.limits.mesh_n = 2;
   result.limits.mesh_n_low = 2;
   result.limits.mesh_n_high = 4;
   result.limits.mesh_outbound_min = 0;
   result.limits.mesh_score_min = 1;
   result.limits.history_length = 128;
   result.limits.history_gossip = 1;
   result.limits.heartbeat_initial_delay = 20ms;
   result.limits.heartbeat_interval = 40ms;
   result.limits.validation_retry_initial_delay = 200ms;
   result.limits.validation_retry_max_delay = 200ms;
   result.limits.prune_backoff = 1s;
   // Retry-budget tests finish before an unanswered request becomes a separate broken-promise penalty.
   result.limits.iwant_followup_time = 10s;
   result.scoring.emplace();
   result.scoring->topics.emplace(topic, pubsub::topic_score_params{});
   result.scoring->behaviour_penalty_weight = -2;
   result.scoring->decay_interval = 60s;
   return result;
}

std::vector<pubsub::rpc> read_rpcs(const pubsub_router_fixture& state, const node& owner, const peer_id& peer,
                                  pubsub::trace_kind kind) {
   auto result = std::vector<pubsub::rpc>{};
   for (const auto& event : state.receipts(owner)) {
      if (event.kind != kind || event.peer != peer || event.frame.empty()) { continue; }
      if (event.session == 0 || event.stream < 0 || event.generation == 0 ||
          event.protocol != builtins::meshsub_v11) {
         throw std::runtime_error{"native GossipSub RPC lacks its negotiated session/stream owner"};
      }
      result.push_back(pubsub::codec::decode(event.frame));
   }
   return result;
}

std::size_t want_count(const pubsub_router_fixture& state, const node& owner, const peer_id& peer,
                       const std::vector<std::uint8_t>& id) {
   auto result = std::size_t{};
   for (const auto& rpc : read_rpcs(state, owner, peer)) {
      if (!rpc.control_value) { continue; }
      for (const auto& want : rpc.control_value->want) {
         result += static_cast<std::size_t>(std::ranges::count(want.message_ids, id));
      }
   }
   return result;
}

bool committed(const pubsub_router_fixture& state, const node& owner, const std::vector<std::uint8_t>& id,
                pubsub::validation_result result) {
   return std::ranges::any_of(state.receipts(owner), [&](const auto& event) {
      return event.kind == pubsub::trace_kind::validation_committed && event.id == id && event.result == result &&
             event.session != 0 && event.stream >= 0 && event.generation != 0 &&
             event.protocol == builtins::meshsub_v11;
   });
}

pubsub::peer_score_snapshot score_of(const node& owner, const peer_id& peer) {
   for (const auto& row : owner.pubsub_scores().peers) {
      if (row.peer == peer) { return row; }
   }
   throw std::runtime_error{"authenticated native GossipSub peer has no score row"};
}

void connect_ready(pubsub_router_fixture& state, node& source, node& target) {
   state.connect(source, target);
   BOOST_REQUIRE(state.wait([&] {
      const auto subscription = [&](const node& owner, const peer_id& peer) {
         return std::ranges::any_of(read_rpcs(state, owner, peer), [&](const auto& rpc) {
            return std::ranges::any_of(rpc.subscriptions, [&](const auto& item) {
               return item.subscribe && item.subject == state.topic;
            });
         });
      };
      return authenticated_gossipsub_peer(source, target.local_peer()) &&
             authenticated_gossipsub_peer(target, source.local_peer()) &&
             subscription(source, target.local_peer()) && subscription(target, source.local_peer()) &&
             gossipsub_mesh_peer(source, target.local_peer(), state.topic) &&
             gossipsub_mesh_peer(target, source.local_peer(), state.topic);
   }));
}

void read_barrier(pubsub_router_fixture& state, stream& channel, node& receiver, const peer_id& source) {
   const auto subject = pubsub::topic{.value = "forge.pubsub.barrier." +
       std::to_string(state.receipts(receiver).size())};
   const auto frame = pubsub::codec::encode(pubsub::rpc{.subscriptions = {{.subscribe = false, .subject = subject}}});
   forge::asio::blocking::run(state.runtime, channel.async_write(frame));
   BOOST_REQUIRE(state.wait([&] {
      return std::ranges::any_of(state.receipts(receiver), [&](const auto& event) {
         return event.kind == pubsub::trace_kind::rpc_read && event.peer == source &&
                event.stream == channel.id() && event.session != 0 && event.generation != 0 && event.frame == frame;
      });
   }));
   // The same native read loop cannot read this frame before finishing the preceding RPC.
}

void nodes_deliver_signed_publish_over_negotiated_stream() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto publisher_options = pubsub_options_for(make_identity_fixture("pubsub-signed-publisher"));
   auto subscriber_options = pubsub_options_for(make_identity_fixture("pubsub-signed-subscriber"));
   publisher_options.allow_insecure_test_mode = false;
   subscriber_options.allow_insecure_test_mode = false;
   auto receipts = std::make_shared<gossipsub_test_receipts>();
   publisher_options.limits.pubsub.tracer = [receipts](const auto& event) { receipts->capture(event); };
   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, subscriber};
   static_cast<void>(listen(publisher, runtime));
   const auto subscriber_endpoint = listen(subscriber, runtime);

   auto received = std::make_shared<std::promise<std::vector<std::uint8_t>>>();
   auto future = received->get_future();
   const auto subject = pubsub::topic{.value = "forge.pubsub"};
   forge::asio::blocking::run(
       runtime, publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   forge::asio::blocking::run(
       runtime, subscriber.async_subscribe(
                    subject,
                    [received](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
                       received->set_value(event.value.data);
                       co_return pubsub::validation_result::accept;
                    }));

   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   forge::asio::blocking::run(
       runtime, publisher.async_connect(subscriber_endpoint, node::connect_options{.expected_peer = subscriber.local_peer()}));
   wait_for_gossipsub(runtime, "signed publish authenticated subscription and mesh", [&] {
      return authenticated_gossipsub_peer(publisher, subscriber.local_peer()) &&
             authenticated_gossipsub_peer(subscriber, publisher.local_peer()) &&
             receipts->subscribed(subscriber.local_peer(), subject) &&
             gossipsub_mesh_peer(publisher, subscriber.local_peer(), subject);
   });

   const auto published = forge::asio::blocking::run(
       runtime, publisher.async_publish(subject,
                                        std::vector<std::uint8_t>{'p', 'u', 'b', 's', 'u', 'b'}));
   BOOST_TEST(!published.signature.empty());

   if (future.wait_for(std::chrono::milliseconds{5'000}) != std::future_status::ready) {
      const auto metrics = subscriber.metrics();
      BOOST_FAIL("pubsub delivery did not finish; received="
                 << metrics.pubsub_messages_received << " delivered=" << metrics.pubsub_messages_delivered
                 << " invalid=" << metrics.pubsub_invalid_messages << " duplicates=" << metrics.pubsub_duplicates
                 << " rejected=" << metrics.protocol_rejections);
   }
   BOOST_TEST(future.get() == std::vector<std::uint8_t>({'p', 'u', 'b', 's', 'u', 'b'}),
              boost::test_tools::per_element());
   wait_for_gossipsub(runtime, "signed publish native delivery committed", [&] {
      return subscriber.pubsub_snapshot().messages_delivered >= 1U;
   });
   BOOST_TEST(publisher.pubsub_snapshot().messages_published >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered >= 1U);

   shutdown.join();
}

void retry_is_redelivered_after_bounded_cooldown() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.limits.heartbeat_initial_delay = 10ms;
   config.limits.heartbeat_interval = 20ms;
   config.limits.validation_retry_initial_delay = 40ms;
   config.limits.validation_retry_max_delay = 80ms;
   config.limits.history_gossip = 1;
   auto& publisher = state.add("pubsub-retry-publisher", config);
   auto& subscriber = state.add("pubsub-retry-subscriber", config);
   auto shutdown = gossipsub_test_shutdown{state.runtime, publisher, subscriber};
   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   auto first_attempt = std::make_shared<std::promise<void>>();
   auto first_attempt_future = first_attempt->get_future();
   auto accepted = std::make_shared<std::promise<void>>();
   auto accepted_future = accepted->get_future();
   auto callback_mutex = std::make_shared<std::mutex>();
   auto callback_times = std::make_shared<std::array<std::chrono::steady_clock::time_point, 2>>();
   state.subscribe(publisher);
   state.subscribe(subscriber, [attempts, first_attempt, accepted, callback_mutex, callback_times](pubsub::event)
       -> boost::asio::awaitable<pubsub::validation_result> {
      const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
      if (attempt == 0) {
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         timer.expires_after(25ms);
         co_await timer.async_wait(boost::asio::use_awaitable);
         {
            const auto lock = std::scoped_lock{*callback_mutex};
            (*callback_times)[0] = std::chrono::steady_clock::now();
         }
         first_attempt->set_value();
         co_return pubsub::validation_result::retry;
      }
      if (attempt == 1) {
         {
            const auto lock = std::scoped_lock{*callback_mutex};
            (*callback_times)[1] = std::chrono::steady_clock::now();
         }
         accepted->set_value();
      }
      co_return pubsub::validation_result::accept;
   });
   connect_ready(state, publisher, subscriber);
   const auto message = forge::asio::blocking::run(state.runtime,
       publisher.async_publish(state.topic, {'r', 'e', 't', 'r', 'y'}));
   const auto id = pubsub::codec::message_id(message);
   BOOST_REQUIRE(first_attempt_future.wait_for(5s) == std::future_status::ready);
   first_attempt_future.get();
   const auto noise_subject = pubsub::topic{.value = "forge.pubsub.retry.noise"};
   for (auto value = std::uint8_t{}; value < 8; ++value) {
      static_cast<void>(forge::asio::blocking::run(state.runtime,
          publisher.async_publish(noise_subject, {value})));
   }
   BOOST_REQUIRE(accepted_future.wait_for(5s) == std::future_status::ready);
   accepted_future.get();
   // Handler entry is not a commit or delivery barrier.
   BOOST_REQUIRE(state.wait([&] {
      return committed(state, subscriber, id, pubsub::validation_result::accept) &&
             subscriber.pubsub_snapshot().messages_delivered == 1U;
   }));
   BOOST_REQUIRE(state.wait([&] {
      return std::ranges::count_if(read_rpcs(state, publisher, subscriber.local_peer(),
          pubsub::trace_kind::rpc_write), [&](const auto& rpc) {
         return std::ranges::any_of(rpc.messages, [&](const auto& sent) {
            return pubsub::codec::message_id(sent) == id;
         });
      }) == 2;
   }));
   const auto accepted_commits = std::ranges::count_if(state.receipts(subscriber), [&](const auto& event) {
      return event.kind == pubsub::trace_kind::validation_committed && event.peer == publisher.local_peer() &&
             event.id == id && event.result == pubsub::validation_result::accept &&
             event.session != 0 && event.stream >= 0 && event.generation != 0;
   });
   BOOST_TEST(accepted_commits == 1);
   {
      const auto lock = std::scoped_lock{*callback_mutex};
      BOOST_CHECK((*callback_times)[1] >= (*callback_times)[0] + config.limits.validation_retry_initial_delay);
   }
   BOOST_TEST(want_count(state, publisher, subscriber.local_peer(), id) == 1U);
   auto reads = std::size_t{};
   for (const auto& rpc : read_rpcs(state, subscriber, publisher.local_peer())) {
      for (const auto& received : rpc.messages) {
         if (pubsub::codec::message_id(received) != id) { continue; }
         ++reads;
         BOOST_CHECK(received.from == message.from);
         BOOST_CHECK(received.subject == message.subject);
         BOOST_TEST(received.seqno == message.seqno, boost::test_tools::per_element());
         BOOST_TEST(received.signature == message.signature, boost::test_tools::per_element());
         BOOST_TEST(received.data == message.data, boost::test_tools::per_element());
      }
   }
   BOOST_TEST(reads == 2U);
   // The cached signed response must be a real native write, not a second local publish.
   BOOST_TEST(std::ranges::count_if(read_rpcs(state, publisher, subscriber.local_peer(),
       pubsub::trace_kind::rpc_write), [&](const auto& rpc) {
      return std::ranges::any_of(rpc.messages, [&](const auto& sent) {
         return pubsub::codec::message_id(sent) == id && sent.signature == message.signature;
      });
   }) == 2);
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 2U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 1U);
   shutdown.join();
}

void ignore_remains_terminal_during_history_window() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.signatures = pubsub::signature_policy::lax_no_sign;
   config.limits.heartbeat_initial_delay = 10ms;
   config.limits.heartbeat_interval = 20ms;
   config.limits.validation_retry_initial_delay = 20ms;
   config.limits.validation_retry_max_delay = 40ms;
   auto& publisher = state.add("pubsub-ignore-publisher", config, {}, "/ip4/127.0.0.1/udp/0/quic-v1");
   auto& subscriber = state.add("pubsub-ignore-subscriber", config, {}, "/ip4/127.0.0.1/udp/0/quic-v1");
   auto shutdown = gossipsub_test_shutdown{state.runtime, publisher, subscriber};
   auto attempts = std::make_shared<std::atomic_uint64_t>(0);
   state.subscribe(publisher);
   state.subscribe(subscriber, [attempts](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      attempts->fetch_add(1, std::memory_order_relaxed);
      co_return pubsub::validation_result::ignore;
   });
   connect_ready(state, publisher, subscriber);
   const auto message = forge::asio::blocking::run(state.runtime,
       publisher.async_publish(state.topic, {'i', 'g', 'n', 'o', 'r', 'e'}, pubsub::publish_options{.sign = false}));
   const auto id = pubsub::codec::message_id(message);
   BOOST_REQUIRE(state.wait([&] { return committed(state, subscriber, id, pubsub::validation_result::ignore); }));
   auto committed_at = std::chrono::steady_clock::time_point{};
   for (const auto& event : state.receipts(subscriber)) {
      if (event.kind == pubsub::trace_kind::validation_committed && event.peer == publisher.local_peer() &&
          event.id == id && event.result == pubsub::validation_result::ignore) {
         committed_at = event.observed;
         break;
      }
   }
   BOOST_REQUIRE((committed_at != std::chrono::steady_clock::time_point{}));
   const auto due = committed_at + config.limits.validation_retry_max_delay + 2 * config.limits.heartbeat_interval;
   BOOST_REQUIRE(state.wait([&] { return std::chrono::steady_clock::now() >= due; }));
   BOOST_REQUIRE(subscriber.pubsub_snapshot().cached_messages == 1U);
   auto replay = state.open(publisher, subscriber);
   const auto duplicates_before = subscriber.pubsub_snapshot().duplicates;
   state.send(replay, pubsub::rpc{.messages = {message}});
   read_barrier(state, replay, subscriber, publisher.local_peer());
   BOOST_TEST(subscriber.pubsub_snapshot().duplicates == duplicates_before + 1U);
   BOOST_TEST(attempts->load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(subscriber.pubsub_snapshot().cached_messages == 1U);
   BOOST_TEST(std::ranges::count_if(state.receipts(subscriber), [&](const auto& event) {
      return event.kind == pubsub::trace_kind::validation_committed && event.id == id &&
             event.result == pubsub::validation_result::ignore;
   }) == 1);
   for (const auto& rpc : read_rpcs(state, subscriber, publisher.local_peer(), pubsub::trace_kind::rpc_write)) {
      if (!rpc.control_value) { continue; }
      for (const auto& want : rpc.control_value->want) {
         BOOST_TEST(std::ranges::count(want.message_ids, id) == 0);
      }
   }
   forge::asio::blocking::run(state.runtime, replay.async_close());
   shutdown.join();
}

void in_progress_validation_is_not_replaced_after_cache_pressure() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.limits.history_length = 1;
   config.limits.history_gossip = 1;
   config.limits.max_messages = 1;
   auto& receiver = state.add("legacy-validation-pressure-owner", config);
   auto remote_config = native_options(state.topic);
   remote_config.limits.heartbeat_initial_delay = 60s;
   auto& first = state.add("legacy-validation-pressure-first", remote_config);
   auto& competing = state.add("legacy-validation-pressure-competing", remote_config);
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::make_shared<std::atomic_size_t>();
   auto second_payloads = std::make_shared<std::atomic_size_t>();
   auto first_shutdown = gossipsub_test_shutdown{state.runtime, first, receiver, [release] { release->notify(); }};
   auto competing_shutdown = gossipsub_test_shutdown{state.runtime, competing, receiver, [release] { release->notify(); }};
   state.subscribe(first);
   state.subscribe(competing);
   state.subscribe(receiver, [release, entered, second_payloads](pubsub::event event)
       -> boost::asio::awaitable<pubsub::validation_result> {
      if (std::ranges::equal(event.value.data, std::string_view{"first"})) {
         entered->fetch_add(1);
         static_cast<void>(co_await release->async_wait(0));
      } else if (std::ranges::equal(event.value.data, std::string_view{"second"})) {
         second_payloads->fetch_add(1);
      }
      co_return pubsub::validation_result::accept;
   });
   connect_ready(state, first, receiver);
   connect_ready(state, competing, receiver);
   const auto key = forge::crypto::pki::pem::read_private_key(
       make_identity_fixture("legacy-validation-pressure-author").private_key_pem);
   auto message = pubsub::message{.data = {'f', 'i', 'r', 's', 't'}, .seqno = {0, 0, 0, 0, 0, 0, 0, 19}, .subject = state.topic};
   pubsub::codec::sign_message(message, key);
   auto equivocation = message;
   equivocation.data = {'s', 'e', 'c', 'o', 'n', 'd'};
   pubsub::codec::sign_message(equivocation, key);
   auto filler = pubsub::message{.data = {'f', 'i', 'l', 'l', 'e', 'r'}, .seqno = {0, 0, 0, 0, 0, 0, 0, 20}, .subject = state.topic};
   pubsub::codec::sign_message(filler, key);
   BOOST_TEST(message.seqno.size() == 8U);
   BOOST_TEST(equivocation.seqno.size() == 8U);
   BOOST_TEST(filler.seqno.size() == 8U);
   BOOST_REQUIRE(pubsub::codec::verify_message(message));
   BOOST_REQUIRE(pubsub::codec::verify_message(equivocation));
   BOOST_REQUIRE(pubsub::codec::verify_message(filler));
   BOOST_REQUIRE(pubsub::codec::message_id(message) == pubsub::codec::message_id(equivocation));
   auto first_stream = state.open(first, receiver);
   auto competing_stream = state.open(competing, receiver);
   state.send(first_stream, pubsub::rpc{.messages = {message}});
   BOOST_REQUIRE(state.wait([&] { return entered->load() == 1U; }));
   const auto before = receiver.metrics();
   state.send(competing_stream, pubsub::rpc{.messages = {filler}});
   state.send(competing_stream, pubsub::rpc{.messages = {equivocation}});
   read_barrier(state, competing_stream, receiver, competing.local_peer());
   BOOST_TEST(receiver.metrics().backpressure_rejections >= before.backpressure_rejections + 1U);
   BOOST_TEST(receiver.metrics().pubsub_invalid_messages == before.pubsub_invalid_messages + 1U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(receiver.pubsub_snapshot().cached_messages == 1U);
   BOOST_TEST(second_payloads->load() == 0U);
   release->notify();
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().messages_delivered == 1U; }));
   BOOST_REQUIRE(committed(state, receiver, pubsub::codec::message_id(message), pubsub::validation_result::accept));
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().cached_messages == 0U; }));
   // The filler was rejected by capacity, not accepted or silently evicted. Replay after real expiry.
   state.send(competing_stream, pubsub::rpc{.messages = {filler}});
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().messages_delivered == 2U; }));
   BOOST_TEST(entered->load() == 1U);
   BOOST_TEST(second_payloads->load() == 0U);
   BOOST_REQUIRE(committed(state, receiver, pubsub::codec::message_id(filler), pubsub::validation_result::accept));
   forge::asio::blocking::run(state.runtime, first_stream.async_close());
   forge::asio::blocking::run(state.runtime, competing_stream.async_close());
   competing_shutdown.join();
   first_shutdown.join();
}

void retry_source_is_not_replaced_by_foreign_ihave() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.limits.max_validation_requests = 2;
   config.limits.validation_retry_initial_delay = 500ms;
   config.limits.validation_retry_max_delay = 500ms;
   auto receiver_config = config;
   receiver_config.limits.heartbeat_initial_delay = 60s;
   const auto deadline = std::chrono::steady_clock::now() + 15s;
   auto& receiver = state.add("legacy-retry-source-owner", receiver_config);
   auto& source = state.add("legacy-retry-source-first", config);
   auto& foreign = state.add("legacy-retry-source-foreign", config);
   auto calls = std::make_shared<std::atomic_size_t>();
   auto accepted = std::make_shared<std::promise<peer_id>>();
   auto accepted_future = accepted->get_future();
   auto source_shutdown = gossipsub_test_shutdown{state.runtime, source, receiver};
   auto foreign_shutdown = gossipsub_test_shutdown{state.runtime, foreign, receiver};
   state.subscribe(source);
   state.subscribe(foreign);
   state.subscribe(receiver, [calls, accepted](pubsub::event event)
       -> boost::asio::awaitable<pubsub::validation_result> {
      if (calls->fetch_add(1) == 0U) { co_return pubsub::validation_result::retry; }
      if (calls->load() == 2U) { accepted->set_value(event.source); }
      co_return pubsub::validation_result::accept;
   });
   connect_ready(state, source, receiver);
   connect_ready(state, foreign, receiver);
   const auto key = forge::crypto::pki::pem::read_private_key(
       make_identity_fixture("legacy-retry-source-author").private_key_pem);
   auto message = pubsub::message{.data = {'s', 'o', 'u', 'r', 'c', 'e'}, .seqno = {0, 0, 0, 0, 0, 0, 0, 21}, .subject = state.topic};
   pubsub::codec::sign_message(message, key);
   BOOST_TEST(message.seqno.size() == 8U);
   BOOST_REQUIRE(pubsub::codec::verify_message(message));
   const auto id = pubsub::codec::message_id(message);
   auto source_stream = state.open(source, receiver);
   auto foreign_stream = state.open(foreign, receiver);
   const auto sent_wants = [&](const peer_id& target) {
      auto count = std::size_t{};
      for (const auto& rpc : read_rpcs(state, receiver, target, pubsub::trace_kind::rpc_write)) {
         if (!rpc.control_value) { continue; }
         for (const auto& want : rpc.control_value->want) {
            count += static_cast<std::size_t>(std::ranges::count(want.message_ids, id));
         }
      }
      return count;
   };
   const auto due_window = [&] {
      // Retry emits no validation_committed event. This same-stream read follows defer_pubsub_message.
      read_barrier(state, source_stream, receiver, source.local_peer());
      const auto due = std::chrono::steady_clock::now() + config.limits.validation_retry_max_delay;
      BOOST_REQUIRE(state.wait([&] { return std::chrono::steady_clock::now() >= due; }));
      BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
      BOOST_REQUIRE(calls->load() == 1U);
      BOOST_REQUIRE(receiver.pubsub_snapshot().cached_messages == 1U);
   };
   const auto have = pubsub::rpc{.control_value = pubsub::control{
       .have = {{.subject = state.topic, .message_ids = {id}}}}};
   // Manual signed input is not cached in source's router, so its native IWANT handler cannot answer.
   state.send(source_stream, pubsub::rpc{.messages = {message}});
   due_window();
   state.send(source_stream, have);
   read_barrier(state, source_stream, receiver, source.local_peer());
   BOOST_REQUIRE(sent_wants(source.local_peer()) == 1U);
   BOOST_TEST(sent_wants(foreign.local_peer()) == 0U);
   due_window();
   state.send(foreign_stream, have);
   read_barrier(state, foreign_stream, receiver, foreign.local_peer());
   BOOST_TEST(sent_wants(foreign.local_peer()) == 0U);
   BOOST_TEST(sent_wants(source.local_peer()) == 1U);
   // In the SAME due window, the original source must consume the still-available second credit.
   state.send(source_stream, have);
   read_barrier(state, source_stream, receiver, source.local_peer());
   BOOST_REQUIRE(sent_wants(source.local_peer()) == 2U);
   BOOST_TEST(sent_wants(foreign.local_peer()) == 0U);
   BOOST_TEST(calls->load() == 1U);
   BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
   state.send(source_stream, pubsub::rpc{.messages = {message}});
   BOOST_REQUIRE(accepted_future.wait_for(5s) == std::future_status::ready);
   BOOST_TEST(accepted_future.get().to_string() == source.local_peer().to_string());
   BOOST_REQUIRE(state.wait([&] {
      return committed(state, receiver, id, pubsub::validation_result::accept) &&
             receiver.pubsub_snapshot().messages_delivered == 1U;
   }));
   BOOST_TEST(calls->load() == 2U);
   BOOST_TEST(score_of(receiver, source.local_peer()).behaviour_penalty == 0.0);
   BOOST_TEST(score_of(receiver, foreign.local_peer()).behaviour_penalty == 0.0);
   BOOST_REQUIRE((std::chrono::steady_clock::now() < deadline));
   forge::asio::blocking::run(state.runtime, source_stream.async_close());
   forge::asio::blocking::run(state.runtime, foreign_stream.async_close());
   foreign_shutdown.join();
   source_shutdown.join();
}

void retry_stops_after_max_attempts_without_duplicate_history() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.limits.max_validation_attempts = 3;
   auto& source = state.add("legacy-retry-limit-source", config);
   auto& receiver = state.add("legacy-retry-limit-owner", config);
   auto calls = std::make_shared<std::atomic_size_t>();
   auto shutdown = gossipsub_test_shutdown{state.runtime, source, receiver};
   state.subscribe(source);
   state.subscribe(receiver, [calls](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      calls->fetch_add(1);
      co_return pubsub::validation_result::retry;
   });
   connect_ready(state, source, receiver);
   const auto message = state.publish(source, "limit");
   const auto id = pubsub::codec::message_id(message);
   BOOST_REQUIRE(state.wait([&] {
      return calls->load() == 3U && receiver.pubsub_scores().pending_validations == 0U;
   }));
   auto replay = state.open(source, receiver);
   state.send(replay, pubsub::rpc{.messages = {message}});
   read_barrier(state, replay, receiver, source.local_peer());
   BOOST_TEST(calls->load() == 3U);
   BOOST_TEST(want_count(state, source, receiver.local_peer(), id) == 2U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(receiver.pubsub_snapshot().cached_messages == 1U);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   BOOST_TEST(score_of(receiver, source.local_peer()).behaviour_penalty == 0.0);
   forge::asio::blocking::run(state.runtime, replay.async_close());
   shutdown.join();
}

void unavailable_source_stops_retry_requests_after_limit() {
   auto state = pubsub_router_fixture{};
   auto source_config = native_options(state.topic);
   source_config.limits.history_length = 1;
   source_config.limits.history_gossip = 1;
   source_config.limits.max_messages = 1;
   auto config = native_options(state.topic);
   config.limits.max_validation_requests = 2;
   auto& source = state.add("legacy-unavailable-source", source_config);
   auto& receiver = state.add("legacy-unavailable-owner", config);
   auto calls = std::make_shared<std::atomic_size_t>();
   auto shutdown = gossipsub_test_shutdown{state.runtime, source, receiver};
   state.subscribe(source);
   state.subscribe(receiver, [calls](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      calls->fetch_add(1);
      co_return pubsub::validation_result::retry;
   });
   connect_ready(state, source, receiver);
   const auto message = state.publish(source, "unavailable");
   const auto id = pubsub::codec::message_id(message);
   BOOST_REQUIRE(state.wait([&] { return calls->load() == 1U && source.pubsub_snapshot().cached_messages == 0U; }));
   BOOST_REQUIRE(state.wait([&] { return want_count(state, source, receiver.local_peer(), id) == 2U; }));
   // This is a policy window (next request deadline plus two real heartbeats), not a delivery sleep.
   wait_on_runtime(state.runtime, config.limits.validation_retry_max_delay + 2 * config.limits.heartbeat_interval,
                   "unavailable-source exhausted request budget");
   const auto at_limit = want_count(state, source, receiver.local_peer(), id);
   BOOST_REQUIRE_EQUAL(at_limit, 2U);
   auto replay = state.open(source, receiver);
   state.send(replay, pubsub::rpc{.messages = {message}});
   read_barrier(state, replay, receiver, source.local_peer());
   BOOST_TEST(want_count(state, source, receiver.local_peer(), id) == at_limit);
   BOOST_TEST(calls->load() == 1U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 0U);
   forge::asio::blocking::run(state.runtime, replay.async_close());
   shutdown.join();
}

void reject_remains_terminal_during_history_window() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   auto& source = state.add("legacy-terminal-reject-source", config);
   auto& receiver = state.add("legacy-terminal-reject-owner", config);
   auto calls = std::make_shared<std::atomic_size_t>();
   auto shutdown = gossipsub_test_shutdown{state.runtime, source, receiver};
   state.subscribe(source);
   state.subscribe(receiver, [calls](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      calls->fetch_add(1);
      co_return pubsub::validation_result::reject;
   });
   connect_ready(state, source, receiver);
   const auto message = state.publish(source, "reject");
   const auto id = pubsub::codec::message_id(message);
   BOOST_REQUIRE(state.wait([&] {
      return committed(state, receiver, id, pubsub::validation_result::reject) &&
             receiver.pubsub_snapshot().invalid_messages == 1U;
   }));
   auto replay = state.open(source, receiver);
   state.send(replay, pubsub::rpc{.messages = {message}});
   read_barrier(state, replay, receiver, source.local_peer());
   BOOST_TEST(calls->load() == 1U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 0U);
   BOOST_TEST(receiver.pubsub_snapshot().invalid_messages == 1U);
   BOOST_TEST(receiver.pubsub_snapshot().cached_messages == 1U);
   BOOST_TEST(want_count(state, source, receiver.local_peer(), id) == 0U);
   forge::asio::blocking::run(state.runtime, replay.async_close());
   shutdown.join();
}

void validation_queue_limit_retries_excess_without_penalizing_peer() {
   auto state = pubsub_router_fixture{};
   auto config = native_options(state.topic);
   config.limits.max_validation_queue = 1;
   config.limits.max_validation_attempts = 1;
   config.limits.validation_retry_initial_delay = 600ms;
   config.limits.validation_retry_max_delay = 600ms;
   auto& receiver = state.add("legacy-validation-queue-owner", config);
   auto& first = state.add("legacy-validation-queue-first", config);
   auto& second = state.add("legacy-validation-queue-second", config);
   auto release = std::make_shared<forge::asio::notification>();
   auto entered = std::make_shared<std::atomic_size_t>();
   auto second_calls = std::make_shared<std::atomic_size_t>();
   auto first_shutdown = gossipsub_test_shutdown{state.runtime, first, receiver, [release] { release->notify(); }};
   auto second_shutdown = gossipsub_test_shutdown{state.runtime, second, receiver, [release] { release->notify(); }};
   state.subscribe(first);
   state.subscribe(second);
   state.subscribe(receiver, [release, entered, second_calls](pubsub::event event)
       -> boost::asio::awaitable<pubsub::validation_result> {
      entered->fetch_add(1);
      if (std::ranges::equal(event.value.data, std::string_view{"a"})) {
         static_cast<void>(co_await release->async_wait(0));
      } else { second_calls->fetch_add(1); }
      co_return pubsub::validation_result::accept;
   });
   connect_ready(state, first, receiver);
   connect_ready(state, second, receiver);
   const auto first_message = state.publish(first, "a");
   BOOST_REQUIRE(state.wait([&] { return entered->load() == 1U; }));
   const auto second_message = state.publish(second, "b");
   const auto second_id = pubsub::codec::message_id(second_message);
   BOOST_REQUIRE(state.wait([&] { return receiver.metrics().backpressure_rejections >= 1U; }));
   BOOST_TEST(entered->load() == 1U);
   BOOST_TEST(second_calls->load() == 0U);
   BOOST_TEST(receiver.pubsub_snapshot().messages_delivered == 0U);
   release->notify();
   BOOST_REQUIRE(state.wait([&] { return receiver.pubsub_snapshot().messages_delivered == 2U; }));
   BOOST_REQUIRE(committed(state, receiver, pubsub::codec::message_id(first_message), pubsub::validation_result::accept));
   BOOST_REQUIRE(committed(state, receiver, second_id, pubsub::validation_result::accept));
   BOOST_TEST(entered->load() == 2U);
   BOOST_TEST(second_calls->load() == 1U);
   BOOST_TEST(want_count(state, second, receiver.local_peer(), second_id) >= 1U);
   BOOST_TEST(receiver.metrics().backpressure_rejections >= 1U);
   BOOST_TEST(receiver.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(score_of(receiver, first.local_peer()).behaviour_penalty == 0.0);
   BOOST_TEST(score_of(receiver, second.local_peer()).behaviour_penalty == 0.0);
   second_shutdown.join();
   first_shutdown.join();
   BOOST_TEST(receiver.metrics().stopped);
}

} // namespace forge::tests::p2p::gossipsub_validation_tests

BOOST_AUTO_TEST_CASE(p2p_gossipsub_nodes_deliver_signed_publish_over_negotiated_stream) {
   forge::tests::p2p::gossipsub_validation_tests::nodes_deliver_signed_publish_over_negotiated_stream();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_is_redelivered_after_bounded_cooldown) {
   forge::tests::p2p::gossipsub_validation_tests::retry_is_redelivered_after_bounded_cooldown();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_ignore_remains_terminal_during_history_window) {
   forge::tests::p2p::gossipsub_validation_tests::ignore_remains_terminal_during_history_window();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_in_progress_validation_is_not_replaced_after_cache_pressure) {
   forge::tests::p2p::gossipsub_validation_tests::in_progress_validation_is_not_replaced_after_cache_pressure();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_source_is_not_replaced_by_foreign_ihave) {
   forge::tests::p2p::gossipsub_validation_tests::retry_source_is_not_replaced_by_foreign_ihave();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_retry_stops_after_max_attempts_without_duplicate_history) {
   forge::tests::p2p::gossipsub_validation_tests::retry_stops_after_max_attempts_without_duplicate_history();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_unavailable_source_stops_retry_requests_after_limit) {
   forge::tests::p2p::gossipsub_validation_tests::unavailable_source_stops_retry_requests_after_limit();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_reject_remains_terminal_during_history_window) {
   forge::tests::p2p::gossipsub_validation_tests::reject_remains_terminal_during_history_window();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_validation_queue_limit_retries_excess_without_penalizing_peer) {
   forge::tests::p2p::gossipsub_validation_tests::validation_queue_limit_retries_excess_without_penalizing_peer();
}
