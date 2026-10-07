module;

#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.yamux.options;

#include "gossipsub_test_receipts.hxx"
#include "gossipsub_test_shutdown.hxx"
#include "pubsub_router_fixture.hxx"
#include "gossipsub_validation_tests.hxx"
#include "gossipsub_outbound_tests.hxx"

namespace forge::tests::p2p::gossipsub_outbound_tests {
using namespace forge::net::p2p;
using namespace std::chrono_literals;
using namespace forge::tests::p2p::gossipsub_validation_tests;
using forge::tests::p2p::make_identity_fixture;

void native_trace::bind(node& owner) { _owner = &owner; }

void native_trace::capture(const pubsub::trace_event& event) {
   controls.capture(event);
   try {
      if (event.session_id == 0 || event.stream_id < 0 || event.generation == 0 ||
          event.protocol != builtins::meshsub_v11) {
         throw std::runtime_error{"native outbound receipt lacks its negotiated owner"};
      }
      // Snapshot reentry must work without waiting on the executor or taking a node lock across the gate.
      if (event.kind == pubsub::trace_kind::validation_committed) {
         static_cast<void>(_owner->pubsub_snapshot());
         static_cast<void>(_owner->diagnostics());
      }
      auto receipt = pubsub_router_receipt{.kind = event.kind, .peer = event.peer, .protocol = event.protocol,
          .session = event.session_id, .stream = event.stream_id, .generation = event.generation,
          .observed = std::chrono::steady_clock::now(), .result = event.result,
          .id = {event.message_id.begin(), event.message_id.end()},
          .data = {event.data.begin(), event.data.end()},
          .frame = {event.framed_rpc.begin(), event.framed_rpc.end()}};
      auto lock = std::unique_lock{_mutex};
      if (_receipts.size() >= 4096U) { throw std::runtime_error{"native outbound receipt bound exceeded"}; }
      _receipts.push_back(std::move(receipt));
      if (_released || _held || !_gate_peer || event.kind != pubsub::trace_kind::validation_committed ||
          event.peer != *_gate_peer || event.author != _gate_peer || event.subject != _gate_topic ||
          event.result != pubsub::validation_result::accept || !std::ranges::equal(event.data, _gate_data)) {
         return;
      }
      const auto identified = pubsub::message{.from = event.author,
          .seqno = {event.seqno.begin(), event.seqno.end()}, .subject = event.subject};
      if (!std::ranges::equal(event.message_id, pubsub::codec::message_id(identified))) {
         throw std::runtime_error{"validation gate does not identify the accepted signed warmup"};
      }
      _gate_id.assign(event.message_id.begin(), event.message_id.end());
      _reentered = true;
      _held = true;
      _changed.notify_all();
      if (!_changed.wait_for(lock, 20s, [&] { return _released; })) {
         _released = true;
         std::fprintf(stderr, "FATAL: native GossipSub source=validation_callback_gate stage=%.*s "
                              "session=%llu stream=%lld generation=%llu receipts=%zu\n",
             static_cast<int>(_stage.size()), _stage.data(),
             static_cast<unsigned long long>(event.session_id), static_cast<long long>(event.stream_id),
             static_cast<unsigned long long>(event.generation), _receipts.size());
         std::fflush(stderr);
         lock.unlock();
         gossipsub_test_shutdown::fail_closed();
      }
   } catch (...) {
      const auto lock = std::scoped_lock{_mutex};
      if (!_failure) { _failure = std::current_exception(); }
      _released = true;
      _changed.notify_all();
   }
}

void native_trace::arm(const peer_id& peer, const pubsub::topic& subject, std::vector<std::uint8_t> data) {
   const auto lock = std::scoped_lock{_mutex};
   _gate_peer = peer;
   _gate_topic = subject;
   _gate_data = std::move(data);
}

void native_trace::release() noexcept {
   const auto lock = std::scoped_lock{_mutex};
   _released = true;
   _changed.notify_all();
}

void native_trace::stage(std::string_view value) {
   const auto lock = std::scoped_lock{_mutex};
   _stage = value;
}

bool native_trace::held(const std::vector<std::uint8_t>& id) const {
   const auto lock = std::scoped_lock{_mutex};
   if (_failure) { std::rethrow_exception(_failure); }
   return _held && !_released && _reentered && _gate_id == id;
}

std::vector<pubsub_router_receipt> native_trace::receipts() const {
   const auto lock = std::scoped_lock{_mutex};
   if (_failure) { std::rethrow_exception(_failure); }
   return _receipts;
}

bool native_trace::observed(pubsub::trace_kind kind, const std::vector<std::uint8_t>& id) const {
   return std::ranges::any_of(receipts(), [&](const auto& event) {
      return event.kind == kind && event.id == id && event.result == pubsub::validation_result::accept;
   });
}

std::size_t native_trace::messages(pubsub::trace_kind kind, const peer_id& peer,
                                   const pubsub::topic& subject) const {
   auto count = std::size_t{};
   for (const auto& event : receipts()) {
      if (event.kind != kind || event.peer != peer || event.frame.empty()) { continue; }
      const auto rpc = pubsub::codec::decode(event.frame);
      count += static_cast<std::size_t>(std::ranges::count_if(rpc.messages, [&](const auto& value) {
         return value.subject == subject;
      }));
   }
   return count;
}

namespace {

void report_native_owner(const node& owner, std::string_view label) noexcept {
   try {
      const auto metrics = owner.metrics();
      const auto resources = owner.diagnostics().resources;
      std::fprintf(stderr, "GOSSIPSUB_OWNER owner=%.*s stopped=%d sessions=%zu streams_memory=%zu "
                           "system_memory=%zu inbound_streams=%zu outbound_streams=%zu "
                           "inbound_connections=%zu outbound_connections=%zu active_dials=%zu\n",
          static_cast<int>(label.size()), label.data(), static_cast<int>(metrics.stopped),
          static_cast<std::size_t>(metrics.active_sessions), static_cast<std::size_t>(resources.streams.memory),
          static_cast<std::size_t>(resources.system.memory),
          static_cast<std::size_t>(resources.system.inbound_streams),
          static_cast<std::size_t>(resources.system.outbound_streams),
          static_cast<std::size_t>(resources.system.inbound_connections),
          static_cast<std::size_t>(resources.system.outbound_connections),
          static_cast<std::size_t>(resources.active_dials));
   } catch (const std::exception& error) {
      std::fprintf(stderr, "GOSSIPSUB_OWNER owner=%.*s snapshot_exception=%s\n",
                   static_cast<int>(label.size()), label.data(), error.what());
   } catch (...) {
      std::fprintf(stderr, "GOSSIPSUB_OWNER owner=%.*s snapshot_exception=nonstandard\n",
                   static_cast<int>(label.size()), label.data());
   }
   std::fflush(stderr);
}

[[noreturn]] void fail_native_wait(std::string_view label, const std::shared_ptr<native_trace>& gate) {
   if (gate) { gate->release(); }
   std::fprintf(stderr, "FATAL: native GossipSub source=bounded_future stage=%.*s\n",
                static_cast<int>(label.size()), label.data());
   gossipsub_test_shutdown::fail_closed();
}

template <typename T>
void require_native_ready(std::future<T>& future, std::string_view label,
                          const std::shared_ptr<native_trace>& gate = {},
                          std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + 5s) {
   if (future.wait_until(deadline) == std::future_status::ready) { return; }
   fail_native_wait(label, gate);
}

template <typename T>
T run_native(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation, std::string_view label,
             const std::shared_ptr<native_trace>& gate = {}) {
   auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   require_native_ready(future, label, gate);
   return future.get();
}

node::options native_node_options(std::string_view name, const pubsub::topic& subject,
                                   const std::shared_ptr<native_trace>& trace) {
   auto options = pubsub_options_for(make_identity_fixture(name));
   options.allow_insecure_test_mode = false;
   options.limits.pubsub = native_options(subject);
   options.limits.pubsub.tracer = [trace](const auto& event) { trace->capture(event); };
   return options;
}

endpoint native_listen(node& owner, forge::asio::runtime& runtime) {
   run_native(runtime, owner.async_listen(parse_endpoint("/ip4/127.0.0.1/tcp/0")), "native TCP listen");
   const auto endpoint = owner.local_endpoints().front();
   BOOST_REQUIRE(endpoint.is_direct_tcp());
   return endpoint;
}

bool native_identified(const node& owner, const peer_id& remote) {
   return authenticated_gossipsub_peer(owner, remote) &&
       std::ranges::any_of(owner.diagnostics().sessions, [&](const auto& row) {
          return row.remote_peer == remote && !row.closed && row.identify_state == identify::state::identified &&
                 row.muxer.value == "/yamux/1.0.0" && row.yamux_role.has_value() &&
                 (row.authentication == peer_authentication::libp2p_tls ||
                  row.authentication == peer_authentication::noise) && row.remote_endpoint &&
                 row.remote_endpoint->is_direct_tcp();
       });
}

void native_connect(node& source, node& target, forge::asio::runtime& runtime) {
   const auto address = target.local_endpoints().front();
   source.peers().learn_endpoint(target.local_peer(), address,
       capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   static_cast<void>(run_native(runtime, source.async_connect(address,
       node::connect_options{.expected_peer = target.local_peer(), .allow_relay = false, .timeout = 3s,
                             .allow_hole_punch = false}), "native authenticated TCP connect"));
   wait_for_gossipsub(runtime, "native authenticated TCP/Yamux Identify", [&] {
      return native_identified(source, target.local_peer()) && native_identified(target, source.local_peer());
   });
}

void native_subscribe(node& owner, forge::asio::runtime& runtime, const pubsub::topic& subject) {
   static_cast<void>(run_native(runtime,
       owner.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }), "native SUBSCRIBE"));
}

void native_mesh(node& first, node& second, forge::asio::runtime& first_runtime,
                  forge::asio::runtime& second_runtime, const pubsub::topic& subject,
                  const native_trace& first_trace, const native_trace& second_trace) {
   native_subscribe(first, first_runtime, subject);
   native_subscribe(second, second_runtime, subject);
   wait_for_gossipsub(first_runtime, "native bidirectional SUBSCRIBE and heartbeat mesh", [&] {
      return first_trace.controls.subscribed(second.local_peer(), subject) &&
             second_trace.controls.subscribed(first.local_peer(), subject) &&
             gossipsub_mesh_peer(first, second.local_peer(), subject) &&
             gossipsub_mesh_peer(second, first.local_peer(), subject);
   });
   BOOST_REQUIRE(native_identified(first, second.local_peer()));
   BOOST_REQUIRE(native_identified(second, first.local_peer()));
}

pubsub::message native_publish(node& owner, forge::asio::runtime& runtime, const pubsub::topic& subject,
                                std::vector<std::uint8_t> data, const std::shared_ptr<native_trace>& gate = {}) {
   auto value = run_native(runtime, owner.async_publish(subject, std::move(data)), "native signed publication", gate);
   BOOST_REQUIRE(value.from.has_value());
   BOOST_TEST(value.from->to_string() == owner.local_peer().to_string());
   BOOST_REQUIRE(pubsub::codec::verify_message(value));
   return value;
}

void require_wire_message(const native_trace& trace, pubsub::trace_kind kind, const peer_id& immediate,
                           const pubsub::message& expected) {
   auto count = std::size_t{};
   for (const auto& event : trace.receipts()) {
      if (event.kind != kind || event.peer != immediate || event.frame.empty()) { continue; }
      for (const auto& value : pubsub::codec::decode(event.frame).messages) {
         if (pubsub::codec::message_id(value) != pubsub::codec::message_id(expected)) { continue; }
         ++count;
         BOOST_REQUIRE(pubsub::codec::verify_message(value));
         BOOST_CHECK(value.from == expected.from);
         BOOST_TEST(value.subject.value == expected.subject.value);
         BOOST_TEST(value.data == expected.data, boost::test_tools::per_element());
         BOOST_TEST(value.seqno == expected.seqno, boost::test_tools::per_element());
         BOOST_TEST(value.signature == expected.signature, boost::test_tools::per_element());
         BOOST_TEST(value.key == expected.key, boost::test_tools::per_element());
      }
   }
   BOOST_TEST(count == 1U);
}

void register_native_echo(node& owner) {
   owner.register_protocol_handler(builtins::echo,
       [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          const auto payload = co_await incoming.stream.async_read_frame();
          co_await incoming.stream.async_write_frame(payload);
          co_await incoming.stream.async_close();
       });
}

boost::asio::awaitable<std::vector<std::uint8_t>> native_echo(node& source, peer_id remote) {
   auto stream = co_await source.async_open_protocol_stream(remote, builtins::echo,
       node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
   const auto payload = std::vector<std::uint8_t>{'h', 'e', 'a', 'l', 't', 'h', 'y'};
   co_await stream.async_write_frame(payload);
   auto reply = co_await stream.async_read_frame();
   co_await stream.async_close();
   co_return reply;
}

void require_rejection(node& publisher, forge::asio::runtime& runtime, const pubsub::topic& subject,
                        std::vector<std::uint8_t> data, const std::shared_ptr<native_trace>& gate = {}) {
   try {
      static_cast<void>(run_native(runtime, publisher.async_publish(subject, std::move(data)),
                                   "typed outbound quota rejection", gate));
      BOOST_FAIL("GossipSub outbound byte limit must reject before native write");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(forge::net::p2p::exceptions::code::backpressure_rejected));
   }
}

void require_resources_released(const node& owner) {
   const auto resources = owner.diagnostics().resources;
   BOOST_TEST(resources.system.memory == 0U);
   BOOST_TEST(resources.streams.memory == 0U);
   BOOST_TEST(resources.system.inbound_streams == 0U);
   BOOST_TEST(resources.system.outbound_streams == 0U);
   BOOST_TEST(resources.system.inbound_connections == 0U);
   BOOST_TEST(resources.system.outbound_connections == 0U);
   BOOST_TEST(resources.active_dials == 0U);
}

void require_clean_trace(const node& owner, const native_trace& trace) {
   static_cast<void>(trace.receipts());
   BOOST_TEST(owner.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(owner.pubsub_snapshot().trace_failures == 0U);
}

void require_failure_state_unchanged(const node& owner, const peer_store::record& before) {
   const auto after = owner.peers().find(before.peer);
   BOOST_REQUIRE(after.has_value());
   BOOST_TEST(after->failures == before.failures);
   BOOST_REQUIRE_EQUAL(after->endpoints.size(), before.endpoints.size());
   for (auto index = std::size_t{}; index < before.endpoints.size(); ++index) {
      BOOST_TEST(after->endpoints[index].failures == before.endpoints[index].failures);
      BOOST_CHECK(after->endpoints[index].backoff_until == before.endpoints[index].backoff_until);
   }
}

void blocked_publication_regression(bool global) {
   constexpr auto queue_limit = std::size_t{512 * 1024};
   constexpr auto payload_size = std::size_t{384 * 1024};
   static_assert(payload_size > forge::net::yamux::options{}.initial_window);
   static_assert(payload_size < queue_limit && 2 * payload_size > queue_limit);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto receiver_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto healthy_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   const auto subject_a = pubsub::topic{.value = global ? "forge.limit.global.a" : "forge.limit.aggregate"};
   const auto subject_b = pubsub::topic{.value = "forge.limit.global.b"};
   auto publisher_trace = std::make_shared<native_trace>();
   auto receiver_trace = std::make_shared<native_trace>();
   auto healthy_trace = std::make_shared<native_trace>();
   auto publisher_options = native_node_options("quota-publisher", subject_a, publisher_trace);
   publisher_options.limits.pubsub.limits.max_outbound_queue_bytes = queue_limit;
   BOOST_REQUIRE(payload_size < publisher_options.limits.pubsub.limits.max_data_size);
   BOOST_REQUIRE(queue_limit < publisher_options.limits.pubsub.limits.max_message_size);
   BOOST_REQUIRE(queue_limit < publisher_options.limits.pubsub.limits.max_rpc_size);
   if (global) { publisher_options.limits.pubsub.scoring->topics.emplace(subject_b, pubsub::topic_score_params{}); }
   auto publisher = node{runtime, std::move(publisher_options)};
   auto receiver = node{receiver_runtime, native_node_options("quota-receiver", subject_a, receiver_trace)};
   auto healthy = node{healthy_runtime, native_node_options("quota-healthy", subject_b, healthy_trace)};
   publisher_trace->bind(publisher);
   receiver_trace->bind(receiver);
   healthy_trace->bind(healthy);
   auto first = std::future<pubsub::message>{};
   auto stage = std::string_view{"quota_setup"};
   const auto checkpoint = [&](std::string_view label) {
      stage = label;
      receiver_trace->stage(label);
   };
   const auto report_failure = [&](std::string_view label) {
      std::fprintf(stderr, "GOSSIPSUB_FAILURE test=%s source=%.*s stage=%.*s "
                           "publication_valid=%d publication_ready=%d\n",
          global ? "global_quota" : "per_peer_quota", static_cast<int>(label.size()), label.data(),
          static_cast<int>(stage.size()), stage.data(),
          static_cast<int>(first.valid()),
          static_cast<int>(first.valid() && first.wait_for(0ms) == std::future_status::ready));
      report_native_owner(publisher, "publisher");
      report_native_owner(receiver, "receiver");
      report_native_owner(healthy, "healthy");
   };
   auto healthy_shutdown = gossipsub_test_shutdown{runtime, publisher, healthy,
       [&] { checkpoint("healthy_cleanup.release"); receiver_trace->release(); }};
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, receiver,
       [&] {
          checkpoint("publication_cleanup.cancel.before");
          try { publisher.stop(); }
          catch (...) { receiver_trace->release(); report_failure("publication_cleanup.cancel.exception"); throw; }
          receiver_trace->release();
          checkpoint("publication_cleanup.release.after");
       },
       [&](auto deadline) {
          checkpoint("publication_cleanup.join.before");
          if (first.valid() && first.wait_until(deadline) != std::future_status::ready) {
             receiver_trace->release();
             report_failure("publication_cleanup.join.timeout");
             fail_native_wait("held native publication cleanup", receiver_trace);
          }
          checkpoint("publication_cleanup.join.after");
       }, [&] { checkpoint("publication_cleanup.cancel.retry"); publisher.stop(); }};
   static_cast<void>(native_listen(publisher, runtime));
   static_cast<void>(native_listen(receiver, receiver_runtime));
   static_cast<void>(native_listen(healthy, healthy_runtime));
   register_native_echo(healthy);
   native_connect(publisher, receiver, runtime);
   native_connect(publisher, healthy, runtime);
   native_mesh(publisher, receiver, runtime, receiver_runtime, subject_a, *publisher_trace, *receiver_trace);
   if (global) {
      native_mesh(publisher, healthy, runtime, healthy_runtime, subject_b, *publisher_trace, *healthy_trace);
      BOOST_TEST(!publisher_trace->controls.subscribed(healthy.local_peer(), subject_a));
      BOOST_TEST(!publisher_trace->controls.subscribed(receiver.local_peer(), subject_b));
      // B can receive a full publication before A occupies the global budget; its per-peer lane is free.
      const auto preflight = native_publish(publisher, runtime, subject_b,
                                            std::vector<std::uint8_t>(payload_size, 0x40U));
      const auto id = pubsub::codec::message_id(preflight);
      wait_for_gossipsub(runtime, "B preflight committed and delivered", [&] {
         return healthy_trace->observed(pubsub::trace_kind::delivery, id) &&
                publisher.diagnostics().resources.streams.memory == 0U;
      });
      require_wire_message(*healthy_trace, pubsub::trace_kind::rpc_read, publisher.local_peer(), preflight);
   }
   wait_for_gossipsub(runtime, "native quota setup resource drain", [&] {
      const auto resources = publisher.diagnostics().resources;
      return resources.active_dials == 0U && resources.streams.memory == 0U;
   });
   const auto peer_a_before = publisher.peers().find(receiver.local_peer());
   const auto peer_b_before = publisher.peers().find(healthy.local_peer());
   BOOST_REQUIRE(peer_a_before.has_value());
   BOOST_REQUIRE(peer_b_before.has_value());
   BOOST_REQUIRE(!peer_a_before->endpoints.empty());
   const auto failures_before = peer_a_before->failures;

   receiver_trace->arm(publisher.local_peer(), subject_a, {0x21U});
   receiver_trace->stage("signed_warmup");
   const auto warmup = native_publish(publisher, runtime, subject_a, {0x21U}, receiver_trace);
   const auto warmup_id = pubsub::codec::message_id(warmup);
   wait_for_gossipsub(runtime, "exact accepted warmup blocks receiver's sole worker", [&] {
      return receiver_trace->held(warmup_id);
   });
   require_wire_message(*receiver_trace, pubsub::trace_kind::rpc_read, publisher.local_peer(), warmup);
   require_wire_message(*publisher_trace, pubsub::trace_kind::rpc_write, receiver.local_peer(), warmup);
   const auto opened_streams_before = publisher.metrics().protocol_streams_opened;
   const auto a_writes = publisher_trace->messages(pubsub::trace_kind::rpc_write, receiver.local_peer(), subject_a);
   const auto b_writes = publisher_trace->messages(pubsub::trace_kind::rpc_write, healthy.local_peer(), subject_b);
   const auto b_reads = healthy_trace->messages(pubsub::trace_kind::rpc_read, publisher.local_peer(), subject_b);
   first = boost::asio::co_spawn(runtime.context(),
       publisher.async_publish(subject_a, std::vector<std::uint8_t>(payload_size, 0x41U)), boost::asio::use_future);
   receiver_trace->stage("held_native_write");
   wait_for_gossipsub(runtime, "384 KiB publication owns native write memory beyond Yamux credit", [&] {
      return publisher.diagnostics().resources.streams.memory >= payload_size;
   });
   BOOST_REQUIRE(first.wait_for(50ms) != std::future_status::ready);
   BOOST_REQUIRE(receiver_trace->held(warmup_id));
   BOOST_TEST(publisher.metrics().protocol_streams_opened == opened_streams_before);
   BOOST_TEST(publisher_trace->messages(pubsub::trace_kind::rpc_write, receiver.local_peer(), subject_a) == a_writes);
   const auto rejections_before = publisher.metrics().backpressure_rejections;
   receiver_trace->stage("second_publication_rejection");
   require_rejection(publisher, runtime, global ? subject_b : subject_a,
                     std::vector<std::uint8_t>(payload_size, 0x42U), receiver_trace);
   BOOST_TEST(publisher.metrics().backpressure_rejections > rejections_before);
   BOOST_TEST(publisher.metrics().backpressure_rejections >= 1U);
   BOOST_TEST(!publisher.metrics().stopped);
   BOOST_REQUIRE(first.wait_for(0ms) != std::future_status::ready);
   BOOST_REQUIRE(receiver_trace->held(warmup_id));
   BOOST_TEST(publisher.diagnostics().resources.streams.memory >= payload_size);
   BOOST_TEST(publisher_trace->messages(pubsub::trace_kind::rpc_write, receiver.local_peer(), subject_a) == a_writes);
   BOOST_TEST(publisher_trace->messages(pubsub::trace_kind::rpc_write, healthy.local_peer(), subject_b) == b_writes);
   BOOST_TEST(healthy_trace->messages(pubsub::trace_kind::rpc_read, publisher.local_peer(), subject_b) == b_reads);
   require_failure_state_unchanged(publisher, *peer_a_before);
   require_failure_state_unchanged(publisher, *peer_b_before);
   BOOST_TEST(publisher.peers().find(receiver.local_peer())->failures == failures_before);

   receiver_trace->stage("healthy_echo");
   const auto echo = run_native(runtime, native_echo(publisher, healthy.local_peer()),
                                 "Echo stays healthy while receiver runtime is held", receiver_trace);
   BOOST_TEST(echo == std::vector<std::uint8_t>({'h', 'e', 'a', 'l', 't', 'h', 'y'}),
              boost::test_tools::per_element());
   BOOST_REQUIRE(first.wait_for(0ms) != std::future_status::ready);
   BOOST_REQUIRE(receiver_trace->held(warmup_id));
   const auto metrics_before_stop = publisher.metrics();
   const auto peer_before_stop = publisher.peers().find(receiver.local_peer());
   BOOST_REQUIRE(peer_before_stop.has_value());
   BOOST_REQUIRE(!peer_before_stop->endpoints.empty());

   // Synchronous node cancellation fixes the publication's terminal result; release the callback before any join.
   try {
      checkpoint("publisher.stop.before");
      publisher.stop();
      checkpoint("publisher.stop.after");
      receiver_trace->release();
      checkpoint("receiver.release.after");
      shutdown.join();
      checkpoint("shutdown.join.after");
   } catch (const std::exception& error) {
      receiver_trace->release();
      std::fprintf(stderr, "GOSSIPSUB_EXCEPTION source=publication_stop_join what=%s\n", error.what());
      report_failure("publication_stop_join.exception");
      throw;
   } catch (...) {
      receiver_trace->release();
      std::fputs("GOSSIPSUB_EXCEPTION source=publication_stop_join what=nonstandard\n", stderr);
      report_failure("publication_stop_join.exception");
      throw;
   }
   require_native_ready(first, "canceled native publication", receiver_trace);
   BOOST_CHECK_THROW(static_cast<void>(first.get()), forge::exceptions::base);
   const auto metrics_after_stop = publisher.metrics();
   require_failure_state_unchanged(publisher, *peer_before_stop);
   require_failure_state_unchanged(publisher, *peer_a_before);
   require_failure_state_unchanged(publisher, *peer_b_before);
   BOOST_TEST(metrics_after_stop.direct_failures == metrics_before_stop.direct_failures);
   wait_on_runtime(runtime, 100ms, "post-stop GossipSub native write drain");
   require_failure_state_unchanged(publisher, *peer_before_stop);
   BOOST_TEST(publisher.metrics().direct_failures == metrics_after_stop.direct_failures);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(receiver.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(healthy.metrics().pubsub_invalid_messages == 0U);
   require_resources_released(publisher);
   require_resources_released(receiver);
   healthy_shutdown.join();
   require_resources_released(healthy);
   require_clean_trace(publisher, *publisher_trace);
   require_clean_trace(receiver, *receiver_trace);
   require_clean_trace(healthy, *healthy_trace);
}

} // namespace

void rejects_publish_after_cached_stream_shutdown() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto subject = pubsub::topic{.value = "forge.pubsub.shutdown"};
   auto publisher_trace = std::make_shared<native_trace>();
   auto subscriber_trace = std::make_shared<native_trace>();
   auto publisher = node{runtime, native_node_options("cached-shutdown-publisher", subject, publisher_trace)};
   auto subscriber = node{runtime, native_node_options("cached-shutdown-subscriber", subject, subscriber_trace)};
   publisher_trace->bind(publisher);
   subscriber_trace->bind(subscriber);
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, subscriber};
   static_cast<void>(native_listen(publisher, runtime));
   static_cast<void>(native_listen(subscriber, runtime));
   native_connect(publisher, subscriber, runtime);
   native_mesh(publisher, subscriber, runtime, runtime, subject, *publisher_trace, *subscriber_trace);
   const auto warmup = native_publish(publisher, runtime, subject, {0x42U});
   const auto id = pubsub::codec::message_id(warmup);
   wait_for_gossipsub(runtime, "cached stream signed warmup delivered", [&] {
      return subscriber_trace->observed(pubsub::trace_kind::validation_committed, id) &&
             subscriber_trace->observed(pubsub::trace_kind::delivery, id);
   });
   require_wire_message(*subscriber_trace, pubsub::trace_kind::rpc_read, publisher.local_peer(), warmup);
   const auto messages_before = publisher_trace->messages(pubsub::trace_kind::rpc_write, subscriber.local_peer(), subject);
   const auto reads_before = subscriber_trace->messages(pubsub::trace_kind::rpc_read, publisher.local_peer(), subject);
   BOOST_REQUIRE(publisher_trace->controls.written_generation(subscriber.local_peer(), subject) != 0U);
   run_native(runtime, publisher.async_stop(), "cached GossipSub stream shutdown");
   BOOST_TEST(publisher.metrics().stopped);
   try {
      static_cast<void>(run_native(runtime, publisher.async_publish(subject, {0x43U}), "closed publish rejection"));
      BOOST_FAIL("GossipSub publish should reject after clean shutdown");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::p2p::exceptions::code_of(error).has_value());
      BOOST_TEST(static_cast<int>(*forge::net::p2p::exceptions::code_of(error)) ==
                 static_cast<int>(forge::net::p2p::exceptions::code::closed));
   }
   wait_for_gossipsub(runtime, "orderly GossipSub session close", [&] {
      return subscriber.metrics().active_sessions == 0U;
   });
   BOOST_TEST(publisher_trace->messages(pubsub::trace_kind::rpc_write, subscriber.local_peer(), subject) == messages_before);
   BOOST_TEST(subscriber_trace->messages(pubsub::trace_kind::rpc_read, publisher.local_peer(), subject) == reads_before);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);
   shutdown.join();
   require_resources_released(publisher);
   require_resources_released(subscriber);
   require_clean_trace(publisher, *publisher_trace);
   require_clean_trace(subscriber, *subscriber_trace);
}

