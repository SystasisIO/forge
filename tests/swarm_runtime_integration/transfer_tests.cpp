#include <boost/asio/this_coro.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include "../swarm_storage/support.hpp"

import forge.swarm.runtime.engine;
import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.core.trusted_invocation;
import forge.api.transport.connection;
import forge.api.stream.session;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.notification;
import forge.net.tcp.listener;

namespace runtime_integration {
namespace swarm = forge::swarm::runtime;
namespace api = forge::api::core;
using peer = forge::swarm::api::peer;
using caller = forge::api::auth::authenticated_caller;
using namespace std::chrono_literals;

caller principal() {
   return {forge::api::auth::caller_source::p2p_peer,
           forge::swarm::core::sha256::hash(std::string{"runtime-tcp-authenticated-peer"})};
}

boost::asio::awaitable<void> drain(boost::asio::awaitable<void> work, std::exception_ptr& failure) {
   try { co_await std::move(work); }
   catch (...) { if (!failure) failure = std::current_exception(); }
}

// The fixture supplies the trusted binding context. It does not simulate TLS
// authentication or claim to qualify the P2P adapter delivered separately.
class tcp_link final : public swarm::capabilities::connection {
 public:
   tcp_link(boost::asio::any_io_executor executor, forge::asio::task::scheduler& scheduler,
            const std::shared_ptr<peer>& implementation)
       : listener{executor, {.protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                             .host = "127.0.0.1", .port = 0}}, connector{executor}, scheduler{scheduler} {
      registry.install<peer>(implementation);
   }

   boost::asio::awaitable<void> open() {
      auto plan = api::binding().serve(registry).build();
      auto options = forge::api::stream::options{};
      options.max_frame_size = 131072;
      options.max_item_size = 65536;
      options.initial_window_items = 2;
      options.initial_window_bytes = 131072;
      options.max_buffered_bytes = 262144;
      options.control_timeout = 2s;
      options.idle_timeout = 4s;
      options.disconnect_grace = 200ms;
      server = scheduler.submit(forge::asio::task::awaitable{
         .name = "swarm-runtime-tcp-test",
         .work = [this, plan = std::move(plan), options](forge::asio::task::context&) mutable
            -> boost::asio::awaitable<void> {
            auto accepted = co_await listener.async_accept();
            session = std::make_shared<forge::api::stream::session>(std::move(accepted.stream), plan, options,
               api::metadata{}, api::trusted_invocation_builder{}.set(principal()).build());
            co_await session->async_serve();
         }});
      BOOST_REQUIRE(server.accepted());
      auto connected = co_await connector.async_connect(listener.local_endpoint());
      client = forge::api::transport::connection{std::move(connected.stream), options};
      handle = co_await client.get_remote_api<peer>();
   }

   api::handle<peer> remote() const override { return handle; }
   caller remote_identity() const override {
      return {forge::api::auth::caller_source::p2p_peer,
              forge::swarm::core::sha256::hash(std::string{"runtime-fixture-seed-identity"})};
   }
   void cancel() noexcept override { client.cancel(); }
   boost::asio::awaitable<void> async_close() override {
      auto failure = std::exception_ptr{};
      co_await drain(listener.async_close(), failure);
      co_await drain(client.async_close(), failure);
      if (session)
         co_await drain(session->async_close(), failure);
      co_await drain(connector.async_stop(), failure);
      if (server.valid())
         co_await drain(server.wait(), failure);
      if (failure)
         std::rethrow_exception(failure);
   }

