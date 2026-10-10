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
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
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
import forge.net.p2p.identify;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../../libraries/net/p2p/details/node_impl.hxx"
#include "../../../libraries/net/p2p/details/protocol_capabilities.hxx"
#include "../../../libraries/net/p2p/details/resource_service.hxx"
#include "../pubsub_router_fixture.hxx"
#include "../gossipsub_test_shutdown.hxx"
#include "node_session_fixture.hxx"

namespace forge::net::p2p {

using namespace std::chrono_literals;
using forge::tests::p2p::pubsub_router_fixture;
using forge::tests::p2p::gossipsub_test_shutdown;

void node_session_fixture::idontwant_bounds() {
   auto state = detail::pubsub_idontwant{};
   auto limits = pubsub::limits{};
   limits.max_idontwant_ids_per_rpc = 2;
   limits.max_idontwant_rpcs_per_heartbeat = 2;
   limits.max_idontwant_entries_per_peer = 2;
   limits.max_idontwant_bytes_per_peer = 3;
   limits.max_idontwant_entries = 3;
   limits.max_idontwant_bytes = 4;
   const auto a = peer(71), b = peer(72);
   const auto one = std::vector<pubsub::control::idontwant>{{.message_ids = {{0, 0xff}, {'x'}}}};
   state.receive(a, one, limits, 2);
   BOOST_CHECK_EQUAL(state.size(), 2U);
   BOOST_CHECK_EQUAL(state.bytes(), 3U);
   BOOST_CHECK(state.contains(a, std::string{"\0\xff", 2}));
   BOOST_CHECK(!state.contains(b, "x"));
   state.receive(a, std::vector<pubsub::control::idontwant>{{.message_ids = {{'y'}}}}, limits, 2);
   BOOST_CHECK(!state.contains(a, "y"));
   state.receive(b, std::vector<pubsub::control::idontwant>{{.message_ids = {{'z'}, {'w'}}}}, limits, 2);
   BOOST_CHECK(state.contains(b, "z"));
   BOOST_CHECK(!state.contains(b, "w"));
   BOOST_CHECK_EQUAL(state.size(), 3U);
   BOOST_CHECK_EQUAL(state.bytes(), 4U);
   state.heartbeat(); state.heartbeat();
   BOOST_CHECK(state.contains(a, "x"));
   state.heartbeat();
   BOOST_CHECK_EQUAL(state.size(), 0U);
   BOOST_CHECK_EQUAL(state.bytes(), 0U);
   BOOST_CHECK(state.ignored() >= 2U);

   limits.max_idontwant_bytes = limits.max_idontwant_bytes_per_peer = 256;
   limits.max_idontwant_entries = limits.max_idontwant_entries_per_peer = 16;
   state.receive(a, std::vector<pubsub::control::idontwant>{{.message_ids = {{'a'}}},
       {.message_ids = {{'b'}, {'c'}}}}, limits, 2);
   BOOST_CHECK(state.contains(a, "a") && state.contains(a, "b") && !state.contains(a, "c"));
   state.receive(a, std::vector<pubsub::control::idontwant>{{.message_ids = {{'d'}}}}, limits, 2);
   state.receive(a, std::vector<pubsub::control::idontwant>{{.message_ids = {{'e'}}}}, limits, 2);
   BOOST_CHECK(state.contains(a, "d") && !state.contains(a, "e"));
   state.forget(a);
   BOOST_CHECK_EQUAL(state.size(), 0U);
   BOOST_CHECK_EQUAL(state.bytes(), 0U);

   auto config = manual_options();
   config.limits.max_idontwant_ids_per_rpc = 2;
   config.limits.max_rpc_size = 64;
   const auto input = pubsub::control{.have = {{.subject = {"t"}, .message_ids = {{'h'}}}},
       .want = {{.message_ids = {{'w'}}}}, .dont_want = {{.message_ids = {{'a'}, {'b'}, {'c'}}}}};
   auto cursor = pubsub::codec::gossip_cursor{};
   auto charge = std::size_t{};
   auto seen = std::vector<std::vector<std::uint8_t>>{};
   auto chunks = std::size_t{};
   while (const auto chunk = pubsub::codec::next_gossip(input, cursor, config)) {
      const auto wire = pubsub::codec::encode(chunk->value, config);
      BOOST_CHECK_EQUAL(wire.size(), chunk->wire_bytes);
      charge += wire.size();
      ++chunks;
      for (const auto& row : chunk->value.control_value->dont_want) {
         BOOST_CHECK_LE(row.message_ids.size(), 2U);
         seen.insert(seen.end(), row.message_ids.begin(), row.message_ids.end());
      }
   }
   BOOST_CHECK_EQUAL(chunks, 4U);
   BOOST_CHECK((seen == std::vector<std::vector<std::uint8_t>>{{'a'}, {'b'}, {'c'}}));
   BOOST_REQUIRE(pubsub::codec::gossip_wire_size(input, config));
   BOOST_CHECK_EQUAL(*pubsub::codec::gossip_wire_size(input, config), charge);
}

void node_session_fixture::native_extension_versions(pubsub::version remote) {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.preferred = pubsub::version::v1_3;
   config.partial_messages = remote == pubsub::version::v1_3;
   auto legacy = config;
   legacy.preferred = remote;
   auto& source = fixture.add("extension-version-source", config);
   auto& target = fixture.add("extension-version-target", legacy);
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target};
   fixture.subscribe(source); fixture.subscribe(target); fixture.connect(source, target);
   const auto self = source.impl_;
   const auto peer = target.local_peer();
   const auto selected = pubsub::codec::protocol(remote);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto advertised = self->store.find(peer);
      return advertised && advertised->capabilities.has(capabilities::pubsub) &&
          std::ranges::find(advertised->protocols, selected) != advertised->protocols.end();
   }));
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto row = self->pubsub_value.outbound.find(peer);
      return row != self->pubsub_value.outbound.end() && row->second.stream && !row->second.snapshot_pending &&
          row->second.protocol == selected && self->pubsub_value.outbound_budget.total() == 0;
   }));
   const auto send_subscription = [&] {
      auto sent = false;
      run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
         auto generation = std::optional<std::uint64_t>{};
         sent = co_await self->send_pubsub_rpc(peer, pubsub::rpc{.subscriptions = {{.subject = fixture.topic}}}, generation);
      }());
      BOOST_REQUIRE(sent);
   };
   const auto received_generations = [&] {
      auto counts = std::map<std::uint64_t, std::size_t>{};
      for (const auto& receipt : fixture.receipts(target)) {
         if (receipt.kind == pubsub::trace_kind::rpc_read && receipt.peer == source.local_peer() &&
             receipt.protocol == selected) { ++counts[receipt.generation]; }
      }
      return counts;
   };
   send_subscription();
   BOOST_REQUIRE(fixture.wait([&] {
      const auto counts = received_generations();
      return counts.size() == 1 && counts.begin()->second >= 2;
   }));
   auto previous = std::uint64_t{};
   auto native = std::shared_ptr<stream>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      previous = self->pubsub_value.outbound.at(peer).generation;
      native = self->pubsub_value.outbound.at(peer).stream;
   }
   native->cancel(); // Actual native close, not a fabricated generation change.
   send_subscription();
   {
      const auto lock = std::scoped_lock{self->mutex};
      BOOST_CHECK(self->pubsub_value.outbound.at(peer).generation > previous);
      BOOST_CHECK(self->pubsub_value.outbound.at(peer).protocol == selected);
   }
   BOOST_REQUIRE(fixture.wait([&] {
      const auto counts = received_generations();
      return counts.size() == 2 && std::ranges::all_of(counts, [](const auto& row) { return row.second >= 2; });
   }));
   auto first = std::map<std::uint64_t, pubsub::rpc>{};
   for (const auto& receipt : fixture.receipts(target)) {
      if (receipt.kind != pubsub::trace_kind::rpc_read || receipt.peer != source.local_peer()) { continue; }
      BOOST_CHECK(receipt.protocol == selected);
      const auto value = pubsub::codec::decode(receipt.frame, config);
      const auto inserted = first.try_emplace(receipt.generation, value).second;
      if (inserted && config.partial_messages) {
         BOOST_REQUIRE(value.control_value);
         BOOST_REQUIRE(value.control_value->extensions);
         BOOST_REQUIRE(value.control_value->extensions->partial_messages.has_value());
         BOOST_CHECK(*value.control_value->extensions->partial_messages);
      } else {
         BOOST_CHECK(!value.control_value || !value.control_value->extensions);
      }
   }
   BOOST_REQUIRE_EQUAL(first.size(), 2U);
   for (const auto& [_, value] : first) {
      BOOST_REQUIRE_EQUAL(value.subscriptions.size(), 1U);
      BOOST_CHECK(value.subscriptions.front().subject == fixture.topic);
   }
   if (remote == pubsub::version::v1_2 || remote == pubsub::version::v1_3) {
      const auto push_protocol = [&](const protocol_id& advertised, bool pubsub_capability) {
         run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
            auto push = co_await target.async_open_protocol_stream(source.local_peer(), builtins::identify_push,
                node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
            co_await push.async_write(wrap_length_delimited(identify::encode(
                identify::document{.protocols = {advertised}})));
            co_await push.async_close();
         }());
         BOOST_REQUIRE(fixture.wait([&] {
            const auto record = self->store.find(peer);
            if (!record || record->protocols != std::vector<protocol_id>{advertised} ||
                record->capabilities.has(capabilities::pubsub) != pubsub_capability) { return false; }
            const auto lock = std::scoped_lock{self->mutex};
            return std::ranges::any_of(self->sessions, [&](const auto& row) {
               const auto& session = row.second;
               return session->info.remote_peer == peer && !session->closed &&
                   session->authentication != peer_authentication::unverified &&
                   session->info.identify_state == identify::state::identified &&
                   session->remote_protocols == record->protocols &&
                   session->info.capabilities.has(capabilities::pubsub) == pubsub_capability;
            });
         }));
      };
      push_protocol(protocol_id{"/meshsub/9.9.9"}, false);
      push_protocol(selected, true);
      run(fixture.runtime, target.async_stop());
      BOOST_REQUIRE(fixture.wait([&] {
         const auto lock = std::scoped_lock{self->mutex};
         return std::ranges::none_of(self->sessions, [&](const auto& row) { return row.second->info.remote_peer == peer; }) &&
             !self->pubsub_value.peer_topics.contains(peer);
      }));
      const auto retained = self->store.find(peer);
      BOOST_REQUIRE(retained);
      BOOST_CHECK(retained->protocols == std::vector<protocol_id>{selected});
      BOOST_CHECK(retained->capabilities.has(capabilities::pubsub));
      const auto candidates = self->pubsub_candidate_peers(fixture.topic.value);
      BOOST_CHECK(std::ranges::find(candidates, peer) != candidates.end());
   }
   shutdown.join();
}

