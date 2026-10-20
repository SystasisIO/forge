#include <boost/asio/co_spawn.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>
#include "../swarm_storage/support.hpp"

import forge.swarm.runtime.engine;
import forge.api.core.trusted_invocation;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.notification;

namespace runtime_engine_test {
namespace swarm = forge::swarm::runtime;
namespace api = forge::api::core;
namespace core = forge::swarm::core;
using caller = forge::api::auth::authenticated_caller;
using peer = forge::swarm::api::peer;
using namespace std::chrono_literals;

struct upload_probe {
   using call_type = api::bidirectional_stream_call<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message>;
   std::shared_ptr<call_type> call;
   std::atomic_bool unchoked{false};
   std::atomic_bool done{false};
   std::atomic_uint blocks{0};
};

boost::asio::awaitable<void> observe_upload(std::shared_ptr<upload_probe> probe,
   std::shared_ptr<forge::asio::notification> changed) {
   try {
      while (auto message = co_await probe->call->async_read()) {
         if (auto* choke = std::get_if<forge::swarm::protocol::choke>(&message->body)) probe->unchoked = !choke->choked;
         if (std::holds_alternative<forge::swarm::protocol::block>(message->body)) ++probe->blocks;
         changed->notify();
      }
   } catch (...) {}
   probe->done = true; changed->notify();
}

caller principal() {
   return {forge::api::auth::caller_source::p2p_peer, core::sha256::hash(std::string{"runtime-unit-principal"})};
}
caller trusted() {
   auto caller = runtime_engine_test::caller{};
   const auto context = api::trusted_invocation_builder{}.set(principal()).build();
   BOOST_REQUIRE(api::server_supplied<runtime_engine_test::caller>::apply(caller, context));
   return caller;
}

class selected_policy final : public swarm::admission {
 public:
   std::shared_ptr<swarm::quota> grant;
   bool metadata_only = false;
   bool wait_for_cancel = false;
   std::atomic_bool entered{false};
   std::atomic_bool left{false};
   forge::asio::notification changed;
   boost::asio::awaitable<std::shared_ptr<swarm::quota>> async_authorize(
      core::swarm_id, caller, swarm::access_kind kind, std::stop_token stop,
      std::chrono::steady_clock::time_point deadline) override {
      entered = true; changed.notify();
      if (wait_for_cancel) {
         try { co_await changed.async_wait_until(changed.epoch(), deadline, stop); } catch (...) {}
      }
      left = true; changed.notify();
      co_return metadata_only && kind != swarm::access_kind::metadata ? nullptr : grant;
   }
};

class stalled_peer final : public peer {
 public:
   std::atomic_uint entered{0};
   std::atomic_uint active{0};
   forge::asio::notification changed;
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override { co_return; }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      ++entered; ++active; changed.notify();
      try { while (co_await stream.async_read()) {} } catch (...) {}
      --active; changed.notify();
   }
};

class alias_connection final : public swarm::capabilities::connection {
 public:
   explicit alias_connection(std::shared_ptr<stalled_peer> value) : implementation{std::move(value)} {}
   api::handle<peer> remote() const override { return api::handle<peer>{implementation}; }
   caller remote_identity() const override { return principal(); }
   void cancel() noexcept override {}
   boost::asio::awaitable<void> async_close() override { co_return; }
 private:
   std::shared_ptr<stalled_peer> implementation;
};

class aliases final : public swarm::capabilities {
 public:
   std::shared_ptr<stalled_peer> implementation = std::make_shared<stalled_peer>();
   std::atomic_uint connected{0};
   forge::asio::notification changed;
   boost::asio::awaitable<std::vector<std::string>> async_lookup(core::swarm_id, std::string,
      std::size_t, std::stop_token, std::chrono::steady_clock::time_point) override { co_return std::vector<std::string>{"alias-one", "alias-two"}; }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(std::string, std::stop_token stop,
      std::chrono::steady_clock::time_point deadline) override {
      ++connected; changed.notify();
      while (true) {
         const auto epoch = changed.epoch();
         if (connected.load() == 2) break;
         co_await changed.async_wait_until(epoch, deadline, stop);
      }
      co_return std::make_unique<alias_connection>(implementation);
   }
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(core::swarm_id, std::string,
      std::stop_token, std::chrono::steady_clock::time_point) override { co_return nullptr; }
};