void forwards_between_subscribed_peers() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   const auto subject = pubsub::topic{.value = "forge.mesh"};
   auto publisher_trace = std::make_shared<native_trace>();
   auto hub_trace = std::make_shared<native_trace>();
   auto subscriber_trace = std::make_shared<native_trace>();
   auto publisher = node{runtime, native_node_options("forward-publisher", subject, publisher_trace)};
   auto hub = node{runtime, native_node_options("forward-hub", subject, hub_trace)};
   auto subscriber = node{runtime, native_node_options("forward-sink", subject, subscriber_trace)};
   publisher_trace->bind(publisher);
   hub_trace->bind(hub);
   subscriber_trace->bind(subscriber);
   auto first_shutdown = gossipsub_test_shutdown{runtime, publisher, hub};
   auto second_shutdown = gossipsub_test_shutdown{runtime, hub, subscriber};
   static_cast<void>(native_listen(publisher, runtime));
   static_cast<void>(native_listen(hub, runtime));
   static_cast<void>(native_listen(subscriber, runtime));
   native_connect(publisher, hub, runtime);
   native_connect(hub, subscriber, runtime);
   native_mesh(publisher, hub, runtime, runtime, subject, *publisher_trace, *hub_trace);
   native_mesh(hub, subscriber, runtime, runtime, subject, *hub_trace, *subscriber_trace);
   BOOST_REQUIRE_EQUAL(publisher.diagnostics().sessions.size(), 1U);
   BOOST_REQUIRE_EQUAL(hub.diagnostics().sessions.size(), 2U);
   BOOST_REQUIRE_EQUAL(subscriber.diagnostics().sessions.size(), 1U);
   BOOST_TEST(!publisher.peers().find(subscriber.local_peer()).has_value());
   BOOST_TEST(!subscriber.peers().find(publisher.local_peer()).has_value());
   BOOST_REQUIRE(hub.pubsub_snapshot().mesh_edges >= 1U);
   const auto published = native_publish(publisher, runtime, subject, {'m', 'e', 's', 'h'});
   const auto id = pubsub::codec::message_id(published);
   wait_for_gossipsub(runtime, "natural publisher -> hub -> sink delivery", [&] {
      return hub_trace->observed(pubsub::trace_kind::validation_committed, id) &&
             subscriber_trace->observed(pubsub::trace_kind::validation_committed, id) &&
             subscriber_trace->observed(pubsub::trace_kind::delivery, id);
   });
   require_wire_message(*hub_trace, pubsub::trace_kind::rpc_read, publisher.local_peer(), published);
   require_wire_message(*hub_trace, pubsub::trace_kind::rpc_write, subscriber.local_peer(), published);
   require_wire_message(*subscriber_trace, pubsub::trace_kind::rpc_read, hub.local_peer(), published);
   BOOST_TEST(hub.pubsub_snapshot().mesh_edges >= 1U);
   BOOST_TEST(subscriber.pubsub_snapshot().messages_delivered >= 1U);
   BOOST_TEST(hub.metrics().pubsub_messages_delivered == 1U);
   BOOST_TEST(subscriber.metrics().pubsub_messages_delivered == 1U);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(hub.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(!publisher.peers().find(subscriber.local_peer()).has_value());
   BOOST_TEST(!subscriber.peers().find(publisher.local_peer()).has_value());
   second_shutdown.join();
   first_shutdown.join();
   require_clean_trace(publisher, *publisher_trace);
   require_clean_trace(hub, *hub_trace);
   require_clean_trace(subscriber, *subscriber_trace);
}