void node_session_fixture::native_extension_first_rpc() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options(); config.preferred = pubsub::version::v1_3;
   auto& source = fixture.add("extension-first-source", config);
   auto& target = fixture.add("extension-first-target", config);
   auto incoming = stream{};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, source, target, [&] { incoming.request_cancel(); }};
   fixture.connect(source, target);
   for (int mode = 0; mode != 4; ++mode) {
      run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
         incoming = co_await source.async_open_protocol_stream(target.local_peer(), builtins::meshsub_v13,
             node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false});
      }());
      const auto before = target.metrics().protocol_rejections;
      if (mode == 3) {
         // Duplicate singular advertisements within the same RPC must not disappear in protobuf merge.
         run(fixture.runtime, incoming.async_write(std::vector<std::uint8_t>{10, 0x1a, 8, 0x32, 2, 0x50, 1, 0x32, 2, 0x50, 0}));
      } else {
         const auto marker = pubsub::topic{.value = "first-" + std::to_string(mode)};
         auto first = pubsub::rpc{.subscriptions = {{.subject = marker}}};
         if (mode == 1) { first.control_value = pubsub::control{.extensions = pubsub::extensions{.partial_messages = true}}; }
         if (mode == 2) {
            // A wholly unknown first extension is ignored but still consumes the advertisement slot.
            run(fixture.runtime, incoming.async_write(std::vector<std::uint8_t>{6, 0x1a, 4, 0x32, 2, 0x58, 1}));
         }
         fixture.send(incoming, first);
         BOOST_REQUIRE(fixture.wait([&] {
            const auto lock = std::scoped_lock{target.impl_->mutex};
            const auto row = target.impl_->pubsub_value.peer_topics.find(source.local_peer());
            return row != target.impl_->pubsub_value.peer_topics.end() && row->second.contains(marker.value);
         }));
         auto observed_generation = std::uint64_t{};
         for (const auto& receipt : fixture.receipts(target)) {
            if (receipt.kind == pubsub::trace_kind::rpc_read && receipt.peer == source.local_peer() &&
                receipt.stream == incoming.id()) { observed_generation = receipt.generation; }
         }
         {
            const auto lock = std::scoped_lock{target.impl_->mutex};
            const auto& state = target.impl_->pubsub_value.inbound.at(source.local_peer()).at(observed_generation);
            BOOST_CHECK(!state.first_rpc);
            BOOST_CHECK_EQUAL(state.partial_messages, mode == 1);
         }
         fixture.send(incoming, pubsub::rpc{.control_value = pubsub::control{
             .extensions = pubsub::extensions{.partial_messages = false}}});
      }
      BOOST_REQUIRE(fixture.wait([&] { return target.metrics().protocol_rejections > before; }));
      incoming.request_cancel();
   }
   shutdown.join();
}

