module;

#include <forge/exceptions/macros.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../../libraries/net/p2p/details/node_impl.hxx"
#include "../pubsub_router_fixture.hxx"
#include "../gossipsub_test_shutdown.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;
using forge::tests::p2p::pubsub_router_fixture;
using forge::tests::p2p::gossipsub_test_shutdown;

void node_session_fixture::partial_registry() {
   auto registry = detail::pubsub_partial{};
   auto foreign = detail::pubsub_partial{};
   auto limits = pubsub::limits{};
   limits.max_partial_groups_per_topic = 1;
   limits.max_partial_groups = 2;
   limits.max_partial_group_bytes = 3;
   limits.max_partial_callbacks = 1;
   limits.max_partial_callback_bytes = 256;
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   const auto old = registry.prepare({"partial"}, pubsub::partial_options{.gossip = gossip});
   BOOST_CHECK(!registry.install(old, limits));
   BOOST_CHECK(registry.current(old->token));
   BOOST_CHECK(!foreign.current(old->token));
   BOOST_CHECK_THROW(foreign.require(old->token), forge::exceptions::base);
   registry.advertise(old->token, {0, 0xff}, limits);
   registry.advertise(old->token, {0, 0xff}, limits);
   BOOST_CHECK_THROW(registry.advertise(old->token, {'x'}, limits), forge::exceptions::base);
   auto lease = registry.admit(old, 64, true, limits);
   BOOST_REQUIRE(lease);
   BOOST_CHECK((lease->take_groups() == std::vector<std::vector<std::uint8_t>>{{0, 0xff}}));
   limits.max_partial_callbacks = 2; // A lost busy lease must not be masked by the global callback bound.
   BOOST_CHECK(!registry.admit(old, 64, true, limits));
   const auto retired = registry.close(old->token.subject());
   retired->stop.request_stop();
   const auto next = registry.prepare({"partial"}, pubsub::partial_options{.gossip = gossip});
   BOOST_CHECK(!registry.install(next, limits));
   registry.advertise(next->token, {'n'}, limits);
   BOOST_CHECK(!registry.current(old->token));
   BOOST_CHECK_THROW(registry.forget(old->token, {}), forge::exceptions::base);
   BOOST_CHECK(registry.registrations().empty());
   BOOST_CHECK(!registry.admit(next, 64, true, limits)); // Old callback still owns this topic's busy state.
   lease.reset();
   lease = registry.admit(next, 64, true, limits);
   BOOST_REQUIRE(lease);
   lease.reset(); // Includes a task whose body was never started.
   auto snapshot = pubsub::snapshot{};
   registry.snapshot(snapshot);
   BOOST_CHECK_EQUAL(snapshot.partial_callbacks, 0U);
   BOOST_CHECK_EQUAL(snapshot.partial_callback_bytes, 0U);
   limits.max_partial_callback_bytes = 1;
   BOOST_CHECK(!registry.admit(next, 1, true, limits)); // Group snapshot cannot be copied before budget admission.
   registry.heartbeat(); registry.heartbeat();
   registry.snapshot(snapshot);
   BOOST_CHECK_EQUAL(snapshot.partial_groups, 1U);
   registry.heartbeat(); registry.snapshot(snapshot);
   BOOST_CHECK_EQUAL(snapshot.partial_groups, 0U);
   BOOST_CHECK_EQUAL(snapshot.partial_group_bytes, 0U);
   registry.stop();
   BOOST_CHECK(next->stop.stop_requested());
   BOOST_CHECK(!registry.current(next->token));

   auto options = pubsub::options{};
   options.preferred = static_cast<pubsub::version>(255);
   BOOST_CHECK_EXCEPTION(pubsub::validate(options), forge::exceptions::base,
       [](const auto& error) { return exceptions::is(error, exceptions::code::invalid_options); });
   BOOST_CHECK_THROW(pubsub::codec::protocol(options.preferred), forge::exceptions::base);
   options.preferred = pubsub::version::v1_1;
   options.partial_messages = true;
   BOOST_CHECK_THROW(pubsub::validate(options), forge::exceptions::base);
}