BOOST_AUTO_TEST_CASE(local_generated_calls_require_trusted_caller_and_explicit_policy) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(42)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto denied = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), nullptr);
   auto allowed = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>());
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto first = co_await denied->async_share(manifest, store);
      auto second = co_await allowed->async_share(manifest, store);
      const auto id = core::calculate_swarm_id(manifest);
      auto raw = api::handle<peer>{allowed};
      auto missing = co_await raw.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, principal());
      auto rejected = false;
      try { co_await missing.async_finish(); } catch (const api::exceptions::invocation_denied&) { rejected = true; }
      BOOST_CHECK(rejected);
      auto closed = api::handle<peer>{denied};
      auto no_policy = co_await closed.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      rejected = false;
      try { co_await no_policy.async_finish(); } catch (const api::exceptions::invocation_denied&) { rejected = true; }
      BOOST_CHECK(rejected);
      auto permitted = co_await raw.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      auto count = 0U;
      while (auto chunk = co_await permitted.async_read()) count += chunk->data.size();
      co_await permitted.async_finish();
      BOOST_CHECK_GT(count, 0U);
      co_await first.async_close(); co_await second.async_close();
      co_await denied->async_close(); co_await allowed->async_close();
      BOOST_CHECK(!store->snapshot().published);
      co_await store->async_close();
      co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(policy_callback_deadline_is_cancellable_and_joined) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>(); policy->wait_for_cancel = true;
   auto config = swarm::engine::options{}; config.callback_timeout = 25ms;
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy, nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto call = co_await handle.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{core::calculate_swarm_id(manifest), {}}, trusted());
      auto denied = false;
      try { co_await call.async_finish(); } catch (...) { denied = true; }
      BOOST_CHECK(denied);
      co_await share.async_close();
      BOOST_CHECK(policy->entered.load()); BOOST_CHECK(policy->left.load());
      BOOST_CHECK_EQUAL(share.inspect().active_calls, 0U);
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(canceling_pending_download_does_not_wait_for_scheduler_slot) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime, {.max_awaitable_tasks = 1, .max_pending_tasks = 8}};
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>()};
   auto release = forge::asio::notification{};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto blocker = tasks.submit(forge::asio::task::awaitable{
         .name = "unrelated-shared-scheduler-worker", .work = [&](forge::asio::task::context&) -> boost::asio::awaitable<void> {
            co_await release.async_wait(0);
         }});
      auto download = co_await engine.async_download(manifest, store);
      auto wait_done = std::make_shared<std::atomic_bool>(false);
      boost::asio::co_spawn(co_await boost::asio::this_coro::executor, download.async_wait(),
         [wait_done](std::exception_ptr) { *wait_done = true; });
      co_await download.async_close();
      BOOST_CHECK(download.inspect().closed);
      BOOST_CHECK(!blocker.cancel_requested());
      release.notify(); co_await blocker.wait();
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
      BOOST_CHECK(wait_done->load());
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(descriptor_store_mismatch_rejected_before_share_or_download) {
   auto fixture = swarm_test::fixture{};
   const auto actual = swarm_test::describe({swarm_test::data(32, 1)});
   const auto other = swarm_test::describe({swarm_test::data(32, 2)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(actual)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>()};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto failures = 0U;
      try { auto wrong = co_await engine.async_share(other, store); }
      catch (const core::exceptions::invalid_checkpoint&) { ++failures; }
      try { auto wrong = co_await engine.async_download(other, store); }
      catch (const core::exceptions::invalid_checkpoint&) { ++failures; }
      BOOST_CHECK_EQUAL(failures, 2U);
      BOOST_CHECK(!store->snapshot().published);
      auto correct = co_await engine.async_share(actual, store);
      co_await correct.async_close(); co_await engine.async_close();
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(shared_upload_choker_bounds_slots_and_redistributes_after_close) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto config = swarm::engine::options{};
   config.upload.slots = 1; config.upload.optimistic = false; config.upload_round = 5ms;
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), nullptr, config);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto changed = std::make_shared<forge::asio::notification>();
      auto probes = std::vector<std::shared_ptr<upload_probe>>{};
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      const auto id = core::calculate_swarm_id(manifest);
      for (auto i = 0U; i < 3; ++i) {
         auto probe = std::make_shared<upload_probe>();
         auto call = co_await handle.async_open<&peer::exchange>(api::call_options{
            .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536, .deadline = deadline},
            forge::swarm::protocol::exchange_request{id, {}}, trusted());
         probe->call = std::make_shared<upload_probe::call_type>(std::move(call));
         boost::asio::co_spawn(co_await boost::asio::this_coro::executor, observe_upload(probe, changed), [](std::exception_ptr) {});
         probes.push_back(probe);
         co_await probe->call->async_write({forge::swarm::protocol::handshake{1, id, {}, 0}});
         co_await probe->call->async_write({forge::swarm::protocol::interest{true}});
      }
      auto winner = std::size_t{3};
      while (std::chrono::steady_clock::now() < deadline) {
         const auto epoch = changed->epoch();
         BOOST_CHECK_LE(share.inspect().unchoked_peers, 1U);
         if (share.inspect().upload_candidates == 3) {
            for (auto i = std::size_t{}; i < probes.size(); ++i) if (probes[i]->unchoked) winner = i;
            if (winner < probes.size()) break;
         }
         co_await changed->async_wait_until(epoch, deadline);
      }
      BOOST_CHECK_LT(winner, probes.size());
      if (winner < probes.size()) {
         co_await probes[winner]->call->async_write({forge::swarm::protocol::request{1, {0, 0}, 0, 32}});
         while (true) {
            const auto epoch = changed->epoch();
            if (probes[winner]->blocks || std::chrono::steady_clock::now() >= deadline) break;
            co_await changed->async_wait_until(epoch, deadline);
         }
         BOOST_CHECK_EQUAL(probes[winner]->blocks.load(), 1U);
         probes[winner]->call->cancel();
         auto replacement = std::size_t{3};
         while (std::chrono::steady_clock::now() < deadline) {
            const auto epoch = changed->epoch();
            BOOST_CHECK_LE(share.inspect().unchoked_peers, 1U);
            for (auto i = std::size_t{}; i < probes.size(); ++i) if (i != winner && probes[i]->unchoked) replacement = i;
            if (replacement < probes.size()) break;
            co_await changed->async_wait_until(epoch, deadline);
         }
         BOOST_CHECK_LT(replacement, probes.size());
         if (replacement < probes.size()) {
            co_await probes[replacement]->call->async_write({forge::swarm::protocol::request{2, {0, 0}, 0, 32}});
            while (true) {
               const auto epoch = changed->epoch();
               if (probes[replacement]->blocks || std::chrono::steady_clock::now() >= deadline) break;
               co_await changed->async_wait_until(epoch, deadline);
            }
            BOOST_CHECK_EQUAL(probes[replacement]->blocks.load(), 1U);
         }
      }
      for (auto& probe : probes) probe->call->cancel();
      for (auto& probe : probes) { try { co_await probe->call->async_finish(); } catch (...) {} }
      co_await share.async_close();
      for (auto& probe : probes) while (!probe->done) {
         const auto epoch = changed->epoch();
         if (probe->done) break;
         co_await changed->async_wait(epoch);
      }
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(outbound_aliases_share_authenticated_peer_limit_before_exchange) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto network = std::make_shared<aliases>();
   auto config = swarm::engine::options{};
   config.per_peer.calls = 1; config.peers = 2; config.attempts = 1; config.idle_timeout = 200ms;
   auto engine = swarm::engine{tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>(), network, config};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = co_await engine.async_download(manifest, store);
      auto failed = false;
      try { co_await download.async_wait(); } catch (...) { failed = true; }
      BOOST_CHECK(failed);
      BOOST_CHECK_EQUAL(network->connected.load(), 2U);
      BOOST_CHECK_EQUAL(network->implementation->entered.load(), 1U);
      co_await download.async_close();
      while (true) {
         const auto epoch = network->implementation->changed.epoch();
         if (!network->implementation->active) break;
         co_await network->implementation->changed.async_wait(epoch);
      }
      BOOST_CHECK(!store->snapshot().published);
      co_await engine.async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(metadata_only_permission_denies_content_exchange) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>();
   policy->metadata_only = true;
   policy->grant = std::make_shared<swarm::quota>(swarm::quota::options{.content = id, .requester = principal(), .bytes = 65536});
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy);
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto metadata = co_await handle.async_open<&peer::metadata>(forge::swarm::protocol::metadata_request{id, {}}, trusted());
      auto chunks = 0U;
      while (co_await metadata.async_read()) ++chunks;
      co_await metadata.async_finish(); BOOST_CHECK_GT(chunks, 0U);
      auto exchange = co_await handle.async_open<&peer::exchange>(forge::swarm::protocol::exchange_request{id, {}}, trusted());
      auto denied = false;
      try { co_await exchange.async_finish(); } catch (const api::exceptions::invocation_denied&) { denied = true; }
      BOOST_CHECK(denied);
      co_await share.async_close(); co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(expiry_and_revocation_interrupt_accepted_metadata_and_finalize_quota) {
   auto fixture = swarm_test::fixture{};
   // A piece layer larger than one metadata chunk exercises queue backpressure.
   const auto manifest = swarm_test::describe({swarm_test::data(1100U * core::block_size)}, core::block_size);
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto policy = std::make_shared<selected_policy>();
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), policy);
   auto body = [&]() -> boost::asio::awaitable<void> {
      for (const auto revoke : {false, true}) {
         auto share = co_await engine->async_share(manifest, store);
         policy->grant = std::make_shared<swarm::quota>(swarm::quota::options{
            .content = id, .requester = principal(), .bytes = 65536,
            .expires = std::chrono::steady_clock::now() + (revoke ? 3s : 200ms)});
         auto handle = api::handle<peer>{engine};
         auto call = co_await handle.async_open<&peer::metadata>(api::call_options{
            .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536,
            .deadline = std::chrono::steady_clock::now() + 4s},
            forge::swarm::protocol::metadata_request{id, {}}, trusted());
         if (revoke) {
            auto first = co_await call.async_read(); BOOST_CHECK(first.has_value());
            policy->grant->revoke();
         }
         auto failed = false;
         try { co_await call.async_finish(); } catch (...) { failed = true; }
         BOOST_CHECK(failed);
         co_await share.async_close();
         const auto status = policy->grant->inspect();
         BOOST_CHECK_GT(status.traffic.queued, 0U);
         BOOST_CHECK_EQUAL(status.traffic.sent, 0U);
         BOOST_CHECK_EQUAL(status.held, 0U);
         BOOST_CHECK_GT(status.finalized, 0U);
      }
      co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(lazy_awaitables_capture_state_before_facade_move_or_destruction) {
   auto fixture = swarm_test::fixture{};
   const auto manifest = swarm_test::describe({swarm_test::data(32)});
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto engine = std::make_unique<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>());
      auto share = co_await engine->async_share(manifest, store);
      auto pending_close = share.async_close();
      share = swarm::sharing{};
      co_await std::move(pending_close);
      auto download = co_await engine->async_download(manifest, store);
      auto pending_wait = download.async_wait();
      auto pending_download_close = download.async_close();
      download = swarm::download{};
      try { co_await std::move(pending_wait); } catch (...) {}
      co_await std::move(pending_download_close);
      auto pending_share = engine->async_share(manifest, store);
      auto pending_download = engine->async_download(manifest, store);
      auto pending_engine_close = engine->async_close();
      engine.reset();
      auto rejected = 0U;
      try { auto unused = co_await std::move(pending_share); } catch (...) { ++rejected; }
      try { auto unused = co_await std::move(pending_download); } catch (...) { ++rejected; }
      BOOST_CHECK_EQUAL(rejected, 2U);
      co_await std::move(pending_engine_close);
      co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

BOOST_AUTO_TEST_CASE(connected_peer_sees_only_verified_store_changes_without_reconnect) {
   auto fixture = swarm_test::fixture{};
   const auto bytes = swarm_test::data(32);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(fixture.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto tasks = forge::asio::task::scheduler{runtime};
   auto engine = std::make_shared<swarm::engine>(tasks, fixture.cpu.get_executor(), std::make_shared<swarm::free_admission>());
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = co_await engine->async_share(manifest, store);
      auto handle = api::handle<peer>{engine};
      auto call = co_await handle.async_open<&peer::exchange>(api::call_options{
         .max_item_bytes = 65536, .max_buffered_items = 1, .max_buffered_bytes = 65536,
         .deadline = std::chrono::steady_clock::now() + 3s},
         forge::swarm::protocol::exchange_request{id, {}}, trusted());
      co_await call.async_write({forge::swarm::protocol::handshake{1, id, {}, 0}});
      co_await call.async_write({forge::swarm::protocol::interest{true}});
      while (auto message = co_await call.async_read()) {
         if (auto* inventory = std::get_if<forge::swarm::protocol::inventory>(&message->body)) {
            BOOST_CHECK(!core::bitmap_test(inventory->pieces, 0)); break;
         }
      }
      auto writer = store->begin({0, 0});
      co_await writer->async_write(0, bytes);
      BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
      co_await writer->async_commit(); writer.reset();
      auto announced = false;
      while (auto message = co_await call.async_read()) {
         if (auto* have = std::get_if<forge::swarm::protocol::have>(&message->body)) {
            BOOST_CHECK(have->piece == core::piece_id(0, 0)); announced = true; break;
         }
      }
      BOOST_CHECK(announced);
      co_await call.async_write({forge::swarm::protocol::request{1, {0, 0}, 0, 32}});
      auto received = false;
      while (auto message = co_await call.async_read()) {
         if (auto* block = std::get_if<forge::swarm::protocol::block>(&message->body)) {
            BOOST_CHECK(block->data == bytes); received = true; break;
         }
      }
      BOOST_CHECK(received);
      call.cancel(); try { co_await call.async_finish(); } catch (...) {}
      co_await share.async_close(); co_await engine->async_close(); co_await store->async_close(); co_await tasks.shutdown();
   };
   forge::asio::blocking::run(runtime, body());
}

} // namespace runtime_engine_test