void node_session_fixture::native_idontwant_suppression() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.preferred = pubsub::version::v1_2;
   config.signatures = pubsub::signature_policy::strict_no_sign;
   auto& owner = fixture.add("idw-owner", config);
   auto& remote = fixture.add("idw-remote", config);
   auto& other = fixture.add("idw-other", config);
   const auto self = owner.impl_;
   const auto peer = remote.local_peer();
   auto request = stream{}, suppression = stream{}, independent = stream{};
   auto ticket = forge::asio::gate::ticket{};
   auto pending = std::future<bool>{};
   const auto release = [&] {
      ticket.release();
      request.request_cancel(); suppression.request_cancel(); independent.request_cancel();
   };
   const auto join = [&](auto deadline) {
      if (pending.valid()) {
         if (pending.wait_until(deadline) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
         static_cast<void>(pending.get());
      }
   };
   auto other_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, other, release, join};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, remote, release, join};
   fixture.subscribe(owner); fixture.subscribe(remote); fixture.subscribe(other);
   const auto message = fixture.publish(owner, "cached-before-native-idontwant");
   const auto id = pubsub::codec::message_id(message, config);
   fixture.connect(owner, remote); fixture.connect(owner, other);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto row = self->pubsub_value.outbound.find(peer);
      return row != self->pubsub_value.outbound.end() && row->second.stream && !row->second.snapshot_pending &&
          self->pubsub_value.outbound_budget.total() == 0 && self->pubsub_value.peer_topics.contains(peer);
   }));
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto opts = node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false};
      request = co_await remote.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v12, opts);
      suppression = co_await remote.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v12, opts);
      independent = co_await other.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v12, opts);
   }());
   auto gate = std::shared_ptr<forge::asio::gate>{};
   {
      const auto lock = std::scoped_lock{self->mutex};
      gate = self->pubsub_value.outbound.at(peer).write_gate;
   }
   auto acquire = boost::asio::co_spawn(fixture.runtime.context(), gate->acquire(), boost::asio::use_future);
   if (acquire.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   ticket = acquire.get();
   const auto initial = fixture.receipts(owner).size();
   const auto invalid_before = owner.pubsub_snapshot().invalid_messages;
   pending = boost::asio::co_spawn(fixture.runtime.context(), [&]() -> boost::asio::awaitable<bool> {
      auto generation = std::optional<std::uint64_t>{};
      co_return co_await self->send_pubsub_rpc(peer, pubsub::rpc{
          .subscriptions = {{.subject = fixture.topic}}, .messages = {message},
          .control_value = pubsub::control{.have = {{.subject = {"unrelated"}, .message_ids = {{'h'}}}}}}, generation);
   }, boost::asio::use_future);
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.outbound_budget.total() != 0;
   }));
   const auto before_request = owner.pubsub_snapshot().control_messages;
   fixture.send(request, pubsub::rpc{.control_value = pubsub::control{.want = {{.message_ids = {id}}}}});
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      const auto validation = self->pubsub_value.validations.find(std::string{id.begin(), id.end()});
      return validation != self->pubsub_value.validations.end() && validation->second.retransmissions.contains(peer) &&
          self->metrics_value.pubsub_control_messages > before_request;
   }));
   fixture.send(suppression, pubsub::rpc{.control_value = pubsub::control{.dont_want = {{.message_ids = {id}}}}});
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().idontwant_entries == 1; }));
   ticket.release();
   if (pending.wait_for(5s) != std::future_status::ready) { gossipsub_test_shutdown::fail_closed(); }
   BOOST_CHECK(pending.get()); // Unrelated subscription and IHAVE still reached the native write.
   fixture.send(request, pubsub::rpc{.subscriptions = {{.subject = {"cached-fence"}}}});
   BOOST_REQUIRE(fixture.wait([&] {
      const auto lock = std::scoped_lock{self->mutex};
      return self->pubsub_value.peer_topics.at(peer).contains("cached-fence") && self->pubsub_value.outbound_budget.total() == 0;
   }));
   auto mixed = false;
   const auto writes = fixture.receipts(owner);
   for (auto i = initial; i < writes.size(); ++i) {
      const auto& receipt = writes[i];
      if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != peer) { continue; }
      const auto value = pubsub::codec::decode(receipt.frame, config);
      BOOST_CHECK(value.messages.empty());
      if (!value.subscriptions.empty() && value.subscriptions.front().subject == fixture.topic) {
         mixed = value.control_value && value.control_value->have.size() == 1;
      }
   }
   BOOST_CHECK(mixed);
   const auto delivered_to = [&](const peer_id& recipient, std::size_t from) {
      const auto events = fixture.receipts(owner);
      for (auto i = from; i < events.size(); ++i) {
         if (events[i].kind != pubsub::trace_kind::rpc_write || events[i].peer != recipient) { continue; }
         for (const auto& value : pubsub::codec::decode(events[i].frame, config).messages) {
            if (pubsub::codec::message_id(value, config) == id) { return true; }
         }
      }
      return false;
   };
   fixture.send(independent, pubsub::rpc{.control_value = pubsub::control{.want = {{.message_ids = {id}}}}});
   BOOST_REQUIRE(fixture.wait([&] { return delivered_to(other.local_peer(), initial); }));
   for (std::size_t i = 0; i < config.limits.idontwant_ttl; ++i) {
      run(fixture.runtime, self->pubsub_heartbeat_once());
      BOOST_CHECK_EQUAL(owner.pubsub_snapshot().idontwant_entries, i + 1 == config.limits.idontwant_ttl ? 0U : 1U);
   }
   const auto after_expiry = fixture.receipts(owner).size();
   fixture.send(request, pubsub::rpc{.control_value = pubsub::control{.want = {{.message_ids = {id}}}}});
   BOOST_REQUIRE(fixture.wait([&] { return delivered_to(peer, after_expiry); }));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().invalid_messages, invalid_before);
   shutdown.join(); other_shutdown.join();
}