void node_session_fixture::native_partial_exchange() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3;
   config.partial_messages = true; config.flood_publish = true;
   auto legacy_config = config; legacy_config.preferred = pubsub::version::v1_1; legacy_config.partial_messages = false;
   auto& owner = fixture.add("partial-owner", config);
   auto& remote = fixture.add("partial-remote", config);
   auto& legacy = fixture.add("partial-legacy", legacy_config);
   auto incoming = stream{};
   auto local = pubsub::partial_topic{}, destination = pubsub::partial_topic{};
   auto received = std::atomic_size_t{}, replies = std::atomic_size_t{}, capability_parts = std::atomic_size_t{};
   auto empty_metadata = std::atomic_bool{false};
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto legacy_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, legacy};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [&] { incoming.request_cancel(); }};
   BOOST_CHECK_THROW(run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await owner.async_subscribe(fixture.topic, full, pubsub::partial_options{
          .receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; }}));
   }()), forge::exceptions::base);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      local = co_await owner.async_subscribe(fixture.topic, full, pubsub::partial_options{
          .receive = [&](pubsub::partial_event event, std::stop_token) -> boost::asio::awaitable<void> {
             if (event.source == remote.local_peer() && event.value.metadata) { ++replies; }
             co_return;
          }, .gossip = gossip});
      destination = co_await remote.async_subscribe(fixture.topic, full, pubsub::partial_options{
          .requests_partial = true,
          .receive = [&](pubsub::partial_event event, std::stop_token stop) -> boost::asio::awaitable<void> {
             if (!event.value.data && event.value.metadata && event.value.metadata->empty()) { empty_metadata = true; }
             ++received;
             if (event.value.data) {
                co_await remote.async_send_partial(event.registration, event.source,
                    pubsub::partial_message{.group_id = event.value.group_id, .metadata = std::vector<std::uint8_t>{'r'}}, stop);
             }
          }, .gossip = gossip});
   }());
   fixture.subscribe(legacy);
   fixture.connect(owner, remote); fixture.connect(owner, legacy);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->partial_peer_supported_locked(remote.local_peer(), fixture.topic, true);
   }));
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto peers = co_await owner.async_partial_peers(local);
      BOOST_CHECK(std::ranges::find(peers, remote.local_peer()) != peers.end());
      BOOST_CHECK(std::ranges::find(peers, legacy.local_peer()) == peers.end());
      co_await owner.async_send_partial(local, remote.local_peer(), pubsub::partial_message{
          .group_id = std::vector<std::uint8_t>{0, 0xff}, .metadata = std::vector<std::uint8_t>{}});
   }());
   BOOST_REQUIRE(fixture.wait([&] { return empty_metadata.load(); }));
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      co_await owner.async_send_partial(local, remote.local_peer(), pubsub::partial_message{
          .group_id = std::vector<std::uint8_t>{0, 0xff}, .data = std::vector<std::uint8_t>{42}});
   }());
   BOOST_REQUIRE(fixture.wait([&] { return replies.load() == 1 && received.load() == 2; }));
   BOOST_CHECK_EQUAL(remote.pubsub_snapshot().messages_delivered, 0U);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().messages_delivered, 0U);
   BOOST_CHECK_THROW(run(fixture.runtime, owner.async_forget_partial(destination, {})), forge::exceptions::base);
   BOOST_CHECK_THROW(run(fixture.runtime, owner.async_advertise_partial(local,
       std::vector<std::uint8_t>(config.limits.max_partial_group_id_size + 1))), forge::exceptions::base);
   BOOST_CHECK_THROW(run(fixture.runtime, remote.async_send_partial(destination, owner.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .data = std::vector<std::uint8_t>{}})), forge::exceptions::base);
   BOOST_CHECK_THROW(run(fixture.runtime, owner.async_send_partial(local, legacy.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{1}})), forge::exceptions::base);
   const auto cached = fixture.publish(owner, "ordinary-full-fallback");
   BOOST_REQUIRE(fixture.wait([&] { return legacy.pubsub_snapshot().messages_delivered == 1; }));
   BOOST_CHECK_EQUAL(remote.pubsub_snapshot().messages_delivered, 0U);
   const auto id = pubsub::codec::message_id(cached, config);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      auto generation = std::optional<std::uint64_t>{};
      static_cast<void>(co_await owner.impl_->send_pubsub_rpc(remote.local_peer(), pubsub::rpc{
          .messages = {cached}, .control_value = pubsub::control{
              .have = {{.subject = fixture.topic, .message_ids = {id}}},
              .want = {{.message_ids = {{0xff}}}}}}, generation));
      // An explicit cache request remains valid even for a peer requesting parts.
      static_cast<void>(co_await remote.impl_->send_pubsub_rpc(owner.local_peer(), pubsub::rpc{
          .control_value = pubsub::control{.want = {{.message_ids = {id}}}}}, generation));
   }());
   BOOST_REQUIRE(fixture.wait([&] { return remote.pubsub_snapshot().messages_delivered == 1; }));
   auto mixed_control = false;
   auto advertised = std::set<std::uint64_t>{};
   for (const auto& receipt : fixture.receipts(owner)) {
      if (receipt.kind != pubsub::trace_kind::rpc_write) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      if (receipt.peer == remote.local_peer() && value.control_value && !value.control_value->want.empty()) {
         mixed_control = true;
         BOOST_CHECK(value.messages.empty());
         BOOST_CHECK(value.control_value->have.empty());
      }
      if (receipt.peer == remote.local_peer() && !advertised.contains(receipt.generation)) {
         BOOST_REQUIRE(value.control_value && value.control_value->extensions);
         BOOST_CHECK(value.control_value->extensions->partial_messages.value_or(false));
         advertised.insert(receipt.generation);
      } else { BOOST_CHECK(!value.control_value || !value.control_value->extensions); }
      if (receipt.peer == legacy.local_peer()) {
         BOOST_CHECK(receipt.protocol == builtins::meshsub_v11);
         BOOST_CHECK(!value.partial);
         for (const auto& row : value.subscriptions) { BOOST_CHECK(!row.requests_partial && !row.supports_sending_partial); }
      }
   }
   BOOST_CHECK_EQUAL(advertised.size(), 1U);
   BOOST_CHECK(mixed_control);
   // A full-only local registration must restore full fallback, despite the remote request flag.
   fixture.subscribe(owner);
   static_cast<void>(fixture.publish(owner, "local-full-only"));
   BOOST_REQUIRE(fixture.wait([&] { return remote.pubsub_snapshot().messages_delivered == 2 && legacy.pubsub_snapshot().messages_delivered == 2; }));

   // Synthetic subscription variants on a native stream; these are not donor interoperability claims.
   const auto probe = pubsub::topic{"partial-capability-probe"};
   auto probe_token = pubsub::partial_topic{};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      probe_token = co_await owner.async_subscribe(probe, full, pubsub::partial_options{
          .receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; }, .gossip = gossip});
      static_cast<void>(co_await remote.async_subscribe(probe, full, pubsub::partial_options{
          .requests_partial = true,
          .receive = [&](pubsub::partial_event event, std::stop_token) -> boost::asio::awaitable<void> {
             if (event.source == owner.local_peer() && event.value.group_id == std::optional{std::vector<std::uint8_t>{9}} &&
                 event.value.data == std::optional{std::vector<std::uint8_t>{42}}) { ++capability_parts; }
             co_return;
          }, .gossip = gossip}));
      incoming = co_await remote.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v13,
          node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
   }());
   const auto rejected_before = owner.metrics().protocol_rejections;
   const auto invalid_before = owner.pubsub_snapshot().invalid_messages;
   auto first = true;
   for (const auto supports : std::vector<std::optional<bool>>{std::nullopt, false, true}) {
      auto update = pubsub::rpc{.subscriptions = {{.subject = probe, .requests_partial = true,
                                                  .supports_sending_partial = supports}}};
      if (first) { update.control_value = pubsub::control{.extensions = pubsub::extensions{.partial_messages = true}}; }
      fixture.send(incoming, update);
      first = false;
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         const auto peer = owner.impl_->pubsub_value.inbound.find(remote.local_peer());
         if (peer == owner.impl_->pubsub_value.inbound.end() || peer->second.empty()) { return false; }
         const auto& latest = peer->second.rbegin()->second;
         const auto topic = latest.partial_topics.find(probe.value);
         return topic != latest.partial_topics.end() && topic->second.requests_partial == std::optional<bool>{true} &&
             topic->second.supports_sending_partial == supports &&
             owner.impl_->partial_peer_supported_locked(remote.local_peer(), probe, false) &&
             owner.impl_->partial_peer_supported_locked(remote.local_peer(), probe, true);
      }));
      const auto expected = capability_parts.load() + 1;
      run(fixture.runtime, owner.async_send_partial(probe_token, remote.local_peer(), pubsub::partial_message{
          .group_id = std::vector<std::uint8_t>{9}, .data = std::vector<std::uint8_t>{42}}));
      BOOST_REQUIRE(fixture.wait([&] { return capability_parts.load() == expected; }));
   }
   fixture.send(incoming, pubsub::rpc{.subscriptions = {{.subject = probe, .requests_partial = false,
                                                        .supports_sending_partial = false}}});
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return !owner.impl_->partial_peer_supported_locked(remote.local_peer(), probe, false) &&
          !owner.impl_->partial_peer_supported_locked(remote.local_peer(), probe, true);
   }));
   BOOST_CHECK_EXCEPTION(run(fixture.runtime, owner.async_send_partial(probe_token, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{9}, .data = std::vector<std::uint8_t>{42}})),
       forge::exceptions::base, [](const auto& error) { return exceptions::is(error, exceptions::code::unsupported_protocol); });

   // Omitting subscribe is proto2 unsubscribe, even when partial request/support fields are present.
   auto sub = std::vector<std::uint8_t>{0x12, static_cast<std::uint8_t>(probe.value.size())};
   sub.insert(sub.end(), probe.value.begin(), probe.value.end());
   sub.insert(sub.end(), {0x18, 1, 0x20, 0});
   BOOST_REQUIRE_LT(sub.size() + 2, 128U);
   auto withdrawal = std::vector<std::uint8_t>{static_cast<std::uint8_t>(sub.size() + 2), 0x0a,
                                               static_cast<std::uint8_t>(sub.size())};
   withdrawal.insert(withdrawal.end(), sub.begin(), sub.end());
   run(fixture.runtime, incoming.async_write(withdrawal));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto& latest = owner.impl_->pubsub_value.inbound.at(remote.local_peer()).rbegin()->second;
      const auto topics = owner.impl_->pubsub_value.peer_topics.find(remote.local_peer());
      return !latest.partial_topics.contains(probe.value) &&
          (topics == owner.impl_->pubsub_value.peer_topics.end() || !topics->second.contains(probe.value));
   }));
   BOOST_CHECK_EQUAL(capability_parts.load(), 3U);
   BOOST_CHECK_EQUAL(owner.metrics().protocol_rejections, rejected_before);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().invalid_messages, invalid_before);
   shutdown.join(); legacy_shutdown.join();
}

