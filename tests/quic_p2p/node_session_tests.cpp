module;

#include <boost/test/unit_test.hpp>
#include <boost/asio/use_future.hpp>
#include <future>
#include <thread>
#include "libp2p_identity_fixture.hxx"

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.connection_gater;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/direct_transport.hxx"
#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/node_impl.hxx"
#include "../../libraries/net/p2p/details/node_impl_autonat_operation.hxx"
#include "../../libraries/net/p2p/details/observed_address_manager.hxx"
#include "../../libraries/net/p2p/details/owner_cancellation.hxx"
#include "../../libraries/net/p2p/details/path_selector.hxx"
#include "../../libraries/net/p2p/details/peer_exchange_codec.hxx"
#include "../../libraries/net/p2p/details/peer_failure.hxx"
#include "../../libraries/net/p2p/details/resource_stream.hxx"
#include "../../libraries/net/p2p/details/reachability_manager.hxx"
#include "../../libraries/net/p2p/details/session_lifecycle.hxx"
#include "../../libraries/net/p2p/details/session_retirement.hxx"


namespace forge::net::p2p {
namespace {

struct close_barrier {
   forge::asio::notification changed;
   std::atomic_bool entered{false};
   std::atomic_bool released{false};
   std::atomic_size_t opens{0};
   std::atomic_size_t accepts{0};
   std::atomic_size_t closes{0};
};

class admission_transport final : public forge::net::transport::detail::session_concept {
 public:
   admission_transport(std::shared_ptr<close_barrier> state, std::shared_ptr<void> native, bool fail_close = false)
       : state_(std::move(state)), native_(std::move(native)), fail_close_(fail_close) {}

   bool valid() const noexcept override { return open_.load(); }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      ++state_->opens;
      FORGE_THROW_EXCEPTION(exceptions::connection_rejected, "fixture rejects native stream open");
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      ++state_->accepts;
      co_return forge::net::transport::stream{};
   }

   boost::asio::awaitable<void> async_close() override {
      ++state_->closes;
      state_->entered = true;
      for (;;) {
         const auto epoch = state_->changed.epoch();
         if (state_->released.load()) {
            break;
         }
         co_await state_->changed.async_wait(epoch);
      }
      open_ = false;
      if (fail_close_) { FORGE_THROW_EXCEPTION(exceptions::internal, "fixture terminal session close failure"); }
   }

   void cancel() override { open_ = false; }

 private:
   std::shared_ptr<close_barrier> state_;
   std::shared_ptr<void> native_;
   std::atomic_bool open_{true};
   bool fail_close_ = false;
};

enum class stream_failure { ping, observer_mismatch, negotiation, binding };

class autonat_upgrade_refusal final : public connection_gater {
 public:
   explicit autonat_upgrade_refusal(peer_id expected) : expected_(std::move(expected)) {}

   bool intercept_address_dial(const peer_id& peer, const endpoint& address) noexcept override {
      return peer == expected_ && address.transport.host == "127.0.0.1";
   }
   bool intercept_secured(connection_direction direction, const peer_id& peer,
                           const connection_endpoints&) noexcept override {
      if (direction == connection_direction::outbound && peer == expected_) { ++secured; }
      return true;
   }
   bool intercept_upgraded(connection_direction direction, const peer_id& peer,
                            const connection_endpoints&) noexcept override {
      if (direction == connection_direction::outbound) {
         if (peer == expected_) { ++upgraded; }
         return false;
      }
      return true;
   }

   std::atomic_size_t secured{0};
   std::atomic_size_t upgraded{0};

 private:
   peer_id expected_;
};

class reachability_stream final : public forge::net::transport::detail::stream_concept {
 public:
   reachability_stream(protocol_id protocol, std::shared_ptr<close_barrier> barrier, std::shared_ptr<void> native,
                       bool refuse_protocol = false)
       : barrier_(std::move(barrier)), native_(std::move(native)) {
      reply_ = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          {.kind = protocol_negotiation::message_kind::header, .protocol = protocol_negotiation::multistream_v1}));
      const auto selected = protocol_negotiation::encode_frame(protocol_negotiation::encode_message(
          {.kind = refuse_protocol ? protocol_negotiation::message_kind::not_available
                                   : protocol_negotiation::message_kind::protocol,
           .protocol = std::move(protocol)}));
      reply_.insert(reply_.end(), selected.begin(), selected.end());
   }
   bool valid() const noexcept override { return true; }
   std::int64_t id() const noexcept override { return 1; }
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t>) override { co_return; }
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      if (!reply_.empty()) { co_return std::exchange(reply_, {}); }
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "reachability fixture primary read error");
   }
   boost::asio::awaitable<void> async_close() override {
      barrier_->entered = true;
      barrier_->changed.notify();
      while (!barrier_->released.load()) {
         const auto epoch = barrier_->changed.epoch();
         if (!barrier_->released.load()) { co_await barrier_->changed.async_wait(epoch); }
      }
      FORGE_THROW_EXCEPTION(exceptions::connection_rejected, "reachability fixture secondary close error");
   }
   void cancel() override {}

 private:
   std::shared_ptr<close_barrier> barrier_;
   std::shared_ptr<void> native_;
   std::vector<std::uint8_t> reply_;
};

class reachability_transport final : public forge::net::transport::detail::session_concept {
 public:
   explicit reachability_transport(forge::net::transport::stream stream,
                                  std::shared_ptr<std::atomic_size_t> opens = {})
       : stream_(std::move(stream)), opens_(std::move(opens)) {}
   bool valid() const noexcept override { return true; }
   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      if (opens_) { ++*opens_; }
      co_return std::move(stream_);
   }
   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      FORGE_THROW_EXCEPTION(exceptions::closed, "fixture has no inbound streams");
      co_return forge::net::transport::stream{};
   }
   boost::asio::awaitable<void> async_close() override { co_return; }
   void cancel() override {}

 private:
   forge::net::transport::stream stream_;
   std::shared_ptr<std::atomic_size_t> opens_;
};

template <typename T> T bounded_result(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   return result.get();
}

template <typename Predicate>
bool drive_until(boost::asio::io_context& context, Predicate predicate,
                 std::chrono::milliseconds timeout = std::chrono::seconds{2}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate() && std::chrono::steady_clock::now() < deadline) {
      context.restart();
      context.poll();
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }
   return predicate();
}