void node_session_fixture::native_idontwant_before_validation() {
   auto fixture = pubsub_router_fixture{};
   auto config = manual_options();
   config.preferred = pubsub::version::v1_2;
   config.signatures = pubsub::signature_policy::strict_no_sign;
   config.scoring.emplace();
   config.scoring->topics.emplace(fixture.topic, pubsub::topic_score_params{});
   config.scoring->behaviour_penalty_weight = -1.0;
   config.scoring->decay_interval = 60s;
   auto& owner = fixture.add("idw-validation-owner", config);
   auto& source = fixture.add("idw-validation-source", config);
   auto& other = fixture.add("idw-validation-other", config);
   auto entered = std::atomic_bool{false};
   auto release = std::make_shared<forge::asio::notification>();
   const auto epoch = release->epoch();
   auto input = stream{}, graft = stream{};
   const auto unblock = [&] { release->notify(); input.request_cancel(); graft.request_cancel(); };
   auto other_shutdown = gossipsub_test_shutdown{fixture.runtime, owner, other, unblock};
   auto shutdown = gossipsub_test_shutdown{fixture.runtime, owner, source, unblock};
   fixture.subscribe(owner, [&, release, epoch](pubsub::event) -> boost::asio::awaitable<pubsub::validation_result> {
      entered.store(true, std::memory_order_release);
      co_await release->async_wait(epoch);
      co_return pubsub::validation_result::accept;
   });
   fixture.subscribe(source); fixture.subscribe(other);
   fixture.connect(owner, source); fixture.connect(owner, other);
   run(fixture.runtime, [&]() -> boost::asio::awaitable<void> {
      const auto opts = node::open_options{.allow_relay = false, .timeout = 3s, .allow_hole_punch = false};
      input = co_await source.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v12, opts);
      graft = co_await other.async_open_protocol_stream(owner.local_peer(), builtins::meshsub_v12, opts);
   }());
   const auto join_mesh = pubsub::rpc{.control_value = pubsub::control{.grafts = {{.subject = fixture.topic}}}};
   fixture.send(input, join_mesh); fixture.send(graft, join_mesh);
   BOOST_REQUIRE(fixture.wait([&] {
      if (owner.pubsub_snapshot().mesh_edges != 2) { return false; }
      const auto lock = std::scoped_lock{owner.impl_->mutex};
      return owner.impl_->pubsub_value.outbound_budget.total() == 0 &&
          owner.impl_->pubsub_value.controls->bytes() == 0;
   }));
   const auto value = pubsub::message{.data = std::vector<std::uint8_t>(config.limits.idontwant_threshold, 'x'),
                                     .subject = fixture.topic};
   const auto id = pubsub::codec::message_id(value, config);
   fixture.send(input, pubsub::rpc{.messages = {value}});
   BOOST_REQUIRE(fixture.wait([&] { return entered.load(std::memory_order_acquire) && other.pubsub_snapshot().idontwant_entries == 1; }));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().messages_delivered, 0U);
   BOOST_CHECK_EQUAL(source.pubsub_snapshot().idontwant_entries, 0U);
   auto seen = false;
   for (const auto& receipt : fixture.receipts(other)) {
      if (receipt.kind != pubsub::trace_kind::rpc_read || receipt.peer != owner.local_peer()) { continue; }
      const auto rpc = pubsub::codec::decode(receipt.frame, config);
      if (!rpc.control_value) { continue; }
      for (const auto& row : rpc.control_value->dont_want) {
         seen = seen || std::ranges::find(row.message_ids, id) != row.message_ids.end();
      }
   }
   BOOST_REQUIRE(seen);
   BOOST_REQUIRE(fixture.wait([&] {
      for (const auto& receipt : fixture.receipts(owner)) {
         if (receipt.kind != pubsub::trace_kind::rpc_write || receipt.peer != other.local_peer() ||
             receipt.protocol != builtins::meshsub_v12) { continue; }
         const auto rpc = pubsub::codec::decode(receipt.frame, config);
         if (!rpc.control_value) { continue; }
         for (const auto& row : rpc.control_value->dont_want) {
            if (std::ranges::find(row.message_ids, id) != row.message_ids.end()) { return true; }
         }
      }
      return false;
   }));
   const auto before = owner.pubsub_snapshot();
   BOOST_CHECK_EQUAL(before.invalid_messages, 0U);
   const auto scores_before = owner.pubsub_scores();
   const auto peer_before = std::ranges::find(scores_before.peers, other.local_peer(), &pubsub::peer_score_snapshot::peer);
   BOOST_REQUIRE(peer_before != scores_before.peers.end());
   const auto topic_before = std::ranges::find(peer_before->topics, fixture.topic, &pubsub::topic_score_snapshot::subject);
   BOOST_REQUIRE(topic_before != peer_before->topics.end());
   BOOST_CHECK_EQUAL(topic_before->invalid_message_deliveries, 0.0);
   const auto receipts_before = fixture.receipts(owner).size();
   // This peer has actually received our IDONTWANT, but still sends the exact pending message.
   fixture.send(graft, pubsub::rpc{.messages = {value}});
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().duplicates == before.duplicates + 1; }));
   const auto receipts = fixture.receipts(owner);
   auto duplicate_read = false;
   for (auto index = receipts_before; index < receipts.size(); ++index) {
      const auto& receipt = receipts[index];
      if (receipt.kind != pubsub::trace_kind::rpc_read || receipt.peer != other.local_peer() ||
          receipt.protocol != builtins::meshsub_v12) { continue; }
      const auto rpc = pubsub::codec::decode(receipt.frame, config);
      duplicate_read = duplicate_read || std::ranges::any_of(rpc.messages, [&](const auto& message) {
         return message.subject == value.subject && message.data == value.data &&
             pubsub::codec::message_id(message, config) == id;
      });
   }
   BOOST_REQUIRE(duplicate_read);
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().messages_delivered, 0U);
   release->notify();
   BOOST_REQUIRE(fixture.wait([&] { return owner.pubsub_snapshot().messages_delivered == 1; }));
   BOOST_CHECK_EQUAL(owner.pubsub_snapshot().invalid_messages, before.invalid_messages);
   const auto scores_after = owner.pubsub_scores();
   const auto peer_after = std::ranges::find(scores_after.peers, other.local_peer(), &pubsub::peer_score_snapshot::peer);
   BOOST_REQUIRE(peer_after != scores_after.peers.end());
   const auto topic_after = std::ranges::find(peer_after->topics, fixture.topic, &pubsub::topic_score_snapshot::subject);
   BOOST_REQUIRE(topic_after != peer_after->topics.end());
   BOOST_CHECK_EQUAL(topic_after->invalid_message_deliveries, topic_before->invalid_message_deliveries);
   BOOST_CHECK_EQUAL(topic_after->weighted_score, topic_before->weighted_score);
   BOOST_CHECK_EQUAL(peer_after->behaviour_penalty, peer_before->behaviour_penalty);
   BOOST_CHECK_EQUAL(peer_after->value, peer_before->value);
   shutdown.join(); other_shutdown.join();
}

} // namespace forge::net::p2p