void node_session_fixture::native_partial_queued_send() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3; config.partial_messages = true;
   auto& owner = fixture.add("partial-gated-owner", config);
   auto& remote = fixture.add("partial-gated-remote", config);
   auto ticket = forge::asio::gate::ticket{};
   auto pending = std::future<void>{};
   auto stop = std::stop_source{};
   const auto cleanup = [&] { stop.request_stop(); ticket.release(); };
   const auto join = [&](auto deadline) {
      if (pending.valid()) {
         if (pending.wait_until(deadline) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
         try { pending.get(); } catch (...) {}
      }
   };
   auto local = pubsub::partial_topic{}, target = pubsub::partial_topic{};
   auto deliveries = std::atomic_size_t{};
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto receive = [&](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { ++deliveries; co_return; };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, cleanup, join};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      local = co_await owner.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip});
      target = co_await remote.async_subscribe(fixture.topic, full, pubsub::partial_options{.requests_partial = true, .receive = receive, .gossip = gossip});
   }());
   fixture.connect(owner, remote);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto out = owner.impl_->pubsub_value.outbound.find(remote.local_peer());
      return owner.impl_->partial_peer_supported_locked(remote.local_peer(), fixture.topic, true) &&
          out != owner.impl_->pubsub_value.outbound.end() && out->second.stream && !out->second.snapshot_pending &&
          owner.impl_->pubsub_value.outbound_budget.total() == 0;
   }));
   auto gate = std::shared_ptr<forge::asio::gate>{};
   auto generation = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto& out = owner.impl_->pubsub_value.outbound.at(remote.local_peer());
      gate = out.write_gate; generation = out.generation;
   }
   const auto hold = [&] {
      auto acquired = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
      if (acquired.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
      ticket = acquired.get();
   };
   const auto start_send = [&](std::stop_token token) {
      pending = boost::asio::co_spawn(fixture.runtime.context(), owner.async_send_partial(local, remote.local_peer(),
          pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .data = std::vector<std::uint8_t>{2}}, token),
          boost::asio::use_future);
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         return owner.impl_->pubsub_value.outbound_budget.total() != 0;
      }));
   };
   const auto partial_writes = [&] {
      auto count = std::size_t{};
      for (const auto& receipt : fixture.receipts(owner)) {
         if (receipt.kind == pubsub::trace_kind::rpc_write && pubsub::codec::decode(receipt.frame, config).partial) { ++count; }
      }
      return count;
   };
   auto pre_canceled = std::stop_source{};
   pre_canceled.request_stop();
   BOOST_CHECK_EXCEPTION(run(fixture.runtime, owner.async_send_partial(local, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .data = std::vector<std::uint8_t>{2}},
       pre_canceled.get_token())), forge::exceptions::base,
       [](const auto& error) { return exceptions::is(error, exceptions::code::canceled); });
   BOOST_CHECK_EQUAL(partial_writes(), 0U);
   hold(); start_send(stop.get_token()); stop.request_stop();
   if (pending.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   BOOST_CHECK_EXCEPTION(pending.get(), forge::exceptions::base,
       [](const auto& error) { return exceptions::is(error, exceptions::code::canceled); });
   BOOST_CHECK_EQUAL(partial_writes(), 0U);
   ticket.release();
   hold(); start_send({});
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      target = co_await remote.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip});
   }());
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->partial_peer_supported_locked(remote.local_peer(), fixture.topic, false) &&
          !owner.impl_->partial_peer_supported_locked(remote.local_peer(), fixture.topic, true);
   }));
   ticket.release();
   if (pending.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   BOOST_CHECK_EXCEPTION(pending.get(), forge::exceptions::base,
       [](const auto& error) { return exceptions::is(error, exceptions::code::unsupported_protocol); });
   BOOST_CHECK_EQUAL(deliveries.load(), 0U);
   BOOST_CHECK_EQUAL(partial_writes(), 0U);
   {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto& out = owner.impl_->pubsub_value.outbound.at(remote.local_peer());
      BOOST_CHECK_EQUAL(out.generation, generation);
      BOOST_CHECK(out.stream && out.stream->valid());
      BOOST_CHECK_EQUAL(owner.impl_->pubsub_value.outbound_budget.total(), 0U);
   }
   run(fixture.runtime, owner.async_send_partial(local, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{3}}));
   BOOST_REQUIRE(fixture.wait([&] { return deliveries.load() == 1; }));
   BOOST_CHECK_EQUAL(partial_writes(), 1U);
   shutdown.join();
}