 private:
   api::registry registry;
   forge::net::tcp::listener listener;
   forge::net::tcp::connector connector;
   forge::asio::task::scheduler& scheduler;
   forge::asio::task::handle server;
   std::shared_ptr<forge::api::stream::session> session;
   forge::api::transport::connection client;
   api::handle<peer> handle;
};

class network final : public swarm::capabilities {
 public:
   network(forge::asio::task::scheduler& scheduler, std::shared_ptr<peer> seed)
       : scheduler{scheduler}, seed{std::move(seed)} {}
   boost::asio::awaitable<std::vector<std::string>> async_lookup(
      forge::swarm::core::swarm_id, std::string, std::size_t limit, std::stop_token,
      std::chrono::steady_clock::time_point) override {
      ++lookups;
      co_return limit ? std::vector<std::string>{"fixture-seed"} : std::vector<std::string>{};
   }
   boost::asio::awaitable<std::unique_ptr<connection>> async_connect(
      std::string route, std::stop_token, std::chrono::steady_clock::time_point) override {
      BOOST_CHECK_EQUAL(route, "fixture-seed");
      ++connections;
      auto value = std::make_unique<tcp_link>(co_await boost::asio::this_coro::executor, scheduler, seed);
      auto failure = std::exception_ptr{};
      try { co_await value->open(); }
      catch (...) { failure = std::current_exception(); }
      if (failure) {
         value->cancel();
         co_await drain(value->async_close(), failure);
         std::rethrow_exception(failure);
      }
      co_return std::move(value);
   }
   boost::asio::awaitable<std::unique_ptr<registration>> async_register(
      forge::swarm::core::swarm_id, std::string, std::stop_token,
      std::chrono::steady_clock::time_point) override {
      co_return nullptr;
   }
   std::atomic_uint lookups{0};
   std::atomic_uint connections{0};
 private:
   forge::asio::task::scheduler& scheduler;
   std::shared_ptr<peer> seed;
};

class funded_policy final : public swarm::admission {
 public:
   explicit funded_policy(std::shared_ptr<swarm::quota> grant) : grant{std::move(grant)} {}
   boost::asio::awaitable<std::shared_ptr<swarm::quota>> async_authorize(
      forge::swarm::core::swarm_id id, caller requester, swarm::access_kind, std::stop_token,
      std::chrono::steady_clock::time_point) override {
      ++calls;
      BOOST_CHECK(requester.transport_authenticated());
      BOOST_CHECK(requester.fingerprint == principal().fingerprint);
      co_return grant->permits(id, requester) ? grant : nullptr;
   }
   std::atomic_uint calls{0};
   std::shared_ptr<swarm::quota> grant;
};

// Models an already-entered, non-interruptible store operation. The real file
// read still runs after release; cancellation must not destroy its owner early.
class late_read_store final : public forge::swarm::core::store {
 public:
   explicit late_read_store(std::shared_ptr<store> backing, std::chrono::milliseconds delay = {})
       : backing{std::move(backing)}, delay{delay} {}
   forge::swarm::core::availability snapshot() const override { return backing->snapshot(); }
   std::unique_ptr<writer> begin(forge::swarm::core::piece_id piece) override { return backing->begin(piece); }
   void discard(forge::swarm::core::piece_id piece) override { backing->discard(piece); }
   lease acquire_lease() override { return backing->acquire_lease(); }
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read(
      forge::swarm::core::piece_id piece, std::uint32_t offset, std::uint32_t size,
      std::stop_token) override {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation());
      ++active;
      entered.store(true);
      changed.notify();
      auto result = std::vector<std::uint8_t>{};
      auto failure = std::exception_ptr{};
      try {
         if (delay.count() > 0) {
            auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
            timer.expires_after(delay);
            co_await timer.async_wait(boost::asio::use_awaitable);
         } else {
            while (true) {
               const auto epoch = changed.epoch();
               if (released.load()) break;
               co_await changed.async_wait(epoch);
            }
         }
         result = co_await backing->async_read(piece, offset, size);
      } catch (...) { failure = std::current_exception(); }
      --active;
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_terminal_cancellation());
      if (failure)
         std::rethrow_exception(failure);
      co_return result;
   }
   boost::asio::awaitable<forge::swarm::core::availability> async_wait_change(
      std::uint64_t generation, std::stop_token stop) override {
      return backing->async_wait_change(generation, stop);
   }
   boost::asio::awaitable<forge::swarm::core::resume_state> async_checkpoint(std::stop_token stop) override {
      return backing->async_checkpoint(stop);
   }
   boost::asio::awaitable<void> async_publish(std::stop_token stop) override { return backing->async_publish(stop); }
   void request_stop() noexcept override { ++stop_calls; }
   boost::asio::awaitable<void> async_close() override { return backing->async_close(); }
   void release() { released.store(true); changed.notify(); }
   std::atomic_bool entered{false};
   std::atomic_bool released{false};
   std::atomic_uint active{0};
   std::atomic_uint stop_calls{0};
   forge::asio::notification changed;
 private:
   std::shared_ptr<store> backing;
   std::chrono::milliseconds delay;
};