node::options fixture_options(std::string_view name) {
   auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = node::options{};
   options.certificate_pem = std::move(identity.certificate_pem);
   options.private_key_pem = std::move(identity.private_key_pem);
   options.peer_state.persistence = peer_store::make_memory_persistence();
   return options;
}

} // namespace

// The friendship permits this test TU to reach existing admission and session
// state. No production option, runtime callback or alternate admission is added.
struct node_session_fixture {
   static void autonat_local_upgrade_refusal_is_not_negative() {
      for (const auto v2 : {false, true}) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
         auto server = node{runtime, passive_reachability_options("autonat-upgrade-server")};
         auto options = passive_reachability_options("autonat-upgrade-client");
         auto gate = std::make_shared<autonat_upgrade_refusal>(server.local_peer());
         options.connection_gater = gate;
         auto client = node{runtime, std::move(options)};
         const auto self = client.impl_;
         auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
            for (auto* owner : {&client, &server}) {
               try { bounded_result(runtime, owner->async_stop()); }
               catch (const std::exception& error) { BOOST_ERROR("AutoNAT gater fixture cleanup: " << error.what()); }
               catch (...) { BOOST_ERROR("AutoNAT gater fixture cleanup failed"); }
            }
         }};
         bounded_result(runtime, server.async_listen(parse_endpoint("/ip4/127.0.0.1/tcp/0")));
         const auto target = server.local_endpoint();
         BOOST_REQUIRE(target);
         auto operation = std::make_shared<node::impl::autonat_operation>();
         operation->owner = self;
         operation->peer = server.local_peer();
         operation->protocol = v2 ? node::impl::autonat_protocol::v2_request : node::impl::autonat_protocol::v1;
         operation->deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
         // Exercise the actual dial-back helper below service address filtering.
         // Loopback is a local policy-error control, never public NAT evidence.
         auto probe = boost::asio::co_spawn(runtime.context(), self->probe_autonat(operation, *target,
             v2 ? std::optional<std::uint64_t>{1} : std::nullopt), boost::asio::use_future);
         BOOST_REQUIRE(probe.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
         BOOST_CHECK_EXCEPTION(static_cast<void>(probe.get()), forge::exceptions::base, [](const auto& error) {
            return exceptions::code_of(error) == exceptions::code::connection_rejected;
         });
         BOOST_TEST(gate->secured.load() == 1U);
         BOOST_TEST(gate->upgraded.load() == 1U);
         BOOST_TEST(self->resources.current().active_dials == 0U);
         BOOST_TEST(self->resources.current().system.outbound_connections == 0U);
         BOOST_TEST(self->resources.current().system.outbound_streams == 0U);
         BOOST_TEST(!operation->attempt.connection.session.valid());
      }
   }

   static node::options passive_reachability_options(std::string_view name = "reachability-generation-owner") {
      auto options = fixture_options(name);
      options.reachability_policy.client_v1_enabled = false;
      options.reachability_policy.client_v2_enabled = false;
      options.reachability_policy.ping_enabled = false;
      return options;
   }

   static boost::asio::awaitable<detail::reachability_manager::probe_result>
   published_snapshot_probe(std::vector<endpoint> addresses) {
      co_return detail::reachability_manager::probe_result{
          .result = {.value = reachability::state::publicly_reachable, .observed = addresses.front()},
          .verified_dialback = true, .internet_scope = true};
   }

   static void getter_and_subscription_share_published_snapshot() {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, passive_reachability_options()};
      const auto self = owner.impl_;
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         try { bounded_result(runtime, owner.async_stop()); }
         catch (...) { BOOST_ERROR("published snapshot fixture failed cleanup"); }
      }};
      auto seconds = std::make_shared<std::atomic<std::int64_t>>(0);
      auto publications = std::make_shared<std::atomic_size_t>(0);
      auto policy = self->options.reachability_policy;
      policy.client_v2_enabled = true;
      const auto weak = std::weak_ptr<node::impl>{self};
      self->reachability_manager_value = std::make_shared<detail::reachability_manager>(
          runtime.context().get_executor(), policy, detail::reachability_manager::callbacks{
              .observers = [] { return std::vector<detail::reachability_manager::observer>{}; },
              .candidates = [weak] { return weak.lock()->reachability_manager_value->candidates(); },
              .exchange = [](detail::reachability_manager::observer, bool, std::vector<endpoint> addresses,
                              std::shared_ptr<cancellation_latch>) { return published_snapshot_probe(std::move(addresses)); },
              // Hold publication, not state commitment. This existing callback
              // boundary and explicit clock make the two snapshots observable.
              .changed = [publications](host_event) { ++*publications; },
              .finished = [weak] { if (const auto value = weak.lock()) { value->finish_reachability(); } },
              .now = [seconds] { return std::chrono::steady_clock::time_point{} + std::chrono::seconds{seconds->load()}; },
          });
      owner.set_advertised_endpoints({parse_endpoint("/ip4/8.8.8.8/tcp/4001")});
      self->start_reachability();
      auto subscription = owner.host_events();
      auto published = bounded_result(runtime, subscription.async_read());
      BOOST_REQUIRE(published);
      const auto matches = [&](const host_event& expected) {
         const auto actual = owner.reachability_status();
         BOOST_TEST(actual.generation == expected.generation);
         BOOST_CHECK(actual.phase == expected.phase);
         BOOST_CHECK(actual.effective == expected.effective);
         BOOST_CHECK(actual.autonat_v1 == expected.autonat_v1);
         BOOST_TEST(actual.resync_required == expected.resync_required);
         BOOST_CHECK(std::ranges::equal(actual.autonat_v2, expected.autonat_v2, [](const auto& a, const auto& b) {
            return a.value == b.value && a.address.to_string() == b.address.to_string();
         }));
         BOOST_CHECK(std::ranges::equal(actual.confirmed_addresses, expected.confirmed_addresses, [](const auto& a, const auto& b) {
            return a.to_string() == b.to_string();
         }));
      };
      const auto source = detail::reachability_manager::observer{
          .peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, 47)}),
          .remote = parse_endpoint("/ip4/11.0.0.1/tcp/4001"), .v2 = true};
      const auto result = bounded_result(runtime, self->reachability_manager_value->async_probe(source));
      BOOST_CHECK(result.value == reachability::state::publicly_reachable);
      BOOST_TEST(publications->load() >= 1U);
      BOOST_REQUIRE(self->reachability_manager_value->current().effective == reachability::state::publicly_reachable);
      matches(*published);

      self->publish_host_state({});
      const auto previous = published->generation;
      published = bounded_result(runtime, subscription.async_read());
      BOOST_REQUIRE(published);
      BOOST_TEST(published->generation > previous);
      BOOST_REQUIRE(published->effective == reachability::state::publicly_reachable);
      matches(*published);

      *seconds = std::chrono::duration_cast<std::chrono::seconds>(policy.observation_ttl).count() + 1;
      BOOST_REQUIRE(self->reachability_manager_value->current().autonat_v2.empty());
      matches(*published);
      self->publish_host_state({});
      const auto positive_generation = published->generation;
      published = bounded_result(runtime, subscription.async_read());
      BOOST_REQUIRE(published);
      BOOST_TEST(published->generation > positive_generation);
      BOOST_REQUIRE(published->effective == reachability::state::unknown);
      BOOST_TEST(published->autonat_v2.empty());
      matches(*published);
      subscription.close();
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
   }

   static endpoint seed_confirmations(const std::shared_ptr<node::impl>& self,
                                     std::chrono::steady_clock::time_point now) {
      const auto local = parse_endpoint("/ip4/10.0.0.2/tcp/4001");
      const auto reported = parse_endpoint("/ip4/8.8.8.8/tcp/8000");
      const auto listened = std::vector<endpoint>{parse_endpoint("/ip4/0.0.0.0/tcp/4001")};
      const auto lock = std::scoped_lock{self->mutex};
      // Pure bounded observation state: these endpoints never open a socket.
      // Keep the real four-observer quorum and listener/transport validation.
      for (std::uint8_t id = 1; id <= 4; ++id) {
         const auto peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, id)});
         BOOST_REQUIRE(self->observed_addresses->observe(id, peer, local,
             parse_endpoint("/ip4/11.0.0." + std::to_string(id) + "/tcp/4001"), reported, listened, now));
      }
      self->refresh_reachability_locked();
      return reported;
   }

   static void advertised_mutations_own_generation_before_start() {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, passive_reachability_options()};
      const auto self = owner.impl_;
      const auto first = parse_endpoint("/ip4/8.8.8.8/tcp/4001");
      owner.set_advertised_endpoints({first});
      const auto initial = self->reachability_manager_value->candidates();
      BOOST_REQUIRE_EQUAL(initial.addresses.size(), 1U);
      owner.set_advertised_endpoints({parse_endpoint("/ip4/8.8.4.4/tcp/4001")});
      owner.set_advertised_endpoints({first});
      const auto returned = self->reachability_manager_value->candidates();
      BOOST_TEST(returned.generation == initial.generation + 2);
      owner.set_advertised_endpoints({first});
      BOOST_TEST(self->reachability_manager_value->candidates().generation == returned.generation);
      BOOST_TEST(!self->reachability_started);
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
      bounded_result(runtime, owner.async_stop());
      BOOST_CHECK_THROW(static_cast<void>(self->reachability_manager_value->set_addresses({})), exceptions::closed);
   }

   static void prestart_observations(bool expired, bool failed_start) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, passive_reachability_options()};
      const auto self = owner.impl_;
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         try { bounded_result(runtime, owner.async_stop()); }
         catch (...) { BOOST_ERROR("prestart observation fixture failed cleanup"); }
      }};
      const auto now = std::chrono::steady_clock::now();
      const auto observed_at = expired ? now - self->options.reachability_policy.observation_ttl * 2 : now;
      const auto reported = seed_confirmations(self, observed_at);
      BOOST_TEST(self->observed_addresses->confirmed(observed_at).size() == 1U);
      BOOST_TEST(owner.reachability_status().confirmed_addresses.empty());
      BOOST_TEST(self->reachability_manager_value->candidates().addresses.empty());
      if (failed_start) {
         self->lifecycle.request_stop();
         BOOST_CHECK_THROW(self->start_reachability(), exceptions::closed);
      } else {
         self->start_reachability();
         const auto confirmed = owner.reachability_status().confirmed_addresses;
         BOOST_TEST(confirmed.size() == (expired ? 0U : 1U));
         if (!expired) {
            BOOST_REQUIRE_EQUAL(confirmed.size(), 1U);
            BOOST_TEST(confirmed.front().to_string() == reported.to_string());
         }
         self->reachability_manager_value->request_stop();
         bounded_result(runtime, self->join_reachability());
      }
      BOOST_TEST(owner.reachability_status().confirmed_addresses.empty());
      BOOST_TEST(self->reachability_manager_value->candidates().addresses.empty());
      BOOST_CHECK_THROW(static_cast<void>(self->reachability_manager_value->set_addresses({})), exceptions::closed);
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
   }

   static void retirement_revokes_confirmation_before_transport_close() {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, passive_reachability_options()};
      const auto self = owner.impl_;
      auto context = boost::asio::io_context{};
      auto barrier = std::make_shared<close_barrier>();
      auto session = std::make_shared<node::impl::session_state>();
      session->id = 1;
      session->info.remote_peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, 1)});
      session->info.path = path::kind::direct;
      session->connection = forge::net::transport::detail::session_access::make(
          std::make_shared<admission_transport>(barrier, std::make_shared<int>(1)));
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         barrier->released = true;
         barrier->changed.notify();
         try {
            context.restart();
            context.run_for(std::chrono::seconds{2});
            bounded_result(runtime, owner.async_stop());
         } catch (...) { BOOST_ERROR("observation retirement fixture failed cleanup"); }
      }};
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->sessions.emplace(session->id, session);
      }
      static_cast<void>(seed_confirmations(self, std::chrono::steady_clock::now()));
      self->start_reachability();
      BOOST_REQUIRE_EQUAL(owner.reachability_status().confirmed_addresses.size(), 1U);
      const auto generation = self->reachability_manager_value->candidates().generation;
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_REQUIRE(self->retire_session_locked(session, true) == session);
         BOOST_TEST(self->confirmed_observed_addresses.empty());
         BOOST_TEST(self->reachability_manager_value->candidates().generation == generation + 1);
         BOOST_TEST(self->reachability_manager_value->candidates().addresses.empty());
      }
      BOOST_TEST(!barrier->entered.load());
      BOOST_TEST(owner.reachability_status().confirmed_addresses.empty());
      auto retired = boost::asio::co_spawn(context, self->async_retire_session(session, false), boost::asio::use_future);
      BOOST_REQUIRE(drive_until(context, [&] { return barrier->entered.load(); }));
      BOOST_CHECK(retired.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
      BOOST_TEST(owner.reachability_status().confirmed_addresses.empty());
      barrier->released = true;
      barrier->changed.notify();
      BOOST_REQUIRE(drive_until(context, [&] {
         return retired.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
      }));
      BOOST_CHECK_NO_THROW(retired.get());
      BOOST_TEST(!session->connection.valid());
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
   }

   static void refresh_after_result_admission_closes() {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, passive_reachability_options()};
      const auto self = owner.impl_;
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         try { bounded_result(runtime, owner.async_stop()); }
         catch (const std::exception& error) { BOOST_ERROR("closed-admission fixture cleanup: " << error.what()); }
         catch (...) { BOOST_ERROR("closed-admission fixture cleanup failed"); }
      }};
      static_cast<void>(seed_confirmations(self, std::chrono::steady_clock::now()));
      self->start_reachability();
      BOOST_REQUIRE_EQUAL(owner.reachability_status().confirmed_addresses.size(), 1U);
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_REQUIRE(!self->reachability_finished);
         // Reproduce request_stop's first transition before the completion
         // callback can acquire the node mutex. No cancellation under this lock.
         self->reachability_manager_value->close_results();
         BOOST_CHECK_THROW(static_cast<void>(self->reachability_manager_value->set_addresses({})), exceptions::closed);
         BOOST_CHECK_NO_THROW(self->refresh_reachability_locked());
         BOOST_TEST(self->reachability_finished);
         BOOST_TEST(!self->reachability_started);
         BOOST_TEST(self->confirmed_observed_addresses.empty());
         BOOST_TEST(self->reachability_manager_value->candidates().addresses.empty());
      }
      self->reachability_manager_value->request_stop();
      BOOST_CHECK_NO_THROW(bounded_result(runtime, self->join_reachability()));
      BOOST_TEST(owner.reachability_status().confirmed_addresses.empty());
      BOOST_CHECK_THROW(static_cast<void>(self->reachability_manager_value->set_addresses({})), exceptions::closed);
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
   }

   static void graceful_quic_shutdown_closes_sessions_before_listener(bool fail_close) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
      auto server = node{runtime, passive_reachability_options("graceful-quic-server")};
      auto client = node{runtime, passive_reachability_options("graceful-quic-client")};
      const auto self = server.impl_;
      auto barrier = std::make_shared<close_barrier>();
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         barrier->released = true;
         barrier->changed.notify();
         try { bounded_result(runtime, server.async_stop()); }
         catch (...) { if (!fail_close) { BOOST_ERROR("graceful QUIC server cleanup failed"); } }
         try { bounded_result(runtime, client.async_stop()); }
         catch (...) { BOOST_ERROR("graceful QUIC client cleanup failed"); }
      }};
      bounded_result(runtime, server.async_listen(parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1")));
      const auto address = server.local_endpoint();
      BOOST_REQUIRE(address);
      bounded_result(runtime, client.async_connect(*address));
      const auto remote_session = client.impl_->session_for_path(server.local_peer(), path::kind::direct);
      BOOST_REQUIRE(remote_session);
      BOOST_REQUIRE(self->session_for_path(client.local_peer(), path::kind::direct));

      // Hold an already-retiring owner ahead of the active real QUIC session.
      // Both shutdown callers must join it without killing the shared UDP socket.
      auto retiring = std::make_shared<node::impl::session_state>();
      retiring->info.remote_peer = client.local_peer();
      retiring->info.path = path::kind::direct;
      retiring->connection = forge::net::transport::detail::session_access::make(
          std::make_shared<admission_transport>(barrier, std::make_shared<int>(1), fail_close));
      {
         const auto lock = std::scoped_lock{self->mutex};
         retiring->id = self->next_session_id++;
         self->sessions.emplace(retiring->id, retiring);
         BOOST_REQUIRE(self->retire_session_locked(retiring, false) == retiring);
      }
      auto first = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
      auto context = boost::asio::io_context{};
      BOOST_REQUIRE(drive_until(context, [&] { return barrier->entered.load(); }));
      auto second = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
      BOOST_CHECK(second.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout);
      BOOST_CHECK(first.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
      BOOST_TEST(self->direct_registry.listening());
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_TEST(self->session_admission_closed);
         BOOST_TEST(self->sessions.empty());
         BOOST_TEST(self->retiring_sessions.contains(retiring->id));
      }
      barrier->released = true;
      barrier->changed.notify();
      for (auto* stopped : {&first, &second}) {
         BOOST_REQUIRE(stopped->wait_for(std::chrono::seconds{5}) == std::future_status::ready);
         if (fail_close) { BOOST_CHECK_THROW(stopped->get(), exceptions::internal); }
         else { BOOST_CHECK_NO_THROW(stopped->get()); }
      }
      BOOST_TEST(barrier->closes.load() == 1U);
      BOOST_TEST(!self->direct_registry.listening());
      // No new stream, health ping or idle timeout may be needed to discover closure.
      BOOST_REQUIRE(drive_until(context, [&] { return remote_session->closed.load(); }));
      BOOST_TEST(!client.impl_->session_for_path(server.local_peer(), path::kind::direct));
      {
         const auto lock = std::scoped_lock{self->mutex};
         BOOST_TEST(self->sessions.empty());
         BOOST_TEST(self->retiring_sessions.empty());
      }
      if (fail_close) { BOOST_CHECK_THROW(bounded_result(runtime, server.async_stop()), exceptions::internal); }
      else { BOOST_CHECK_NO_THROW(bounded_result(runtime, server.async_stop())); }
   }

   static void reachability_pins_qualified_session(bool retire_selected) {
      for (const auto protocol : {builtins::autonat_v1, builtins::autonat_v2_dial_request}) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
         auto options = passive_reachability_options("reachability-session-pin");
         options.reachability_policy.client_v1_enabled = true;
         options.reachability_policy.client_v2_enabled = true;
         options.reachability_policy.max_observers = 1;
         options.reachability_policy.min_observers = 1;
         options.reachability_policy.min_reachability_observers = 1;
         options.reachability_policy.max_pending_probes = 1;
         options.reachability_policy.ping_enabled = true;
         auto owner = node{runtime, std::move(options)};
         const auto self = owner.impl_;
         const auto peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, 43)});
         const auto control_opens = std::make_shared<std::atomic_size_t>(0);
         const auto dialback_opens = std::make_shared<std::atomic_size_t>(0);
         auto barrier = std::make_shared<close_barrier>();
         barrier->released = true;
         const auto make_session = [&](std::uint64_t id, auto opens) {
            auto session = std::make_shared<node::impl::session_state>();
            session->id = id;
            session->info.remote_peer = peer;
            session->info.path = path::kind::direct;
            session->info.identify_state = identify::state::identified;
            session->authentication = peer_authentication::noise;
            session->remote_endpoint = parse_endpoint(id == 1 ? "/ip4/11.0.0.1/tcp/4001" : "/ip4/11.0.0.1/tcp/5001");
            session->remote_protocols = {protocol};
            auto stream = forge::net::transport::detail::stream_access::make(
                std::make_shared<reachability_stream>(protocol, barrier, std::make_shared<int>(1)));
            session->connection = forge::net::transport::detail::session_access::make(
                std::make_shared<reachability_transport>(std::move(stream), std::move(opens)));
            return session;
         };
         const auto control = make_session(1, control_opens);
         const auto dialback = make_session(2, dialback_opens);
         auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
            try {
               {
                  const auto lock = std::scoped_lock{self->mutex};
                  self->sessions.clear();
               }
               bounded_result(runtime, self->async_retire_session(control, true));
               bounded_result(runtime, self->async_retire_session(dialback, true));
               bounded_result(runtime, owner.async_stop());
            } catch (...) { BOOST_ERROR("reachability session pin fixture failed to join cleanup"); }
         }};
         auto selected_id = std::uint64_t{};
         {
            const auto lock = std::scoped_lock{self->mutex};
            self->sessions.emplace(control->id, control);
            const auto selected = self->reachability_session_locked(peer);
            BOOST_REQUIRE(selected == control);
            selected_id = selected->id;
            // The independent same-peer dialback arrives after selection but before open.
            self->sessions.emplace(dialback->id, dialback);
            if (!retire_selected) { dialback->remote_protocols.clear(); }
            BOOST_CHECK(self->reachability_session_locked(peer) == control);
            const auto observers = self->reachability_sessions_locked();
            BOOST_REQUIRE_EQUAL(observers.size(), 1U);
            BOOST_CHECK(observers.front() == control);
            // Capacity cannot stop scanning before a later, stronger session of an admitted peer.
            control->remote_protocols = {builtins::ping};
            dialback->remote_protocols = {protocol};
            const auto replaced = self->reachability_sessions_locked();
            BOOST_REQUIRE_EQUAL(replaced.size(), 1U);
            BOOST_CHECK(replaced.front() == dialback);
            control->remote_protocols = {protocol};
            if (!retire_selected) { dialback->remote_protocols.clear(); }
            if (retire_selected) { self->sessions.erase(control->id); }
         }
         auto operation = node::impl::open_reachability_stream_owned(self, peer, protocol,
             std::chrono::seconds{1}, std::make_shared<cancellation_latch>(), selected_id);
         if (retire_selected) {
            // A live, fully qualified replacement must not hide retirement of the pinned ID.
            auto failure = std::exception_ptr{};
            auto unexpected = std::optional<node::impl::opened_direct_stream>{};
            try { unexpected.emplace(bounded_result(runtime, std::move(operation))); }
            catch (...) { failure = std::current_exception(); }
            if (unexpected) {
               // Join the wrongly selected stream even when running the pre-fix RED policy.
               try { bounded_result(runtime, unexpected->stream.async_close()); } catch (...) {}
            }
            BOOST_REQUIRE(failure);
            BOOST_CHECK_THROW(std::rethrow_exception(failure), exceptions::closed);
            BOOST_TEST(control_opens->load() == 0U);
         } else {
            auto opened = bounded_result(runtime, std::move(operation));
            BOOST_REQUIRE(opened.stream.valid());
            BOOST_REQUIRE(opened.remote_endpoint);
            BOOST_TEST(opened.remote_endpoint->to_multiaddr().to_string() == control->remote_endpoint->to_multiaddr().to_string());
            BOOST_CHECK_THROW(bounded_result(runtime, opened.stream.async_close()), exceptions::connection_rejected);
            BOOST_TEST(control_opens->load() == 1U);
         }
         BOOST_TEST(dialback_opens->load() == 0U);
         BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
         BOOST_TEST(self->resources.current().system.outbound_streams == 0U);
      }
   }

   static void background_reachability_never_reconnects() {
      for (auto version = 0; version < 3; ++version) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
         auto client_options = fixture_options("background-reachability-client");
         client_options.reachability_policy.client_v1_enabled = false;
         client_options.reachability_policy.client_v2_enabled = false;
         client_options.reachability_policy.ping_enabled = false;
         auto server_options = fixture_options("background-reachability-server");
         server_options.reachability_policy = client_options.reachability_policy;
         server_options.reachability_policy.service_v1_enabled = true;
         server_options.reachability_policy.service_v2_enabled = true;
         auto client = node{runtime, std::move(client_options)};
         auto server = node{runtime, std::move(server_options)};
         auto self = client.impl_;
         auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
            try {
               bounded_result(runtime, client.async_stop());
               bounded_result(runtime, server.async_stop());
            } catch (...) { BOOST_ERROR("background reachability fixture failed to stop"); }
         }};
         bounded_result(runtime, server.async_listen(parse_endpoint("/ip4/127.0.0.1/tcp/0")));
         const auto address = server.local_endpoint();
         BOOST_REQUIRE(address);
         const auto connected = bounded_result(runtime, client.async_connect(*address));
         BOOST_CHECK(connected.remote_peer == server.local_peer());
         auto cached = self->session_for_path(server.local_peer(), path::kind::direct);
         BOOST_REQUIRE(cached);
         BOOST_REQUIRE(cached->authentication != peer_authentication::unverified);
         BOOST_REQUIRE(cached->info.identify_state == identify::state::identified);
         auto cancellation = std::make_shared<cancellation_latch>();
         if (version == 0) {
            const auto before_ping = client.metrics().path_direct_attempts;
            bounded_result(runtime, node::impl::ping_reachability_owned(self, server.local_peer(), cancellation));
            BOOST_TEST(client.metrics().path_direct_attempts == before_ping);
         }
         self->forget_session(cached);
         bounded_result(runtime, self->async_retire_session(cached, true));
         BOOST_REQUIRE(!self->session_for_path(server.local_peer(), path::kind::direct));
         // Keep the actual listener and its learned root: the old helper can
         // reconnect successfully, so an unavailable network cannot hide it.
         BOOST_REQUIRE(client.peers().find(server.local_peer()));
         const auto before = client.metrics();
         if (version == 0) {
            BOOST_CHECK_THROW(bounded_result(runtime,
                node::impl::ping_reachability_owned(self, server.local_peer(), cancellation)), exceptions::closed);
         } else {
            BOOST_CHECK_THROW(static_cast<void>(bounded_result(runtime,
                node::impl::exchange_reachability_owned(self, server.local_peer(), *address, version == 2,
                    {parse_endpoint("/ip4/127.0.0.1/tcp/4001")}, cancellation, cached->id))), exceptions::closed);
         }
         const auto after = client.metrics();
         BOOST_TEST(after.path_direct_attempts == before.path_direct_attempts);
         BOOST_TEST(after.sessions_opened == before.sessions_opened);
         BOOST_TEST(after.handshakes_completed == before.handshakes_completed);
         BOOST_TEST(self->resources.current().active_dials == 0U);
         BOOST_TEST(!self->session_for_path(server.local_peer(), path::kind::direct));
      }
   }

   static void reachability_error_awaits_stream_cleanup(stream_failure failure) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto options = fixture_options("reachability-terminal-owner");
      if (failure == stream_failure::binding) {
         options.limits.resources.protocol.max_outbound_streams = 0;
      }
      auto owner = node{runtime, std::move(options)};
      auto self = owner.impl_;
      auto context = boost::asio::io_context{};
      auto barrier = std::make_shared<close_barrier>();
      auto native = std::make_shared<int>(1);
      const auto native_weak = std::weak_ptr<int>{native};
      const auto protocol = failure == stream_failure::observer_mismatch ? builtins::autonat_v1 : builtins::ping;
      auto transport = forge::net::transport::detail::stream_access::make(
          std::make_shared<reachability_stream>(protocol, barrier, std::move(native), failure == stream_failure::negotiation));
      auto session = std::make_shared<node::impl::session_state>();
      session->id = 1;
      session->info.remote_peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, 43)});
      session->info.path = path::kind::direct;
      session->info.identify_state = identify::state::identified;
      session->authentication = peer_authentication::noise;
      session->remote_endpoint = parse_endpoint("/ip4/11.0.0.1/tcp/4001");
      session->remote_protocols = {protocol};
      session->connection = forge::net::transport::detail::session_access::make(
          std::make_shared<reachability_transport>(std::move(transport)));
      {
         const auto lock = std::scoped_lock{self->mutex};
         self->sessions.emplace(session->id, session);
      }
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         barrier->released = true;
         barrier->changed.notify();
         try {
            context.restart();
            context.run_for(std::chrono::seconds{2});
            {
               const auto lock = std::scoped_lock{self->mutex};
               self->sessions.erase(session->id);
            }
            bounded_result(runtime, owner.async_stop());
         } catch (...) { BOOST_ERROR("reachability terminal fixture failed cleanup"); }
      }};
      auto cancellation = boost::asio::cancellation_signal{};
      auto result = boost::asio::co_spawn(context, reachability_cleanup_operation(self, session, failure),
          boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
      BOOST_REQUIRE(drive_until(context, [&] {
         return barrier->entered.load() || result.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
      }));
      if (!barrier->entered.load()) {
         try {
            result.get();
         } catch (const std::exception& error) {
            BOOST_FAIL("reachability completed before its cleanup barrier: " << error.what());
         }
         BOOST_FAIL("reachability succeeded without entering its cleanup barrier");
      }
      BOOST_CHECK(result.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
      BOOST_TEST(self->resources.current().system.outbound_streams == 1U);
      BOOST_TEST(!native_weak.expired());
      cancellation.emit(boost::asio::cancellation_type::terminal);
      context.restart();
      context.poll();
      BOOST_CHECK(result.wait_for(std::chrono::seconds{0}) == std::future_status::timeout);
      BOOST_TEST(self->resources.current().system.outbound_streams == 1U);
      barrier->released = true;
      barrier->changed.notify();
      BOOST_REQUIRE(drive_until(context, [&] {
         return result.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
      }));
      BOOST_CHECK_EXCEPTION(result.get(), forge::exceptions::base, [failure](const auto& error) {
         const auto expected = failure == stream_failure::ping ? exceptions::code::protocol_error
             : failure == stream_failure::observer_mismatch ? exceptions::code::peer_verification_failed
             : failure == stream_failure::negotiation ? exceptions::code::unsupported_protocol
             : exceptions::code::backpressure_rejected;
         return exceptions::code_of(error) == expected;
      });
      BOOST_TEST(native_weak.expired());
      BOOST_TEST(self->resources.current().system.outbound_streams == 0U);
      BOOST_TEST(self->resources.current().active_dials == 0U);
      BOOST_TEST(owner.metrics().path_direct_attempts == 0U);
      if (failure == stream_failure::binding) {
         BOOST_TEST(owner.metrics().backpressure_rejections == 1U);
         BOOST_TEST(owner.metrics().protocol_rejections == 1U);
      }
   }

   static boost::asio::awaitable<void> reachability_cleanup_operation(std::shared_ptr<node::impl> self,
       std::shared_ptr<node::impl::session_state> session, stream_failure failure) {
      if (failure == stream_failure::ping) {
         co_await node::impl::ping_reachability_owned(self, session->info.remote_peer, std::make_shared<cancellation_latch>());
      } else if (failure == stream_failure::observer_mismatch) {
         static_cast<void>(co_await node::impl::exchange_reachability_owned(self, session->info.remote_peer,
             parse_endpoint("/ip4/12.0.0.1/tcp/4001"), false,
             {parse_endpoint("/ip4/8.8.8.8/tcp/4001")}, std::make_shared<cancellation_latch>(), session->id));
      } else {
         // Exercise common negotiation/resource binding directly, not through
         // the reachability wrapper that previously attempted a second close.
         static_cast<void>(co_await self->open_session_stream(session, builtins::ping, false));
      }
   }

   static void removed_cached_session_has_one_fresh_dial() {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto server_options = fixture_options("cached-removal-server");
      server_options.limits.resources.system.max_streams = 1;
      auto server = node{runtime, std::move(server_options)};
      auto client = node{runtime, fixture_options("cached-removal-client")};
      auto self = client.impl_;
      auto server_self = server.impl_;

      // Occupy the real P2P stream budget before either session exists. TCP
      // authentication/Yamux upgrade still work; inbound protocol streams reset.
      auto held = server_self->resources.reserve_stream(
          client.local_peer(), resource_manager::session_direction::inbound);
      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) noexcept {
         if (held) {
            held->release();
         }
         client.request_stop();
         server.request_stop();
         for (auto* owner : {&client, &server}) {
            try {
               forge::asio::blocking::run(runtime, owner->async_stop());
            } catch (...) {
               BOOST_ERROR("cached-session regression failed to join node shutdown");
            }
         }
      }};
      BOOST_REQUIRE(held);
      server.register_protocol_handler(builtins::echo,
          [](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
             co_await incoming.stream.async_close();
          });
      forge::asio::blocking::run(runtime, server.async_listen(parse_endpoint("/ip4/127.0.0.1/tcp/0")));
      const auto listening = server.local_endpoint();
      BOOST_REQUIRE(listening);
      const auto remote = server.local_peer();
      const auto connected = forge::asio::blocking::run(
          runtime, client.async_connect(*listening,
              node::connect_options{.expected_peer = remote, .allow_relay = false,
                                    .timeout = std::chrono::seconds{10},
                                    .direct_attempt_timeout = std::chrono::seconds{5},
                                    .max_direct_endpoints = 1, .allow_hole_punch = false}));
      BOOST_CHECK(connected.remote_peer == remote);
      auto cached = self->session_for_path(remote, path::kind::direct);
      auto inbound = server_self->session_for_path(client.local_peer(), path::kind::direct);
      BOOST_REQUIRE(cached);
      BOOST_REQUIRE(inbound);
      BOOST_REQUIRE(cached->connection.valid());
      BOOST_REQUIRE_EQUAL(client.metrics().handshakes_completed, 1U);
      BOOST_REQUIRE_EQUAL(server.metrics().handshakes_completed, 1U);

      // This real wrapper selects synchronously and owns peer/protocol values.
      // Do not run its awaitable until terminal retirement has removed the
      // selected session: no sleeps, registry hooks or copied retry loop.
      auto pending = self->open_protocol_direct_with_context(
          peer_id{remote}, protocol_id{builtins::echo}, std::chrono::seconds{15}, 1,
          std::chrono::seconds{5}, {});
      self->forget_session(cached);
      forge::asio::blocking::run(runtime, self->async_retire_session(cached, true));
      server_self->forget_session(inbound);
      forge::asio::blocking::run(runtime, server_self->async_retire_session(inbound, true));
      BOOST_REQUIRE(!self->session_for_path(remote, path::kind::direct));
      BOOST_REQUIRE(!cached->connection.valid());
      const auto before = client.metrics();
      const auto server_before = server.metrics();
      BOOST_REQUIRE_EQUAL(before.active_sessions, 0U);
      BOOST_REQUIRE_EQUAL(server_before.active_sessions, 0U);

      // A dial/handshake failure or unsupported protocol is not this regression.
      // The replacement must handshake, then fail on a real transport stream.
      BOOST_CHECK_EXCEPTION(
          static_cast<void>(forge::asio::blocking::run(runtime, std::move(pending))),
          forge::exceptions::base,
          [](const auto& error) { return exceptions::code_of(error) == exceptions::code::closed; });
      const auto after = client.metrics();
      const auto server_after = server.metrics();
      const auto fresh_dials = after.sessions_opened - before.sessions_opened;
      BOOST_TEST(fresh_dials >= 1U);
      BOOST_TEST(fresh_dials <= 1U);
      BOOST_TEST(after.handshakes_completed - before.handshakes_completed == 1U);
      BOOST_TEST(server_after.sessions_opened - server_before.sessions_opened == 1U);
      BOOST_TEST(server_after.handshakes_completed - server_before.handshakes_completed == 1U);
      BOOST_TEST(after.handshakes_failed == before.handshakes_failed);
      BOOST_TEST(server_after.handshakes_failed == server_before.handshakes_failed);
      // One fresh dial plus the stale and fresh protocol-open attempts.
      BOOST_TEST(after.path_direct_attempts - before.path_direct_attempts == 3U);
      BOOST_TEST(after.protocol_streams_opened == before.protocol_streams_opened);
      BOOST_TEST(server_after.protocol_streams_accepted == server_before.protocol_streams_accepted);
      BOOST_TEST(server_after.backpressure_rejections > server_before.backpressure_rejections);
      BOOST_TEST(server_self->resources.current().system.inbound_streams == 1U);
   }

   static void blocked_admission(bool timeout) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto owner = node{runtime, fixture_options("admission-owner")};
      auto self = owner.impl_;
      auto context = boost::asio::io_context{};
      auto held = forge::asio::blocking::run(runtime, self->session_admission_gate.acquire());
      auto cancellation = std::make_shared<cancellation_latch>();
      auto barrier = std::make_shared<close_barrier>();

      auto session = self->resources.reserve_session(resource_manager::session_direction::outbound);
      BOOST_REQUIRE(session);
      auto fd = session->reserve_file_descriptors(1);
      BOOST_REQUIRE(fd);
      auto attempt = detail::direct_attempt{};
      attempt.resources = std::make_shared<detail::direct_attempt_resources>();
      attempt.resources->teardown_ticket = self->teardown.track();
      attempt.resources->session = std::move(*session);
      attempt.resources->file_descriptor = std::move(*fd);
      auto resources_weak = std::weak_ptr<detail::direct_attempt_resources>{attempt.resources};
      auto transport = std::make_shared<admission_transport>(barrier, attempt.resources);
      auto transport_weak = std::weak_ptr<admission_transport>{transport};
      attempt.connection.peer = make_peer_id(public_key{public_key::type::ed25519, std::vector<std::uint8_t>(32, 43)});
      attempt.connection.session = forge::net::transport::detail::session_access::make(transport);
      transport.reset();
      attempt.target = parse_endpoint("/ip4/127.0.0.1/tcp/4001/p2p/" + attempt.connection.peer.to_string());
      attempt.started_at = std::chrono::steady_clock::now();
      auto roots = std::vector<forge::multiformats::multiaddr>{attempt.target.to_multiaddr()};
      const auto remote = attempt.connection.peer;
      const auto deadline = std::chrono::steady_clock::now() +
          (timeout ? std::chrono::milliseconds{400} : std::chrono::seconds{5});

      auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{self.get(), [&](void*) {
         barrier->released = true;
         barrier->changed.notify();
         cancellation->request_stop();
         held.release();
         owner.request_stop();
         context.restart();
         context.run_for(std::chrono::seconds{2});
         forge::asio::blocking::run(runtime, owner.async_stop());
      }};
      auto pending = boost::asio::co_spawn(
          context, self->commit_direct_attempt(std::move(attempt), std::move(roots), deadline, cancellation),
          boost::asio::use_future);
      context.poll();
      // Drain all ready handlers, not one post: the only unfinished child work
      // is now the acquisition of the occupied gate.
      BOOST_REQUIRE(pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
      BOOST_REQUIRE(std::chrono::steady_clock::now() < deadline);
      BOOST_REQUIRE(!barrier->entered.load());
      BOOST_REQUIRE(!cancellation->stop_requested());
      if (!timeout) {
         cancellation->request_stop();
      }
      BOOST_REQUIRE(drive_until(context, [&] { return barrier->entered.load(); }));
      BOOST_CHECK(pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
      BOOST_TEST(self->resources.current().system.file_descriptors == 1U);
      BOOST_TEST(self->resources.current().system.outbound_connections == 1U);
      BOOST_TEST(!transport_weak.expired());
      BOOST_TEST(!resources_weak.expired());
      BOOST_TEST(barrier->opens.load() == 0U);
      BOOST_TEST(barrier->accepts.load() == 0U);
      BOOST_TEST(self->sessions.empty());
      BOOST_TEST(self->retiring_sessions.empty());

      barrier->released = true;
      barrier->changed.notify();
      BOOST_REQUIRE(drive_until(context, [&] {
         return pending.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
      }));
      // The gate is deliberately still held: cancellation/timeout, not release,
      // must have completed the operation and its terminal cleanup.
      BOOST_CHECK_EXCEPTION(pending.get(), forge::exceptions::base, [timeout](const auto& error) {
         return exceptions::code_of(error) ==
             (timeout ? exceptions::code::timeout : exceptions::code::canceled);
      });
      BOOST_TEST(transport_weak.expired());
      BOOST_TEST(resources_weak.expired());
      BOOST_TEST(self->resources.current().system.file_descriptors == 0U);
      BOOST_TEST(self->resources.current().system.outbound_connections == 0U);
      BOOST_TEST(!self->store.find(remote).has_value());
      BOOST_TEST(self->sessions.empty());
      BOOST_TEST(self->retiring_sessions.empty());
      BOOST_TEST(barrier->opens.load() == 0U);
      BOOST_TEST(barrier->accepts.load() == 0U);
      held.release();
      auto reacquired = forge::asio::blocking::run(runtime, self->session_admission_gate.acquire());
      reacquired.release();
   }
};