void node_session_fixture::native_partial_subscription_freshness() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3; config.partial_messages = true;
   auto& owner = fixture.add("partial-fresh-owner", config);
   auto& remote = fixture.add("partial-fresh-remote", config);
   const auto other = pubsub::topic{"unrelated"};
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto ticket = forge::asio::gate::ticket{};
   auto old = std::future<void>{}, replacement = std::future<void>{};
   const auto join = [&](auto deadline) {
      for (auto* pending : {&old, &replacement}) {
         if (!pending->valid()) { continue; }
         if (pending->wait_until(deadline) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
         try { pending->get(); } catch (...) {}
      }
   };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [&] { ticket.release(); }, join};
   fixture.subscribe(owner); fixture.subscribe(remote);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await owner.async_subscribe(other, full));
   }());
   fixture.connect(owner, remote);
   const auto idle = [&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      const auto out = owner.impl_->pubsub_value.outbound.find(remote.local_peer());
      return out != owner.impl_->pubsub_value.outbound.end() && out->second.stream && !out->second.snapshot_pending &&
          owner.impl_->pubsub_value.outbound_budget.total() == 0;
   };
   BOOST_REQUIRE(fixture.wait(idle));
   for (auto scenario = 0; scenario != 4; ++scenario) {
      // Each R1 starts full-only. The public R2 operation commits while R1 waits on the actual write gate.
      fixture.subscribe(owner);
      BOOST_REQUIRE(fixture.wait(idle));
      auto gate = std::shared_ptr<forge::asio::gate>{};
      {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         gate = owner.impl_->pubsub_value.outbound.at(remote.local_peer()).write_gate;
      }
      auto acquired = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
      if (acquired.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
      ticket = acquired.get();
      const auto before = fixture.receipts(owner).size();
      old = boost::asio::co_spawn(fixture.runtime.context(), [&, scenario]() -> boost::asio::awaitable<void> {
         if (scenario == 1) { static_cast<void>(co_await owner.async_subscribe(fixture.topic, full)); }
         else if (scenario == 3) { co_await owner.async_unsubscribe(fixture.topic); }
         else { co_await owner.impl_->announce_pubsub_subscriptions(remote.local_peer()); }
      }, boost::asio::use_future);
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         return owner.impl_->pubsub_value.outbound_budget.total() != 0;
      }));
      replacement = boost::asio::co_spawn(fixture.runtime.context(), [&, scenario]() -> boost::asio::awaitable<void> {
         if (scenario == 2) { co_await owner.async_unsubscribe(fixture.topic); }
         else {
            static_cast<void>(co_await owner.async_subscribe(fixture.topic, full,
                pubsub::partial_options{.requests_partial = true, .receive = receive, .gossip = gossip}));
         }
      }, boost::asio::use_future);
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{owner.impl_->mutex};
         if (scenario == 2) { return !owner.impl_->pubsub_value.handlers.contains(fixture.topic.value); }
         const auto current = owner.impl_->pubsub_value.partial.find(fixture.topic);
         return current && current->options.requests_partial;
      }));
      ticket.release();
      for (auto* pending : {&old, &replacement}) {
         if (pending->wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
         pending->get();
      }
      auto saw_topic = false, saw_other = false;
      const auto receipts = fixture.receipts(owner);
      for (auto i = before; i < receipts.size(); ++i) {
         const auto& receipt = receipts[i];
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != remote.local_peer()) { continue; }
         const auto value = pubsub::codec::decode(receipt.frame, config);
         for (const auto& row : value.subscriptions) {
            if (row.subject == other) { saw_other = true; BOOST_CHECK(row.subscribe); }
            if (row.subject != fixture.topic) { continue; }
            saw_topic = true;
            BOOST_CHECK_EQUAL(row.subscribe, scenario != 2);
            BOOST_CHECK_EQUAL(row.requests_partial.value_or(false), scenario != 2);
            BOOST_CHECK_EQUAL(row.supports_sending_partial.value_or(false), scenario != 2);
         }
      }
      BOOST_CHECK(saw_topic);
      if (scenario == 0 || scenario == 2) { BOOST_CHECK(saw_other); }
   }
   shutdown.join();
}