class adversarial_peer final : public peer {
 public:
   enum class behavior { wrong_offset, corrupt, late_corrupt_duplicate };
   adversarial_peer(forge::swarm::core::descriptor manifest, std::vector<std::uint8_t> bytes, behavior mode)
       : manifest{std::move(manifest)}, bytes{std::move(bytes)}, mode{mode} {}
   boost::asio::awaitable<void> metadata(forge::swarm::protocol::metadata_request, caller,
      api::stream_writer<forge::swarm::protocol::metadata_chunk>) override {
      BOOST_FAIL("known-descriptor download unexpectedly used a metadata fallback");
      co_return;
   }
   boost::asio::awaitable<void> exchange(forge::swarm::protocol::exchange_request request, caller,
      api::duplex_stream<forge::swarm::protocol::peer_message, forge::swarm::protocol::peer_message> stream) override {
      namespace core = forge::swarm::core;
      namespace wire = forge::swarm::protocol;
      auto first = co_await stream.async_read();
      BOOST_REQUIRE(first && std::holds_alternative<wire::handshake>(first->body));
      const auto pieces = core::validate_descriptor(manifest).total_pieces;
      auto available = std::vector<std::uint8_t>((pieces + 7U) / 8U);
      for (std::uint32_t i = 0; i < pieces; ++i)
         available[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
      co_await stream.async_write({wire::handshake{1U, request.swarm, request.realm, 0U}});
      co_await stream.async_write({wire::inventory{std::move(available)}});
      co_await stream.async_write({wire::choke{false}});
      while (auto message = co_await stream.async_read()) {
         const auto* block_request = std::get_if<wire::request>(&message->body);
         if (!block_request) continue;
         ++requests;
         const auto start = std::size_t{block_request->piece.index} * manifest.piece_size + block_request->offset;
         auto data = std::vector<std::uint8_t>{bytes.begin() + start, bytes.begin() + start + block_request->length};
         auto offset = block_request->offset;
         if (mode == behavior::wrong_offset)
            offset = offset == 0U ? core::block_size : 0U;
         if (mode == behavior::corrupt)
            data.front() ^= 1U;
         auto response = wire::block{block_request->id, block_request->piece, offset, data};
         // The first mode remains valid at the wire/descriptor geometry layer;
         // only correlation with the actual outstanding request can reject it.
         wire::validate(wire::peer_message{response}, manifest);
         co_await stream.async_write({std::move(response)});
         if (mode == behavior::late_corrupt_duplicate) {
            data.front() ^= 1U;
            co_await stream.async_write({wire::block{
               block_request->id, block_request->piece, block_request->offset, std::move(data)}});
         }
      }
      co_await stream.async_close();
   }
   std::atomic_uint requests{0};
 private:
   forge::swarm::core::descriptor manifest;
   std::vector<std::uint8_t> bytes;
   behavior mode;
};

void transfer(bool funded, bool insufficient = false, std::chrono::milliseconds local_delay = {}) {
   namespace core = forge::swarm::core;
   auto source = swarm_test::fixture{};
   auto destination = swarm_test::fixture{};
   const auto slow = local_delay.count() > 0;
   auto bytes = swarm_test::data(slow ? 16U * core::block_size : 8U * core::block_size + 31U);
   const auto manifest = swarm_test::describe({bytes}, (slow ? 8U : 2U) * core::block_size);
   const auto id = core::calculate_swarm_id(manifest);
   auto source_options = source.options(manifest);
   auto seed = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(std::move(source_options)))};
   swarm_test::fill(*seed, manifest, {bytes});
   swarm_test::run(seed->async_publish());
   if (slow)
      seed = std::make_shared<late_read_store>(std::move(seed), local_delay);
   auto target = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(destination.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto grant = std::make_shared<swarm::quota>(swarm::quota::options{
      .content = id, .requester = principal(),
      .bytes = insufficient ? 2U * core::block_size : 1024U * 1024U});
   auto funded_access = std::make_shared<funded_policy>(grant);
   auto access = funded ? std::shared_ptr<swarm::admission>{funded_access}
                        : std::shared_ptr<swarm::admission>{std::make_shared<swarm::free_admission>()};
   auto limits = swarm::engine::options{};
   if (slow)
      limits.idle_timeout = 400ms;
   auto serving = std::make_shared<swarm::engine>(scheduler, source.cpu.get_executor(), access, nullptr, limits);
   auto routes = std::make_shared<network>(scheduler, serving);
   auto receiving = swarm::engine{scheduler, destination.cpu.get_executor(),
                                 std::make_shared<swarm::free_admission>(), routes, limits};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = swarm::sharing{};
      auto download = swarm::download{};
      auto failure = std::exception_ptr{};
      try {
         share = co_await serving->async_share(manifest, seed, "integration");
         download = co_await receiving.async_download(manifest, target, "integration");
         auto denied = false;
         try { co_await download.async_wait(); }
         catch (const api::exceptions::resource_exhausted&) {
            if (!insufficient) throw;
            denied = true;
         }
         catch (const api::exceptions::invocation_denied&) {
            if (!insufficient) throw;
            denied = true;
         }
         if (insufficient) {
            BOOST_CHECK(denied);
            BOOST_CHECK(!download.inspect().complete);
            BOOST_CHECK(!target->snapshot().published);
            BOOST_CHECK_LT(download.inspect().traffic.received, bytes.size());
            BOOST_CHECK_LT(grant->inspect().available, core::block_size);
            BOOST_CHECK_GT(grant->inspect().traffic.requested, grant->inspect().traffic.queued);
         } else {
            BOOST_CHECK(download.inspect().complete);
            BOOST_CHECK_EQUAL(download.inspect().traffic.received, bytes.size());
            BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
            BOOST_CHECK(swarm_test::read_file(destination.root / "result") == bytes);
            BOOST_CHECK(target->snapshot().published);
         }
         if (funded) {
            BOOST_CHECK_GT(funded_access->calls.load(), 0U);
            BOOST_CHECK_GT(grant->inspect().traffic.queued, 0U);
            BOOST_CHECK_EQUAL(grant->inspect().traffic.sent, 0U);
            if (!insufficient)
               BOOST_CHECK_GE(grant->inspect().traffic.queued, bytes.size());
         }
      } catch (...) { failure = std::current_exception(); }
      auto close_failure = std::exception_ptr{};
      co_await drain(download.async_close(), close_failure);
      if (close_failure && !failure) {
         try { std::rethrow_exception(close_failure); }
         catch (const api::exceptions::resource_exhausted&) { if (!insufficient) failure = close_failure; }
         catch (const api::exceptions::invocation_denied&) { if (!insufficient) failure = close_failure; }
         catch (...) { failure = close_failure; }
      }
      BOOST_CHECK(download.inspect().closed);
      BOOST_CHECK_EQUAL(download.inspect().active_calls, 0U);
      co_await drain(receiving.async_close(), failure);
      co_await drain(share.async_close(), failure);
      co_await drain(serving->async_close(), failure);
      try {
         const auto retained = co_await seed->async_read({0, 0}, 0, 31U);
         BOOST_CHECK(retained == std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + 31U));
         auto ran = false;
         auto probe = scheduler.submit(forge::asio::task::awaitable{
            .name = "post-swarm-owner-close", .work = [&ran](forge::asio::task::context&)
               -> boost::asio::awaitable<void> { ran = true; co_return; }});
         BOOST_CHECK(probe.accepted());
         co_await probe.wait();
         BOOST_CHECK(ran);
      } catch (...) { if (!failure) failure = std::current_exception(); }
      co_await drain(target->async_close(), failure);
      co_await drain(seed->async_close(), failure);
      co_await drain(scheduler.shutdown(), failure);
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
   BOOST_CHECK_GT(routes->lookups.load(), 0U);
   BOOST_CHECK_GT(routes->connections.load(), 0U);
   BOOST_CHECK_EQUAL(grant->inspect().held, 0U);
   BOOST_CHECK(swarm_test::read_file(source.root / "result") == bytes);
}

