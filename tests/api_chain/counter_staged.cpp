module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>
#include <coroutine>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

module test.api_chain.counter_client;

import forge.api.chain.outbox;
import forge.asio.affine;
import forge.db.core.driver;
import forge.db.mdbx.driver;
import forge.db.object.registration;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.transaction;
import package.chain_api_component.test_support;

namespace demo {

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
   // Missing profiles schedule the next check one poll interval ahead.
   boost::asio::steady_timer tick{co_await boost::asio::this_coro::executor, 10ms};
   co_await tick.async_wait(boost::asio::use_awaitable);
   co_await queue.recover();
   require(sends == 1 && exact && !queries, "Restoring the profile did not send the original staged packet once");
   co_await driver->async_close();
}

} // namespace demo