void node_session_fixture::native_partial_subscription_tracer() {
   auto fixture = pubsub_router_fixture{};
   auto armed = std::atomic_bool{false}, released = std::atomic_bool{false};
   auto release = std::promise<void>{};
   auto resumed = release.get_future().share();
   auto entered = std::promise<peer_id>{};
   auto observed = entered.get_future();
   auto old = std::future<void>{}, replacement = std::future<void>{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3; config.partial_messages = true;
   auto traced = config;
   traced.tracer = [&](const pubsub::trace_event& event) {
      if (event.kind != pubsub::trace_kind::rpc_write || !armed.exchange(false)) { return; }
      entered.set_value(event.peer);
      if (resumed.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   };
   auto& owner = fixture.add("partial-tracer-owner", traced);
   auto& first = fixture.add("partial-tracer-first", config);
   auto& second = fixture.add("partial-tracer-second", config);
   const auto unblock = [&] { if (!released.exchange(true)) { release.set_value(); } };
   const auto join = [&](auto deadline) {
      for (auto* operation : {&old, &replacement}) {
         if (!operation->valid()) { continue; }
         if (operation->wait_until(deadline) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
         try { operation->get(); } catch (...) {}
      }
   };
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto second_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, second, unblock, join};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, first, unblock, join};
   fixture.subscribe(owner); fixture.subscribe(first); fixture.subscribe(second);
   fixture.connect(owner, first); fixture.connect(owner, second);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->pubsub_value.outbound.size() == 2 && owner.impl_->pubsub_value.outbound_budget.total() == 0 &&
          std::ranges::all_of(owner.impl_->pubsub_value.outbound, [](const auto& row) {
             return row.second.stream && !row.second.snapshot_pending;
          });
   }));
   armed = true;
   old = boost::asio::co_spawn(fixture.runtime.context(), [&]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await owner.async_subscribe(fixture.topic, full));
   }, boost::asio::use_future);
   if (observed.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   const auto first_recipient = observed.get();
   const auto remaining_recipient = first_recipient == first.local_peer() ? second.local_peer() : first.local_peer();
   replacement = boost::asio::co_spawn(fixture.runtime.context(), [&]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await owner.async_subscribe(fixture.topic, full,
          pubsub::partial_options{.requests_partial = true, .receive = receive, .gossip = gossip}));
   }, boost::asio::use_future);
   // The replacement installs its state before waiting for the occupied writer.
   BOOST_REQUIRE(fixture.wait([&] {
      const auto registration = owner.impl_->pubsub_value.partial.find(fixture.topic);
      return registration && registration->options.requests_partial;
   }));
   const auto after_replacement = fixture.receipts(owner).size();
   unblock();
   if (old.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   old.get();
   if (replacement.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   replacement.get();
   auto saw_remaining = false;
   const auto receipts = fixture.receipts(owner);
   for (auto i = after_replacement; i < receipts.size(); ++i) {
      const auto& receipt = receipts[i];
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != remaining_recipient) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      for (const auto& row : value.subscriptions) {
         if (row.subject != fixture.topic) { continue; }
         saw_remaining = true;
         BOOST_CHECK(row.subscribe && row.requests_partial.value_or(false) && row.supports_sending_partial.value_or(false));
      }
   }
   BOOST_CHECK(saw_remaining);
   shutdown.join(); second_shutdown.join();
}