void outbound_byte_limit_rejects_publish_without_stopping_node() {
   constexpr auto queue_limit = std::size_t{64};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto subject = pubsub::topic{.value = "forge.limit"};
   auto publisher_trace = std::make_shared<native_trace>();
   auto subscriber_trace = std::make_shared<native_trace>();
   auto options = native_node_options("quota-rejection-publisher", subject, publisher_trace);
   options.limits.pubsub.limits.max_outbound_queue_bytes = queue_limit;
   auto publisher = node{runtime, std::move(options)};
   auto subscriber = node{runtime, native_node_options("quota-rejection-subscriber", subject, subscriber_trace)};
   publisher_trace->bind(publisher);
   subscriber_trace->bind(subscriber);
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, subscriber};
   // With no candidate yet, obtain the actual signed RPC shape; even the original four bytes exceed quota.
   const auto probe = native_publish(publisher, runtime, subject, {'o', 'v', 'e', 'r'});
   BOOST_REQUIRE(pubsub::codec::encode(pubsub::rpc{.messages = {probe}}).size() > queue_limit);
   BOOST_REQUIRE(probe.data.size() < queue_limit);
   static_cast<void>(native_listen(publisher, runtime));
   static_cast<void>(native_listen(subscriber, runtime));
   register_native_echo(subscriber);
   native_connect(publisher, subscriber, runtime);
   native_mesh(publisher, subscriber, runtime, runtime, subject, *publisher_trace, *subscriber_trace);
   wait_for_gossipsub(runtime, "actual subscribed quota recipient", [&] {
      return publisher_trace->controls.subscribed(subscriber.local_peer(), subject) &&
             publisher.diagnostics().resources.streams.memory == 0U;
   });
   const auto resources_before = publisher.diagnostics().resources;
   const auto metrics_before = publisher.metrics();
   const auto peer_before = publisher.peers().find(subscriber.local_peer());
   BOOST_REQUIRE(peer_before.has_value());
   require_rejection(publisher, runtime, subject, {'o', 'v', 'e', 'r'});
   BOOST_TEST(publisher.metrics().backpressure_rejections >= 1U);
   BOOST_TEST(!publisher.metrics().stopped);
   BOOST_TEST(publisher_trace->messages(pubsub::trace_kind::rpc_write, subscriber.local_peer(), subject) == 0U);
   BOOST_TEST(subscriber_trace->messages(pubsub::trace_kind::rpc_read, publisher.local_peer(), subject) == 0U);
   const auto resources_after = publisher.diagnostics().resources;
   BOOST_TEST(resources_after.streams.memory == resources_before.streams.memory);
   BOOST_TEST(resources_after.system.outbound_streams == resources_before.system.outbound_streams);
   BOOST_TEST(resources_after.active_dials == resources_before.active_dials);
   BOOST_TEST(publisher.metrics().protocol_streams_opened == metrics_before.protocol_streams_opened);
   BOOST_TEST(publisher.metrics().direct_failures == metrics_before.direct_failures);
   require_failure_state_unchanged(publisher, *peer_before);
   const auto echo = run_native(runtime, native_echo(publisher, subscriber.local_peer()), "Echo after quota rejection");
   BOOST_TEST(echo == std::vector<std::uint8_t>({'h', 'e', 'a', 'l', 't', 'h', 'y'}),
              boost::test_tools::per_element());
   BOOST_TEST(!publisher.metrics().stopped);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);
   shutdown.join();
   require_resources_released(publisher);
   require_resources_released(subscriber);
   require_clean_trace(publisher, *publisher_trace);
   require_clean_trace(subscriber, *subscriber_trace);
}