void denied_metadata() {
   namespace core = forge::swarm::core;
   namespace wire = forge::swarm::protocol;
   auto files = swarm_test::fixture{};
   const auto bytes = swarm_test::data(31U);
   const auto manifest = swarm_test::describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto store = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(files.options(manifest)))};
   swarm_test::fill(*store, manifest, {bytes});
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto grant = std::make_shared<swarm::quota>(swarm::quota::options{
      .content = id, .requester = principal(), .bytes = 0U});
   auto policy = std::make_shared<funded_policy>(grant);
   auto serving = std::make_shared<swarm::engine>(scheduler, files.cpu.get_executor(), policy);
   auto routes = network{scheduler, serving};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = swarm::sharing{};
      auto connection = std::unique_ptr<swarm::capabilities::connection>{};
      auto failure = std::exception_ptr{};
      try {
         share = co_await serving->async_share(manifest, store, "protected");
         connection = co_await routes.async_connect("fixture-seed", {},
            std::chrono::steady_clock::now() + 5s);
         auto forged = caller{forge::api::auth::caller_source::tls_certificate,
                              core::sha256::hash(std::string{"spoofed-client"})};
         auto remote = connection->remote();
         auto call = co_await remote.async_open<&peer::metadata>(
            api::call_options{.deadline = std::chrono::steady_clock::now() + 5s},
            wire::metadata_request{id, "protected"}, forged);
         auto leaked = false;
         try { leaked = (co_await call.async_read()).has_value(); }
         catch (const api::exceptions::cancelled&) {}
         catch (const api::exceptions::invocation_denied&) {}
         BOOST_CHECK(!leaked);
         auto denied = false;
         try { co_await call.async_finish(); }
         catch (const api::exceptions::invocation_denied&) { denied = true; }
         BOOST_CHECK(denied);
         BOOST_CHECK_EQUAL(policy->calls.load(), 1U);
         BOOST_CHECK_EQUAL(grant->inspect().traffic.queued, 0U);
         BOOST_CHECK(!forged.transport_authenticated());
      } catch (...) { failure = std::current_exception(); }
      if (connection)
         co_await drain(connection->async_close(), failure);
      co_await drain(share.async_close(), failure);
      co_await drain(serving->async_close(), failure);
      co_await drain(store->async_close(), failure);
      co_await drain(scheduler.shutdown(), failure);
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
   BOOST_CHECK_EQUAL(grant->inspect().held, 0U);
}