void node_session_fixture::native_partial_gossip() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3; config.partial_messages = true;
   config.limits.mesh_n = config.limits.mesh_n_low = config.limits.mesh_n_high = 1;
   config.limits.mesh_outbound_min = 0; config.limits.max_partial_callbacks = 1;
   auto& owner = fixture.add("partial-gossip-owner", config);
   auto& first = fixture.add("partial-gossip-first", config);
   auto& second = fixture.add("partial-gossip-second", config);
   auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto token = pubsub::partial_topic{};
   auto calls = std::atomic_size_t{}, received = std::atomic_size_t{};
   auto off_mesh = std::atomic_bool{false};
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto receive = [&](pubsub::partial_event event, std::stop_token) -> boost::asio::awaitable<void> {
      if (!event.value.data && event.value.metadata && event.value.group_id == std::optional{std::vector<std::uint8_t>{0, 0xff}}) { ++received; }
      co_return;
   };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto shutdown_second = gossipsub_test_shutdown{fixture.runtime, owner, second, [release] { release->notify(); }};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, first, [release] { release->notify(); }};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      token = co_await owner.async_subscribe(fixture.topic, full, pubsub::partial_options{
          .receive = receive,
          .gossip = [&](pubsub::partial_gossip_event event, std::stop_token stop) -> boost::asio::awaitable<void> {
             ++calls;
             if (event.groups != std::vector<std::vector<std::uint8_t>>{{0, 0xff}} || event.peers.empty()) { co_return; }
             {
                const auto lock = std::scoped_lock{owner.impl_->mutex};
                off_mesh = !owner.impl_->pubsub_value.mesh.at(fixture.topic.value).contains(event.peers.front());
             }
             const auto canceled = std::stop_callback{stop, [release] { release->notify(); }};
             co_await release->async_wait(epoch);
             if (!stop.stop_requested()) {
                co_await owner.async_send_partial(event.registration, event.peers.front(),
                    pubsub::partial_message{.group_id = event.groups.front(), .metadata = std::vector<std::uint8_t>{7}}, stop);
             }
          }});
      static_cast<void>(co_await first.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip}));
      static_cast<void>(co_await second.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip}));
      co_await owner.async_advertise_partial(token, {0, 0xff});
   }());
   fixture.connect(owner, first); fixture.connect(owner, second);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->partial_peer_supported_locked(first.local_peer(), fixture.topic, false) &&
          owner.impl_->partial_peer_supported_locked(second.local_peer(), fixture.topic, false);
   }));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().cached_messages, 0U);
   run(fixture.runtime, owner.impl_->pubsub_heartbeat_once());
   BOOST_REQUIRE(fixture.wait([&] { return calls.load() == 1; }));
   run(fixture.runtime, owner.impl_->pubsub_heartbeat_once());
   run(fixture.runtime, owner.impl_->pubsub_heartbeat_once());
   BOOST_CHECK_EQUAL(calls.load(), 1U);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().partial_callbacks, 1U);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().partial_groups, 0U);
   BOOST_CHECK(off_mesh.load());
   release->notify();
   BOOST_REQUIRE(fixture.wait([&] { return received.load() == 1 && owner.pubsub_snapshot().partial_callbacks == 0; }));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().partial_callback_bytes, 0U);
   run(fixture.runtime, owner.async_advertise_partial(token, {1}));
   run(fixture.runtime, owner.async_forget_partial(token, {1}));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().partial_groups, 0U);
   shutdown.join(); shutdown_second.join();
}

void node_session_fixture::native_partial_callback_stop() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3; config.partial_messages = true;
   auto& owner = fixture.add("partial-stop-owner", config);
   auto& remote = fixture.add("partial-stop-remote", config);
   auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto local = pubsub::partial_topic{}, target = pubsub::partial_topic{}, retired = pubsub::partial_topic{};
   auto unsubscribed = std::atomic_bool{false}, entered = std::atomic_bool{false}, canceled = std::atomic_bool{false};
   const auto full = [](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> { co_return pubsub::validation_result::accept; };
   const auto receive = [&](pubsub::partial_event event, std::stop_token stop) -> boost::asio::awaitable<void> {
      if (event.value.metadata == std::optional{std::vector<std::uint8_t>{1}}) {
         co_await remote.async_unsubscribe(event.registration);
         unsubscribed = true;
         co_return;
      }
      if (event.value.metadata == std::optional{std::vector<std::uint8_t>{2}}) { throw std::runtime_error{"application rejection"}; }
      entered = true;
      const auto request_stop = std::stop_callback{stop, [release] { release->notify(); }};
      co_await release->async_wait(epoch);
      canceled = stop.stop_requested();
   };
   const auto gossip = [](pubsub::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, [release] { release->notify(); }};
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      local = co_await owner.async_subscribe(fixture.topic, full, pubsub::partial_options{
          .receive = [](pubsub::partial_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; }, .gossip = gossip});
      target = co_await remote.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip});
   }());
   fixture.connect(owner, remote);
   const auto supported = [&] {
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->partial_peer_supported_locked(remote.local_peer(), fixture.topic, false);
   };
   BOOST_REQUIRE(fixture.wait(supported));
   run(fixture.runtime, owner.async_send_partial(local, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{1}}));
   BOOST_REQUIRE(fixture.wait([&] { return unsubscribed.load() && remote.pubsub_snapshot().partial_callbacks == 0; }));
   // Local unsubscribe has written its frame, not joined the remote decoder.
   // Observe that withdrawal before waiting for replacement capabilities.
   BOOST_REQUIRE(fixture.wait([&] { return !supported(); }));
   retired = target;
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      target = co_await remote.async_subscribe(fixture.topic, full, pubsub::partial_options{.receive = receive, .gossip = gossip});
   }());
   BOOST_REQUIRE(fixture.wait(supported));
   BOOST_CHECK_THROW(run(fixture.runtime, remote.async_unsubscribe(retired)), forge::exceptions::base);
   const auto invalid = remote.pubsub_snapshot().invalid_messages;
   run(fixture.runtime, owner.async_send_partial(local, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{2}}));
   BOOST_REQUIRE(fixture.wait([&] { return remote.pubsub_snapshot().partial_callback_failures == 1; }));
   BOOST_CHECK_EQUAL(remote.pubsub_snapshot().invalid_messages, invalid);
   run(fixture.runtime, owner.async_send_partial(local, remote.local_peer(),
       pubsub::partial_message{.group_id = std::vector<std::uint8_t>{1}, .metadata = std::vector<std::uint8_t>{3}}));
   BOOST_REQUIRE(fixture.wait([&] { return entered.load(); }));
   run(fixture.runtime, remote.async_stop());
   BOOST_CHECK(canceled.load());
   BOOST_CHECK_EQUAL(remote.pubsub_snapshot().partial_callbacks, 0U);
   BOOST_CHECK_EQUAL(remote.pubsub_snapshot().partial_callback_bytes, 0U);
   auto registry = detail::pubsub_partial{};
   const auto registration = registry.prepare(fixture.topic, {.receive = receive, .gossip = gossip});
   static_cast<void>(registry.install(registration, config.limits));
   registry.advertise(registration->token, {1}, config.limits);
   auto lease = registry.admit(registration, 64, true, config.limits);
   BOOST_REQUIRE(lease);
   auto ran = false;
   BOOST_CHECK(!remote.impl_->launch_tracked([lease = std::move(lease), &ran]() -> boost::asio::awaitable<void> {
      ran = true; co_return;
   }));
   auto snapshot = pubsub::snapshot{};
   registry.snapshot(snapshot);
   BOOST_CHECK(!ran);
   BOOST_CHECK_EQUAL(snapshot.partial_callbacks, 0U);
   BOOST_CHECK_EQUAL(snapshot.partial_callback_bytes, 0U);
   BOOST_CHECK_EQUAL(registry.registrations().size(), 1U);
   shutdown.join();
}

} // namespace forge::net::p2p