void outbound_byte_limit_counts_blocked_active_publication_per_peer() { blocked_publication_regression(false); }
void outbound_byte_limit_is_global_across_peers() { blocked_publication_regression(true); }

void singleflights_cold_subscriptions_then_serializes_concurrent_publishes() {
   constexpr auto publish_count = std::size_t{24};
   constexpr auto payload_size = std::size_t{64 * 1024};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 8}};
   const auto publisher_identity = make_identity_fixture("pubsub-concurrent-publisher");
   const auto subscriber_identity = make_identity_fixture("pubsub-concurrent-subscriber");
   const auto pubsub_capabilities = capability_set{.bits = capabilities::direct_quic | capabilities::pubsub};
   auto receipts = std::make_shared<gossipsub_test_receipts>();
   auto publisher_options = options_for(publisher_identity, pubsub_capabilities);
   auto subscriber_options = options_for(subscriber_identity, pubsub_capabilities);
   publisher_options.allow_insecure_test_mode = false;
   subscriber_options.allow_insecure_test_mode = false;
   publisher_options.limits.pubsub.tracer = [receipts](const auto& event) { receipts->capture(event); };
   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto subscriptions = std::vector<std::future<pubsub::subscription>>{};
   auto publishes = std::vector<std::future<pubsub::message>>{};
   subscriptions.reserve(publish_count);
   publishes.reserve(publish_count);
   auto ready = std::make_shared<std::atomic_size_t>();
   auto start = std::make_shared<std::atomic_bool>();
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, subscriber,
       [start] { start->store(true, std::memory_order_release); },
       [&](std::chrono::steady_clock::time_point deadline) {
          for (auto& subscription : subscriptions) {
             if (subscription.valid() && subscription.wait_until(deadline) != std::future_status::ready) {
                throw std::runtime_error{"cold GossipSub subscription did not join within its test budget"};
             }
          }
          for (auto& publish : publishes) {
             if (publish.valid() && publish.wait_until(deadline) != std::future_status::ready) {
                throw std::runtime_error{"concurrent GossipSub publication did not join within its test budget"};
             }
          }
       }};
   static_cast<void>(listen_tcp(publisher, runtime));
   const auto subscriber_endpoint = listen_tcp(subscriber, runtime);
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});

   auto delivered = std::make_shared<delivery_state>();
   const auto subject = pubsub::topic{.value = "forge.pubsub.concurrent"};
   forge::asio::blocking::run(
       runtime,
       subscriber.async_subscribe(
           subject, [delivered](pubsub::event event) mutable -> boost::asio::awaitable<pubsub::validation_result> {
              if (!event.value.data.empty()) {
                 {
                    auto lock = std::scoped_lock{delivered->mutex};
                    delivered->values.insert(event.value.data);
                 }
                 delivered->ready.notify_all();
              }
              co_return pubsub::validation_result::accept;
           }));
   // SUBSCRIBE uses the native PubSub connection singleflight even before any peer topic is known.
   BOOST_REQUIRE_EQUAL(publisher.metrics().sessions_opened, 0U);
   for (auto index = std::size_t{}; index < publish_count; ++index) {
      subscriptions.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&publisher, index, ready, start]() -> boost::asio::awaitable<pubsub::subscription> {
             ready->fetch_add(1U, std::memory_order_release);
             while (!start->load(std::memory_order_acquire)) {
                co_await boost::asio::post(boost::asio::use_awaitable);
             }
             co_return co_await publisher.async_subscribe(
                 pubsub::topic{.value = "forge.pubsub.cold." + std::to_string(index)},
                 [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
                    co_return pubsub::validation_result::accept;
                 });
          }, boost::asio::use_future));
   }
   wait_for_gossipsub(runtime, "cold GossipSub subscription start barrier", [&] {
      return ready->load(std::memory_order_acquire) == publish_count;
   });
   start->store(true, std::memory_order_release);
   for (auto& subscription : subscriptions) {
      BOOST_REQUIRE(subscription.wait_for(std::chrono::seconds{10}) == std::future_status::ready);
      static_cast<void>(subscription.get());
   }
   BOOST_TEST(publisher.metrics().sessions_opened == 1U);
   BOOST_TEST(publisher.metrics().active_sessions == 1U);
   forge::asio::blocking::run(
       runtime, publisher.async_subscribe(subject, [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
          co_return pubsub::validation_result::accept;
       }));
   wait_for_gossipsub(runtime, "concurrent publish authenticated subscription and mesh", [&] {
      return authenticated_gossipsub_peer(publisher, subscriber.local_peer()) &&
             authenticated_gossipsub_peer(subscriber, publisher.local_peer()) &&
             receipts->subscribed(subscriber.local_peer(), subject) &&
             gossipsub_mesh_peer(publisher, subscriber.local_peer(), subject);
   });
   ready->store(0U, std::memory_order_release);
   start->store(false, std::memory_order_release);
   for (auto index = std::size_t{}; index < publish_count; ++index) {
      auto payload = std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(index + 1U));
      publishes.push_back(boost::asio::co_spawn(
          runtime.context(),
          [&publisher, subject, payload = std::move(payload), ready,
           start]() mutable -> boost::asio::awaitable<pubsub::message> {
             ready->fetch_add(1U, std::memory_order_release);
             while (!start->load(std::memory_order_acquire)) {
                co_await boost::asio::post(boost::asio::use_awaitable);
             }
             co_return co_await publisher.async_publish(subject, std::move(payload));
          },
          boost::asio::use_future));
   }
   const auto start_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (ready->load(std::memory_order_acquire) != publish_count &&
          std::chrono::steady_clock::now() < start_deadline) {
      wait_on_runtime(runtime, std::chrono::milliseconds{1}, "concurrent GossipSub start barrier");
   }
   BOOST_REQUIRE_EQUAL(ready->load(std::memory_order_acquire), publish_count);
   start->store(true, std::memory_order_release);
   for (auto& publish : publishes) {
      BOOST_REQUIRE_MESSAGE(publish.wait_for(std::chrono::seconds{10}) == std::future_status::ready,
                            "concurrent GossipSub publish did not finish");
      try {
         static_cast<void>(publish.get());
      } catch (const forge::exceptions::base& error) {
         const auto publisher_metrics = publisher.metrics();
         const auto subscriber_metrics = subscriber.metrics();
         BOOST_FAIL("concurrent GossipSub publish failed: "
                    << error.what() << "; publisher sessions=" << publisher_metrics.active_sessions << " closed="
                    << publisher_metrics.sessions_closed << " pruned=" << publisher_metrics.sessions_pruned
                    << " connection_rejections=" << publisher_metrics.connection_rejections << " direct_failures="
                    << publisher_metrics.direct_failures << " invalid=" << publisher_metrics.pubsub_invalid_messages
                    << " protocol_rejections=" << publisher_metrics.protocol_rejections
                    << " handshakes_failed=" << publisher_metrics.handshakes_failed << "; subscriber sessions="
                    << subscriber_metrics.active_sessions << " closed=" << subscriber_metrics.sessions_closed
                    << " invalid=" << subscriber_metrics.pubsub_invalid_messages
                    << " protocol_rejections=" << subscriber_metrics.protocol_rejections);
      }
   }
   {
      auto lock = std::unique_lock{delivered->mutex};
      BOOST_REQUIRE(delivered->ready.wait_for(lock, std::chrono::seconds{10},
                                              [&] { return delivered->values.size() == publish_count; }));
      for (auto index = std::size_t{}; index < publish_count; ++index) {
         const auto expected = std::vector<std::uint8_t>(payload_size, static_cast<std::uint8_t>(index + 1U));
         BOOST_TEST(delivered->values.contains(expected));
      }
   }
   BOOST_TEST(publisher.metrics().active_sessions == 1U);
   BOOST_TEST(publisher.metrics().sessions_opened == 1U);
   BOOST_TEST(subscriber.metrics().active_sessions == 1U);
   BOOST_TEST(publisher.metrics().pubsub_invalid_messages == 0U);
   BOOST_TEST(subscriber.metrics().pubsub_invalid_messages == 0U);

   shutdown.join();
}