BOOST_AUTO_TEST_CASE(p2p_dial_cancellation_interrupts_occupied_admission_and_awaits_native_close) {
   node_session_fixture::blocked_admission(false);
}

BOOST_AUTO_TEST_CASE(p2p_dial_deadline_interrupts_occupied_admission_and_awaits_native_close) {
   node_session_fixture::blocked_admission(true);
}

BOOST_AUTO_TEST_CASE(p2p_cached_session_removed_before_open_allows_only_one_fresh_handshaken_dial) {
   node_session_fixture::removed_cached_session_has_one_fresh_dial();
}

BOOST_AUTO_TEST_CASE(p2p_background_reachability_reuses_sessions_without_redial_or_path_attempts) {
   node_session_fixture::background_reachability_never_reconnects();
}

BOOST_AUTO_TEST_CASE(p2p_graceful_quic_shutdown_joins_retiring_sessions_before_listener_stop) {
   node_session_fixture::graceful_quic_shutdown_closes_sessions_before_listener(false);
}

BOOST_AUTO_TEST_CASE(p2p_graceful_quic_shutdown_preserves_close_error_after_joined_cleanup) {
   node_session_fixture::graceful_quic_shutdown_closes_sessions_before_listener(true);
}

BOOST_AUTO_TEST_CASE(p2p_reachability_keeps_qualified_control_when_same_peer_dialback_arrives) {
   node_session_fixture::reachability_pins_qualified_session(false);
}