namespace {

namespace ps = forge::net::p2p::pubsub;
using partial_registry = forge::net::p2p::detail::pubsub_partial;

void check_partial_budget(const partial_registry& registry, const ps::limits& limits,
                          std::size_t groups, std::size_t bytes) {
   auto snapshot = ps::snapshot{};
   registry.snapshot(snapshot);
   BOOST_CHECK_EQUAL(snapshot.partial_groups, groups);
   BOOST_CHECK_EQUAL(snapshot.partial_group_bytes, bytes);
   BOOST_CHECK_LE(snapshot.partial_groups, limits.max_partial_groups);
   BOOST_CHECK_LE(snapshot.partial_group_bytes, limits.max_partial_group_bytes);
   BOOST_CHECK_EQUAL(snapshot.partial_callbacks, 0U);
   BOOST_CHECK_EQUAL(snapshot.partial_callback_bytes, 0U);
}

bool partial_backpressure(const forge::exceptions::base& error) {
   return forge::net::p2p::exceptions::is(error, forge::net::p2p::exceptions::code::backpressure_rejected);
}

} // namespace

BOOST_AUTO_TEST_SUITE(pubsub_partial_runtime)
BOOST_AUTO_TEST_CASE(registry_generation_budget_ttl_busy_and_config) { forge::net::p2p::node_session_fixture::partial_registry(); }