void late_store_completion() {
   namespace core = forge::swarm::core;
   auto source = swarm_test::fixture{};
   auto destination = swarm_test::fixture{};
   const auto bytes = swarm_test::data(core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   auto backing = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   swarm_test::fill(*backing, manifest, {bytes});
   auto seed = std::make_shared<late_read_store>(backing);
   auto target = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(destination.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto access = std::make_shared<swarm::free_admission>();
   auto serving = std::make_shared<swarm::engine>(scheduler, source.cpu.get_executor(), access);
   auto routes = std::make_shared<network>(scheduler, serving);
   auto receiving = swarm::engine{scheduler, destination.cpu.get_executor(), access, routes};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = swarm::sharing{};
      auto download = swarm::download{};
      auto closer = forge::asio::task::handle{};
      auto closed = std::atomic_bool{false};
      auto close_started = std::atomic_bool{false};
      auto close_progress = forge::asio::notification{};
      auto failure = std::exception_ptr{};
      try {
         share = co_await serving->async_share(manifest, seed, "late-read");
         download = co_await receiving.async_download(manifest, target, "late-read");
         while (true) {
            const auto epoch = seed->changed.epoch();
            if (seed->entered.load()) break;
            co_await seed->changed.async_wait_until(epoch, std::chrono::steady_clock::now() + 5s);
            if (!seed->entered.load()) {
               BOOST_FAIL("store read did not start before deadline");
            }
         }
         share.cancel();
         download.cancel();
         closer = scheduler.submit(forge::asio::task::awaitable{
            .name = "close-while-store-active",
            .work = [&share, &closed, &close_started, &close_progress](forge::asio::task::context&)
               -> boost::asio::awaitable<void> {
               close_started.store(true);
               close_progress.notify();
               co_await share.async_close();
               closed.store(true);
            }});
         BOOST_REQUIRE(closer.accepted());
         while (true) {
            const auto epoch = close_progress.epoch();
            if (close_started.load()) break;
            co_await close_progress.async_wait_until(epoch, std::chrono::steady_clock::now() + 5s);
            if (!close_started.load())
               BOOST_FAIL("sharing close did not start before deadline");
         }
         auto pause = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         pause.expires_after(25ms);
         co_await pause.async_wait(boost::asio::use_awaitable);
         BOOST_CHECK(!closed.load());
         BOOST_CHECK_EQUAL(seed->active.load(), 1U);
      } catch (...) { failure = std::current_exception(); }
      seed->release();
      if (closer.valid())
         co_await drain(closer.wait(), failure);
      BOOST_CHECK(closed.load());
      BOOST_CHECK_EQUAL(seed->active.load(), 0U);
      co_await drain(download.async_close(), failure);
      co_await drain(receiving.async_close(), failure);
      co_await drain(share.async_close(), failure);
      co_await drain(serving->async_close(), failure);
      BOOST_CHECK_EQUAL(seed->stop_calls.load(), 0U);
      BOOST_CHECK_EQUAL(share.inspect().active_calls, 0U);
      try {
         const auto retained = co_await backing->async_read({0, 0}, 0, core::block_size);
         BOOST_CHECK(retained == bytes);
      } catch (...) { if (!failure) failure = std::current_exception(); }
      co_await drain(target->async_close(), failure);
      co_await drain(backing->async_close(), failure);
      co_await drain(scheduler.shutdown(), failure);
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
}

void hostile_download(adversarial_peer::behavior mode) {
   namespace core = forge::swarm::core;
   auto files = swarm_test::fixture{};
   const auto bytes = swarm_test::data(4U * core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   auto target = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(files.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto hostile = std::make_shared<adversarial_peer>(manifest, bytes, mode);
   auto routes = std::make_shared<network>(scheduler, hostile);
   auto receiving = swarm::engine{scheduler, files.cpu.get_executor(),
      std::make_shared<swarm::free_admission>(), routes};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto download = swarm::download{};
      auto failure = std::exception_ptr{};
      try {
         download = co_await receiving.async_download(manifest, target, "hostile");
         auto rejected = false;
         try { co_await download.async_wait(); }
         catch (const api::exceptions::resource_exhausted&) { rejected = true; }
         catch (const forge::swarm::protocol::exceptions::invalid_message&) { rejected = true; }
         catch (const core::exceptions::integrity_failure&) { rejected = true; }
         if (mode == adversarial_peer::behavior::late_corrupt_duplicate) {
            BOOST_CHECK(!rejected);
            BOOST_CHECK(download.inspect().complete);
            BOOST_CHECK_EQUAL(download.inspect().traffic.verified, bytes.size());
            BOOST_CHECK_GT(download.inspect().traffic.received, bytes.size());
            BOOST_CHECK(swarm_test::read_file(files.root / "result") == bytes);
         } else {
            BOOST_CHECK(rejected);
            BOOST_CHECK(!download.inspect().complete);
            BOOST_CHECK(!target->snapshot().published);
            BOOST_CHECK_EQUAL(download.inspect().traffic.verified, 0U);
            if (mode == adversarial_peer::behavior::wrong_offset)
               BOOST_CHECK(target->snapshot().unfinished.empty());
         }
      } catch (...) { failure = std::current_exception(); }
      co_await drain(download.async_close(), failure);
      co_await drain(receiving.async_close(), failure);
      co_await drain(target->async_close(), failure);
      co_await drain(scheduler.shutdown(), failure);
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
   BOOST_CHECK_GT(hostile->requests.load(), 0U);
}

void overlapping_downloads_share_one_allowance() {
   namespace core = forge::swarm::core;
   auto source = swarm_test::fixture{};
   auto destination_a = swarm_test::fixture{};
   auto destination_b = swarm_test::fixture{};
   const auto bytes = swarm_test::data(2U * core::block_size);
   const auto manifest = swarm_test::describe({bytes});
   auto backing = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(source.options(manifest)))};
   swarm_test::fill(*backing, manifest, {bytes});
   swarm_test::run(backing->async_publish());
   auto seed = std::make_shared<late_read_store>(backing);
   auto target_a = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(destination_a.options(manifest)))};
   auto target_b = std::shared_ptr<core::store>{swarm_test::run(
      forge::swarm::storage::file_store::async_open(destination_b.options(manifest)))};
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto limits = swarm::engine::options{};
   limits.per_peer.calls = 2U;
   limits.per_peer.items = 32U;
   auto grant = std::make_shared<swarm::quota>(swarm::quota::options{
      .content = core::calculate_swarm_id(manifest), .requester = principal(), .bytes = 3U * core::block_size});
   auto serving = std::make_shared<swarm::engine>(scheduler, source.cpu.get_executor(),
      std::make_shared<funded_policy>(grant), nullptr, limits);
   auto routes = std::make_shared<network>(scheduler, serving);
   auto receiving = swarm::engine{scheduler, destination_a.cpu.get_executor(),
      std::make_shared<swarm::free_admission>(), routes, limits};
   auto body = [&]() -> boost::asio::awaitable<void> {
      auto share = swarm::sharing{};
      auto a = swarm::download{};
      auto b = swarm::download{};
      auto failure = std::exception_ptr{};
      try {
         share = co_await serving->async_share(manifest, seed, "shared-allowance");
         a = co_await receiving.async_download(manifest, target_a, "shared-allowance");
         b = co_await receiving.async_download(manifest, target_b, "shared-allowance");
         const auto deadline = std::chrono::steady_clock::now() + 5s;
         while (true) {
            const auto epoch = seed->changed.epoch();
            if (seed->active.load() == 2U) break;
            co_await seed->changed.async_wait_until(epoch, deadline);
            if (std::chrono::steady_clock::now() >= deadline && seed->active.load() != 2U)
               BOOST_FAIL("two runtime sessions did not overlap at the store boundary");
         }
         BOOST_CHECK_EQUAL(grant->inspect().held, 2U * core::block_size);
         BOOST_CHECK_EQUAL(grant->inspect().available, core::block_size);
         seed->release();
         auto finish = [](swarm::download& operation) -> boost::asio::awaitable<bool> {
            try { co_await operation.async_wait(); }
            catch (const api::exceptions::resource_exhausted&) { co_return false; }
            catch (const api::exceptions::invocation_denied&) { co_return false; }
            co_return operation.inspect().complete;
         };
         const auto completed_a = co_await finish(a);
         const auto completed_b = co_await finish(b);
         BOOST_CHECK_EQUAL(unsigned(completed_a) + unsigned(completed_b), 1U);
         BOOST_CHECK_EQUAL(unsigned(target_a->snapshot().published) + unsigned(target_b->snapshot().published), 1U);
         if (completed_a)
            BOOST_CHECK(swarm_test::read_file(destination_a.root / "result") == bytes);
         if (completed_b)
            BOOST_CHECK(swarm_test::read_file(destination_b.root / "result") == bytes);
         BOOST_CHECK_EQUAL(grant->inspect().traffic.queued, 3U * core::block_size);
         BOOST_CHECK_EQUAL(grant->inspect().available, 0U);
      } catch (...) { failure = std::current_exception(); }
      seed->release();
      co_await drain(a.async_close(), failure);
      co_await drain(b.async_close(), failure);
      co_await drain(receiving.async_close(), failure);
      co_await drain(share.async_close(), failure);
      co_await drain(serving->async_close(), failure);
      BOOST_CHECK_EQUAL(grant->inspect().held, 0U);
      BOOST_CHECK_EQUAL(seed->active.load(), 0U);
      BOOST_CHECK_EQUAL(seed->stop_calls.load(), 0U);
      co_await drain(target_a->async_close(), failure);
      co_await drain(target_b->async_close(), failure);
      co_await drain(backing->async_close(), failure);
      co_await drain(scheduler.shutdown(), failure);
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
}
} // namespace runtime_integration