void dead_outbound_generation_detaches_mesh_and_reannounces_before_graft() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto publisher_identity = make_identity_fixture("pubsub-outbound-generation-publisher");
   const auto subscriber_identity = make_identity_fixture("pubsub-outbound-generation-subscriber");
   auto publisher_options = pubsub_options_for(publisher_identity);
   publisher_options.allow_insecure_test_mode = false;
   publisher_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::milliseconds{10};
   publisher_options.limits.pubsub.limits.heartbeat_interval = std::chrono::seconds{60};
   auto subscriber_options = pubsub_options_for(subscriber_identity);
   subscriber_options.allow_insecure_test_mode = false;
   subscriber_options.limits.pubsub.limits.heartbeat_initial_delay = std::chrono::seconds{60};
   subscriber_options.limits.pubsub.limits.max_data_size = 4;
   auto publisher_receipts = std::make_shared<gossipsub_test_receipts>();
   auto subscriber_receipts = std::make_shared<gossipsub_test_receipts>();
   publisher_options.limits.pubsub.tracer = [publisher_receipts](const auto& event) { publisher_receipts->capture(event); };
   subscriber_options.limits.pubsub.tracer = [subscriber_receipts](const auto& event) { subscriber_receipts->capture(event); };

   auto publisher = node{runtime, std::move(publisher_options)};
   auto subscriber = node{runtime, std::move(subscriber_options)};
   auto inbound = std::optional<forge::net::p2p::stream>{};
   auto shutdown = gossipsub_test_shutdown{runtime, publisher, subscriber};
   const auto subscriber_endpoint = listen(subscriber, runtime);

   const auto subject = pubsub::topic{.value = "forge.pubsub.outbound-generation"};
   const auto accept = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      co_return pubsub::validation_result::accept;
   };
   forge::asio::blocking::run(runtime, publisher.async_subscribe(subject, accept));
   forge::asio::blocking::run(runtime, subscriber.async_subscribe(subject, accept));
   publisher.peers().learn_endpoint(subscriber.local_peer(), subscriber_endpoint,
                                    capability_set{.bits = capabilities::direct_quic | capabilities::pubsub});
   forge::asio::blocking::run(
       runtime,
       publisher.async_connect(subscriber_endpoint, node::connect_options{.expected_peer = subscriber.local_peer()}));

   inbound.emplace(forge::asio::blocking::run(
       runtime, subscriber.async_open_protocol_stream(publisher.local_peer(), builtins::meshsub_v11)));
   const auto subscribe_and_graft = pubsub::codec::encode(pubsub::rpc{
       .subscriptions = {pubsub::subscription{.subscribe = true, .subject = subject}},
       .control_value = pubsub::control{.grafts = {pubsub::control::graft{.subject = subject}}},
   });
   forge::asio::blocking::run(runtime, inbound->async_write(subscribe_and_graft));
   for (auto poll = 0;
        poll < 200 && (publisher.pubsub_snapshot().peers != 1U || publisher.pubsub_snapshot().mesh_edges != 1U);
        ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub explicit inbound generation");
   }
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 1U);
   BOOST_REQUIRE(authenticated_gossipsub_peer(publisher, subscriber.local_peer()));
   BOOST_REQUIRE(authenticated_gossipsub_peer(subscriber, publisher.local_peer()));
   BOOST_REQUIRE(publisher_receipts->subscribed(subscriber.local_peer(), subject));
   const auto active_sessions = publisher.metrics().active_sessions;
   BOOST_REQUIRE(active_sessions > 0U);
   wait_for_gossipsub(runtime, "GossipSub G1 native subscription receipt", [&] {
      return subscriber_receipts->subscribed(publisher.local_peer(), subject) &&
             publisher_receipts->written_generation(subscriber.local_peer(), subject) != 0U;
   });
   const auto g1_generation = publisher_receipts->written_generation(subscriber.local_peer(), subject);
   const auto g1_read_stream = subscriber_receipts->latest_subscription_stream(publisher.local_peer(), subject);
   BOOST_REQUIRE(g1_generation != 0U);

   forge::asio::blocking::run(runtime,
                              publisher.async_publish(subject, std::vector<std::uint8_t>(8, std::uint8_t{0x41U})));
   for (auto poll = 0; poll < 200 && subscriber.metrics().protocol_rejections < 1U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub G1 remote reset");
   }
   BOOST_REQUIRE(subscriber.metrics().protocol_rejections >= 1U);
   const auto g1_streams = publisher.metrics().protocol_streams_opened;

   // A real subscription announcement reopens the dead stream without using MESSAGE as a repair probe.
   wait_for_gossipsub(runtime, "GossipSub G2 candidate subscription announcement", [&] {
      forge::asio::blocking::run(runtime, publisher.async_subscribe(subject, accept));
      return publisher_receipts->written_generation(subscriber.local_peer(), subject) > g1_generation;
   });
   BOOST_REQUIRE(publisher.metrics().protocol_streams_opened > g1_streams);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(subscriber.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.metrics().active_sessions == active_sessions);
   const auto g2_streams = publisher.metrics().protocol_streams_opened;
   const auto g2_generation = publisher_receipts->written_generation(subscriber.local_peer(), subject);

   // The publisher had no listener/heartbeat until now: observe detach before starting native repair.
   static_cast<void>(listen(publisher, runtime));
   wait_for_gossipsub(runtime, "GossipSub G2 native heartbeat SUBSCRIBE before GRAFT", [&] {
      return subscriber_receipts->reannounced_before_graft(publisher.local_peer(), subject, g1_read_stream) &&
             gossipsub_mesh_peer(publisher, subscriber.local_peer(), subject);
   });

   forge::asio::blocking::run(runtime,
                              publisher.async_publish(subject, std::vector<std::uint8_t>(8, std::uint8_t{0x42U})));
   for (auto poll = 0; poll < 400 && subscriber.metrics().protocol_rejections < 2U; ++poll) {
      wait_on_runtime(runtime, std::chrono::milliseconds{5}, "GossipSub G2 remote reset");
   }
   BOOST_REQUIRE(subscriber.metrics().protocol_rejections >= 2U);
   wait_for_gossipsub(runtime, "GossipSub G3 candidate subscription announcement", [&] {
      forge::asio::blocking::run(runtime, publisher.async_subscribe(subject, accept));
      return publisher_receipts->written_generation(subscriber.local_peer(), subject) > g2_generation;
   });
   BOOST_REQUIRE(publisher.metrics().protocol_streams_opened > g2_streams);
   BOOST_REQUIRE(publisher.pubsub_snapshot().mesh_edges == 0U);
   BOOST_REQUIRE(publisher.pubsub_snapshot().peers == 1U);
   BOOST_REQUIRE(publisher.metrics().active_sessions == active_sessions);

   forge::asio::blocking::run(runtime, inbound->async_close());
   shutdown.join();
}

} // namespace forge::tests::p2p::gossipsub_outbound_tests