BOOST_AUTO_TEST_SUITE(pubsub_extensions)
BOOST_AUTO_TEST_CASE(isolated_meshsub_protocol_projects_pubsub_and_gossipsub_service) {
   using namespace forge::net::p2p;
   for (const auto& protocol : {builtins::meshsub_v10, builtins::meshsub_v11,
                              builtins::meshsub_v12, builtins::meshsub_v13}) {
      // A peer advertising only v1.2 or v1.3 must not rely on legacy fallback entries.
      const auto protocols = std::array{protocol};
      BOOST_CHECK(capabilities_for(protocols).has(capabilities::pubsub));
      BOOST_CHECK_EQUAL(detail::resource_service_id(protocol, false), "p2p.gossipsub");
   }
   const auto unrelated = std::array{builtins::identify, builtins::ping, protocol_id{"/meshsub/9.9.9"},
                                     protocol_id{"/custom/1.0.0"}};
   BOOST_CHECK(!capabilities_for(unrelated).has(capabilities::pubsub));
   BOOST_CHECK_EQUAL(detail::resource_service_id(unrelated.back(), false), "p2p.custom:/custom/1.0.0");
}
BOOST_AUTO_TEST_CASE(raw_id_budgets_rpc_limits_ttl_and_chunk_charges) { forge::net::p2p::node_session_fixture::idontwant_bounds(); }
BOOST_AUTO_TEST_CASE(native_v13_first_snapshot_and_stream_reconnect) { forge::net::p2p::node_session_fixture::native_extension_versions(forge::net::p2p::pubsub::version::v1_3); }
BOOST_AUTO_TEST_CASE(native_v13_fallback_v12) { forge::net::p2p::node_session_fixture::native_extension_versions(forge::net::p2p::pubsub::version::v1_2); }
BOOST_AUTO_TEST_CASE(native_v13_fallback_v11) { forge::net::p2p::node_session_fixture::native_extension_versions(forge::net::p2p::pubsub::version::v1_1); }
BOOST_AUTO_TEST_CASE(native_v13_fallback_v10) { forge::net::p2p::node_session_fixture::native_extension_versions(forge::net::p2p::pubsub::version::v1_0); }
BOOST_AUTO_TEST_CASE(native_first_rpc_absent_unknown_repeat_and_duplicate) { forge::net::p2p::node_session_fixture::native_extension_first_rpc(); }
BOOST_AUTO_TEST_CASE(native_idontwant_after_gate_suppresses_cached_reply_but_preserves_controls_peers_and_ttl) { forge::net::p2p::node_session_fixture::native_idontwant_suppression(); }
BOOST_AUTO_TEST_CASE(native_idontwant_is_sent_before_application_validation_finishes) { forge::net::p2p::node_session_fixture::native_idontwant_before_validation(); }
BOOST_AUTO_TEST_SUITE_END()