BOOST_AUTO_TEST_CASE(p2p_reachability_retired_session_id_rejects_without_qualified_fallback) {
   node_session_fixture::reachability_pins_qualified_session(true);
}

BOOST_AUTO_TEST_CASE(p2p_ping_primary_error_waits_for_cancellation_resistant_stream_close) {
   node_session_fixture::reachability_error_awaits_stream_cleanup(stream_failure::ping);
}

BOOST_AUTO_TEST_CASE(p2p_autonat_observer_mismatch_waits_for_cancellation_resistant_stream_close) {
   node_session_fixture::reachability_error_awaits_stream_cleanup(stream_failure::observer_mismatch);
}

BOOST_AUTO_TEST_CASE(p2p_negotiation_failure_retains_reservation_until_terminal_stream_close) {
   node_session_fixture::reachability_error_awaits_stream_cleanup(stream_failure::negotiation);
}

BOOST_AUTO_TEST_CASE(p2p_protocol_bind_rejection_retains_reservation_until_terminal_stream_close) {
   node_session_fixture::reachability_error_awaits_stream_cleanup(stream_failure::binding);
}

BOOST_AUTO_TEST_CASE(p2p_advertised_mutations_advance_reachability_generation_before_start) {
   node_session_fixture::advertised_mutations_own_generation_before_start();
}