namespace forge::net::p2p {

void gossipsub_outbound_cached_shutdown_regression() {
   forge::tests::p2p::gossipsub_outbound_tests::rejects_publish_after_cached_stream_shutdown();
}

void gossipsub_outbound_forwarding_regression() {
   forge::tests::p2p::gossipsub_outbound_tests::forwards_between_subscribed_peers();
}

void gossipsub_outbound_quota_rejection_regression() {
   forge::tests::p2p::gossipsub_outbound_tests::outbound_byte_limit_rejects_publish_without_stopping_node();
}

void gossipsub_outbound_per_peer_quota_regression() {
   forge::tests::p2p::gossipsub_outbound_tests::outbound_byte_limit_counts_blocked_active_publication_per_peer();
}

void gossipsub_outbound_global_quota_regression() {
   forge::tests::p2p::gossipsub_outbound_tests::outbound_byte_limit_is_global_across_peers();
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_CASE(p2p_gossipsub_singleflights_cold_subscriptions_then_serializes_concurrent_publishes) {
   forge::tests::p2p::gossipsub_outbound_tests::singleflights_cold_subscriptions_then_serializes_concurrent_publishes();
}

BOOST_AUTO_TEST_CASE(p2p_gossipsub_dead_outbound_generation_detaches_mesh_and_reannounces_before_graft) {
   forge::tests::p2p::gossipsub_outbound_tests::dead_outbound_generation_detaches_mesh_and_reannounces_before_graft();
}