BOOST_AUTO_TEST_CASE(registry_global_count_exhaustion_and_forget_close_ttl_reuse) {
   auto registry = partial_registry{};
   auto limits = ps::limits{};
   limits.max_partial_groups_per_topic = 4;
   limits.max_partial_groups = 2;
   limits.max_partial_group_bytes = 64;
   limits.partial_group_ttl = 2;
   const auto gossip = [](ps::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   const auto a = registry.prepare({"count-a"}, {.gossip = gossip});
   const auto b = registry.prepare({"count-b"}, {.gossip = gossip});
   const auto c = registry.prepare({"count-c"}, {.gossip = gossip});
   BOOST_CHECK(!registry.install(a, limits));
   BOOST_CHECK(!registry.install(b, limits));
   BOOST_CHECK(!registry.install(c, limits));
   registry.advertise(a->token, {1}, limits);
   registry.advertise(b->token, {2}, limits);
   check_partial_budget(registry, limits, 2, 2);
   BOOST_CHECK_EXCEPTION(registry.advertise(c->token, {3}, limits), forge::exceptions::base, partial_backpressure);
   registry.advertise(a->token, {1}, limits); // Renewal at the global bound must not charge a second entry.
   check_partial_budget(registry, limits, 2, 2);
   registry.forget(a->token, {1});
   registry.forget(a->token, {1});
   check_partial_budget(registry, limits, 1, 1);
   registry.advertise(c->token, {3}, limits);
   check_partial_budget(registry, limits, 2, 2);
   const auto retired = registry.close(b->token.subject());
   BOOST_REQUIRE(retired);
   retired->stop.request_stop();
   check_partial_budget(registry, limits, 1, 1);
   const auto next = registry.prepare({"count-b"}, {.gossip = gossip});
   BOOST_CHECK(!registry.install(next, limits));
   BOOST_CHECK(!registry.current(b->token));
   registry.advertise(next->token, {4}, limits);
   check_partial_budget(registry, limits, 2, 2);
   registry.heartbeat();
   check_partial_budget(registry, limits, 2, 2);
   BOOST_CHECK_EXCEPTION(registry.advertise(a->token, {5}, limits), forge::exceptions::base, partial_backpressure);
   check_partial_budget(registry, limits, 2, 2);
   registry.heartbeat();
   check_partial_budget(registry, limits, 0, 0);
   registry.advertise(a->token, {5}, limits);
   registry.advertise(c->token, {6}, limits);
   check_partial_budget(registry, limits, 2, 2);
   BOOST_CHECK_EXCEPTION(registry.advertise(next->token, {7}, limits), forge::exceptions::base, partial_backpressure);
   check_partial_budget(registry, limits, 2, 2);
   registry.stop();
   check_partial_budget(registry, limits, 0, 0);
}

BOOST_AUTO_TEST_CASE(registry_global_bytes_exhaustion_and_forget_replacement_close_ttl_reuse) {
   auto registry = partial_registry{};
   auto limits = ps::limits{};
   limits.max_partial_groups_per_topic = 4;
   limits.max_partial_groups = 8;
   limits.max_partial_group_bytes = 5;
   limits.partial_group_ttl = 2;
   const auto gossip = [](ps::partial_gossip_event, std::stop_token) -> boost::asio::awaitable<void> { co_return; };
   const auto a = registry.prepare({"bytes-a"}, {.gossip = gossip});
   const auto b = registry.prepare({"bytes-b"}, {.gossip = gossip});
   const auto c = registry.prepare({"bytes-c"}, {.gossip = gossip});
   BOOST_CHECK(!registry.install(a, limits));
   BOOST_CHECK(!registry.install(b, limits));
   BOOST_CHECK(!registry.install(c, limits));
   registry.advertise(a->token, {1, 2}, limits);
   registry.advertise(b->token, {3, 4, 5}, limits);
   check_partial_budget(registry, limits, 2, 5);
   BOOST_CHECK_EXCEPTION(registry.advertise(c->token, {6}, limits), forge::exceptions::base, partial_backpressure);
   registry.advertise(b->token, {3, 4, 5}, limits);
   check_partial_budget(registry, limits, 2, 5);
   registry.forget(a->token, {1, 2});
   registry.forget(a->token, {1, 2});
   check_partial_budget(registry, limits, 1, 3);
   registry.advertise(c->token, {6, 7}, limits);
   check_partial_budget(registry, limits, 2, 5);
   const auto retired = registry.close(b->token.subject());
   BOOST_REQUIRE(retired);
   retired->stop.request_stop();
   check_partial_budget(registry, limits, 1, 2);
   const auto next_b = registry.prepare({"bytes-b"}, {.gossip = gossip});
   BOOST_CHECK(!registry.install(next_b, limits));
   registry.advertise(next_b->token, {8, 9, 10}, limits);
   check_partial_budget(registry, limits, 2, 5);
   const auto next_c = registry.prepare({"bytes-c"}, {.gossip = gossip});
   BOOST_CHECK(registry.install(next_c, limits) == c);
   c->stop.request_stop();
   BOOST_CHECK(!registry.current(c->token));
   check_partial_budget(registry, limits, 1, 3);
   registry.advertise(next_c->token, {11, 12}, limits);
   check_partial_budget(registry, limits, 2, 5);
   registry.heartbeat();
   check_partial_budget(registry, limits, 2, 5);
   BOOST_CHECK_EXCEPTION(registry.advertise(a->token, {13}, limits), forge::exceptions::base, partial_backpressure);
   check_partial_budget(registry, limits, 2, 5);
   registry.heartbeat();
   check_partial_budget(registry, limits, 0, 0);
   registry.advertise(a->token, {1, 2, 3, 4, 5}, limits);
   check_partial_budget(registry, limits, 1, 5);
   BOOST_CHECK_EXCEPTION(registry.advertise(next_b->token, {6}, limits), forge::exceptions::base, partial_backpressure);
   check_partial_budget(registry, limits, 1, 5);
   registry.forget(a->token, {1, 2, 3, 4, 5});
   check_partial_budget(registry, limits, 0, 0);
   registry.advertise(next_b->token, {7, 8, 9, 10, 11}, limits);
   check_partial_budget(registry, limits, 1, 5);
   registry.stop();
   check_partial_budget(registry, limits, 0, 0);
}

BOOST_AUTO_TEST_CASE(native_parts_metadata_reentrant_reply_and_full_fallback) { forge::net::p2p::node_session_fixture::native_partial_exchange(); }
BOOST_AUTO_TEST_CASE(native_canceled_gate_and_changed_capability_preserve_stream) { forge::net::p2p::node_session_fixture::native_partial_queued_send(); }
BOOST_AUTO_TEST_CASE(native_old_subscription_intents_cannot_restore_flags_or_unrelated_topics) { forge::net::p2p::node_session_fixture::native_partial_subscription_freshness(); }
BOOST_AUTO_TEST_CASE(native_old_full_subscribe_after_tracer_uses_replacement_flags) { forge::net::p2p::node_session_fixture::native_partial_subscription_tracer(); }
BOOST_AUTO_TEST_CASE(native_off_mesh_gossip_without_full_cache_busy_tick_and_ttl) { forge::net::p2p::node_session_fixture::native_partial_gossip(); }
BOOST_AUTO_TEST_CASE(native_scoped_unsubscribe_callback_failure_and_shutdown_join) { forge::net::p2p::node_session_fixture::native_partial_callback_stop(); }
BOOST_AUTO_TEST_SUITE_END()