BOOST_AUTO_TEST_CASE(p2p_prestart_observations_activate_only_until_manager_finishes) {
   node_session_fixture::prestart_observations(false, false);
}

BOOST_AUTO_TEST_CASE(p2p_expired_prestart_observations_never_activate) {
   node_session_fixture::prestart_observations(true, false);
}

BOOST_AUTO_TEST_CASE(p2p_reachability_start_failure_revokes_prestart_observations) {
   node_session_fixture::prestart_observations(false, true);
}

BOOST_AUTO_TEST_CASE(p2p_session_retirement_revokes_observations_before_native_close) {
   node_session_fixture::retirement_revokes_confirmation_before_transport_close();
}

BOOST_AUTO_TEST_CASE(p2p_reachability_refresh_after_result_admission_closes_revokes_confirmations) {
   node_session_fixture::refresh_after_result_admission_closes();
}

BOOST_AUTO_TEST_CASE(p2p_autonat_v1_v2_local_upgraded_gater_refusal_is_not_negative_evidence) {
   node_session_fixture::autonat_local_upgrade_refusal_is_not_negative();
}

BOOST_AUTO_TEST_CASE(p2p_reachability_getter_and_subscription_share_snapshot_across_held_publish_and_ttl) {
   node_session_fixture::getter_and_subscription_share_published_snapshot();
}

} // namespace forge::net::p2p