BOOST_AUTO_TEST_SUITE(swarm_runtime_tcp)
BOOST_AUTO_TEST_CASE(explicit_free_policy_downloads_a_real_file_over_tcp) {
   runtime_integration::transfer(false);
}
BOOST_AUTO_TEST_CASE(shared_funded_policy_downloads_without_claiming_remote_receipt) {
   runtime_integration::transfer(true);
}
BOOST_AUTO_TEST_CASE(exhausted_quota_discloses_no_metadata_and_ignores_spoofed_caller) {
   runtime_integration::denied_metadata();
}
BOOST_AUTO_TEST_CASE(quota_exhaustion_stops_payload_without_free_retry_or_origin_fallback) {
   runtime_integration::transfer(true, true);
}
BOOST_AUTO_TEST_CASE(sharing_close_joins_late_store_work_without_stopping_the_store) {
   runtime_integration::late_store_completion();
}
BOOST_AUTO_TEST_CASE(valid_wire_block_with_wrong_owned_offset_is_rejected_before_storage) {
   runtime_integration::hostile_download(runtime_integration::adversarial_peer::behavior::wrong_offset);
}
BOOST_AUTO_TEST_CASE(corrupt_peer_never_publishes_unverified_content) {
   runtime_integration::hostile_download(runtime_integration::adversarial_peer::behavior::corrupt);
}
BOOST_AUTO_TEST_CASE(late_corrupt_duplicate_does_not_overwrite_the_accepted_block) {
   runtime_integration::hostile_download(runtime_integration::adversarial_peer::behavior::late_corrupt_duplicate);
}
BOOST_AUTO_TEST_CASE(healthy_free_transfer_exceeds_three_idle_intervals_with_slow_local_storage) {
   runtime_integration::transfer(false, false, std::chrono::milliseconds{100});
}
BOOST_AUTO_TEST_CASE(healthy_funded_transfer_exceeds_three_idle_intervals_without_resetting_the_grant) {
   runtime_integration::transfer(true, false, std::chrono::milliseconds{100});
}
BOOST_AUTO_TEST_CASE(two_live_downloads_reserve_one_shared_budget_before_store_work) {
   runtime_integration::overlapping_downloads_share_one_allowance();
}
BOOST_AUTO_TEST_SUITE_END()
