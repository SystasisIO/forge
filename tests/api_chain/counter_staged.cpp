module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <coroutine>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

module test.api_chain.counter_client;

import forge.api.chain.outbox;
import forge.asio.affine;
import forge.chain.api.exceptions;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.mdbx.driver;
import forge.db.object.registration;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;
import package.chain_api_component.test_support;

namespace demo {
namespace {

boost::asio::awaitable<void> next_poll() {
   using namespace std::chrono_literals;
   boost::asio::steady_timer tick{co_await boost::asio::this_coro::executor, 10ms};
   co_await tick.async_wait(boost::asio::use_awaitable);
}

boost::asio::awaitable<void> test_first_send(
    const std::filesystem::path& path, forge::chain::protocol::chain_id chain,
    const forge::chain::protocol::packed_transaction& packet, forge::api::chain::outbox::replay mode, bool cancel) {
   namespace protocol = forge::chain::protocol;
   using forge::api::chain::destination;
   using forge::api::chain::outbox;
   using package_chain_api_component::require;
   using namespace std::chrono_literals;

   const destination target = forge::api::chain::http_endpoint{"primary"};
   const auto id = packet.id();
   const auto family = forge::db::core::family{"first-send"};
   const auto key = forge::db::core::record_key{{std::byte{1}}};
   const auto db = forge::db::mdbx::config{.path = path.string(), .families = {"objectdb", "first-send"}};
   const auto lane = forge::asio::affine::lane::options{.thread_name = "first-send"};
   auto driver = std::shared_ptr<forge::db::core::driver>{};
   auto admission = outbox::first_send_decision::defer;
   auto preparations = 0u;
   auto submissions = 0u;
   auto observations = 0u;
   const auto transport = outbox::transport{
       .submit = [&](protocol::chain_id network, destination to, protocol::transaction_submit_request request,
                     std::stop_token) -> boost::asio::awaitable<protocol::transaction_submit_response> {
          require(network == chain && to == target && request.transaction == packet,
                  "First-send recovery changed the signed packet or destination");
          auto read = co_await driver->begin_read();
          const auto latch = co_await read.get(family, key);
          require(latch == std::vector{static_cast<std::byte>(outbox::first_send_decision::proceed)},
                  "Submission began before the application latch committed");
          ++submissions;
          if (submissions == 1) {
             throw std::runtime_error{"Fixture lost the first acknowledgement"};
          }
          co_return protocol::transaction_submit_response{.id = id, .state = protocol::transaction_lifecycle::accepted};
       },
       .status = [&](protocol::chain_id network, destination to, protocol::transaction_id requested,
                     std::stop_token) -> boost::asio::awaitable<protocol::transaction_status_response> {
          require(network == chain && to == target && requested == id, "Reconciliation changed the packet identity");
          ++observations;
          auto response = protocol::transaction_status_response{};
          response.context.chain = chain;
          response.id = id;
          co_return response;
       }};
   const auto delivery = outbox::options{
       .poll_interval = 1ms,
       .timeout = 2s,
       .prepare_first_send = [&](protocol::chain_id network, const destination& to, protocol::transaction_id requested,
                                 const protocol::packed_transaction& stored, std::stop_token)
           -> boost::asio::awaitable<outbox::first_send_check> {
          require(network == chain && to == target && requested == id && stored == packet,
                  "Preparation did not receive the exact retained packet");
          auto probe = co_await driver->begin_transaction();
          co_await probe.rollback();
          ++preparations;
          co_return [&](forge::db::object::transaction& writer) -> boost::asio::awaitable<outbox::first_send_decision> {
             co_await writer.db_transaction().put(family, key, std::vector{static_cast<std::byte>(admission)});
             co_return admission;
          };
       }};

   {
      driver = co_await forge::db::mdbx::driver::open(db, lane);
      auto store = co_await forge::db::object::store::open(driver);
      outbox queue{store, transport, delivery};
      auto write = co_await store.begin_transaction();
      co_await queue.stage(write, chain, target, packet, {.mode = mode});
      co_await write.commit();
      co_await queue.recover();
      {
         auto read = co_await driver->begin_read();
         require(preparations == 1 && !submissions && !observations && !(co_await read.get(family, key)),
                 "Deferred admission sent the packet or committed tentative application writes");
      }
      co_await driver->async_close();
   }

   admission = cancel ? outbox::first_send_decision::deny : outbox::first_send_decision::proceed;
   co_await next_poll();
   {
      driver = co_await forge::db::mdbx::driver::open(db, lane);
      auto store = co_await forge::db::object::store::open(driver);
      outbox queue{store, transport, delivery};
      co_await queue.recover();
      {
         auto read = co_await driver->begin_read();
         const auto latch = co_await read.get(family, key);
         require(preparations == 2 && submissions == (cancel ? 0u : 1u) && !observations &&
                     latch == std::vector{static_cast<std::byte>(admission)},
                 "Restart did not commit the first-send admission or terminal denial");
      }
      co_await driver->async_close();
   }

   admission = cancel ? outbox::first_send_decision::proceed : outbox::first_send_decision::deny;
   co_await next_poll();
   driver = co_await forge::db::mdbx::driver::open(db, lane);
   auto store = co_await forge::db::object::store::open(driver);
   outbox queue{store, transport, delivery};
   co_await queue.recover();
   if (cancel) {
      auto rejected = false;
      try {
         (void)co_await queue.submit(chain, target, packet, {.mode = mode});
      } catch (const forge::chain::api::exceptions::admission_rejected&) {
         rejected = true;
      }
      require(rejected && preparations == 2 && !submissions && !observations,
              "Terminal denial did not survive restart and a repeated submission");
   } else {
      require(preparations == 2 && observations == 1 && submissions == (mode == outbox::replay::same_packed ? 2u : 1u),
              "Recovery repeated first-send admission or changed the retained replay policy");
   }
   co_await driver->async_close();
}

} // namespace

boost::asio::awaitable<void> test_staged_http_profile(const std::filesystem::path& path,
                                                      forge::chain::protocol::chain_id chain,
                                                      const forge::chain::protocol::packed_transaction& packet) {
   namespace protocol = forge::chain::protocol;
   using forge::api::chain::destination;
   using forge::api::chain::outbox;
   using package_chain_api_component::require;
   using namespace std::chrono_literals;

   const destination target = forge::api::chain::http_endpoint{"primary"};
   bool configured = false;
   unsigned profile_checks = 0, sends = 0, queries = 0;
   bool exact = false;
   const outbox::transport transport{
       .submit = [&](protocol::chain_id network, destination to, protocol::transaction_submit_request request,
                     std::stop_token) -> boost::asio::awaitable<protocol::transaction_submit_response> {
          ++sends;
          exact = configured && network == chain && to == target && request.transaction == packet;
          co_return protocol::transaction_submit_response{.id = request.transaction.id()};
       },
       .status = [&](protocol::chain_id, destination, protocol::transaction_id id,
                     std::stop_token) -> boost::asio::awaitable<protocol::transaction_status_response> {
          ++queries;
          co_return protocol::transaction_status_response{.id = id};
       },
       .configured = [&](const destination& to) {
          ++profile_checks;
          return configured && to == target;
       }};
   const forge::db::mdbx::config db{.path = path.string(), .families = {"objectdb"}};
   const forge::asio::affine::lane::options lane{.thread_name = "staged-http"};
   const outbox::options delivery{.poll_interval = 1ms};
   {
      auto driver = co_await forge::db::mdbx::driver::open(db, lane);
      auto store = co_await forge::db::object::store::open(driver);
      outbox queue{store, transport, delivery};
      auto write = co_await store.begin_transaction();
      const auto id = co_await queue.stage(write, chain, target, packet, {.mode = outbox::replay::reconcile_only});
      co_await write.commit();
      require(id == packet.id(), "Staging changed the transaction ID");
      co_await queue.recover();
      require(profile_checks && !sends && !queries, "Recovery attempted transport with an absent HTTP profile");
      co_await driver->async_close();
   }

   configured = true;
   auto driver = co_await forge::db::mdbx::driver::open(db, lane);
   auto store = co_await forge::db::object::store::open(driver);
   outbox queue{store, transport, delivery};
   co_await next_poll();
   co_await queue.recover();
   require(sends == 1 && exact && !queries, "Restoring the profile did not send the original staged packet once");
   co_await driver->async_close();
   co_await test_first_send(path.parent_path() / "deny-first-send", chain, packet, outbox::replay::reconcile_only, true);
   co_await test_first_send(path.parent_path() / "allow-first-send", chain, packet, outbox::replay::reconcile_only, false);
   co_await test_first_send(path.parent_path() / "replay-first-send", chain, packet, outbox::replay::same_packed, false);
}

} // namespace demo
