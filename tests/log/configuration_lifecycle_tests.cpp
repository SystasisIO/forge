module;

#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

module forge.log.logger_config;

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.notification;
import forge.asio.runtime;
import forge.log.exceptions;

#include "../../libraries/log/details/configured_sink_runtime.hxx"

namespace {
using namespace std::chrono_literals;

void verify_shutdown_boundary(bool prepared) {
   auto runtime = forge::asio::runtime{};
   auto owner = forge::detail::configured_sink_runtime{};
   auto config = std::vector<forge::sink_config>{{.name = "output", .type = "console"}};
   auto admitted = std::promise<void>{};
   auto admitted_result = admitted.get_future();
   auto release = std::promise<void>{};
   auto release_result = release.get_future();
   auto rejected = false;
   auto error = std::exception_ptr{};
   auto worker = std::jthread{[&] {
      try {
         auto operation = owner.begin_configuration(config);
         auto outputs = std::unordered_map<std::string, std::shared_ptr<forge::sink>>{};
         if (prepared) {
            outputs = operation.prepare(config);
         }
         admitted.set_value();
         // This is a test barrier inside the real admitted operation, not an
         // OS scheduling assumption or a production-only callback hook.
         if (release_result.wait_for(5s) != std::future_status::ready) {
            return;
         }
         try {
            if (prepared) {
               auto publication = operation.acquire_publication();
            } else {
               outputs = operation.prepare(config);
            }
         } catch (const forge::log::exceptions::invalid_config&) {
            rejected = true;
         }
      } catch (...) {
         error = std::current_exception();
      }
   }};
   auto unblock = boost::scope::scope_exit{[&] { release.set_value(); }};
   BOOST_REQUIRE(admitted_result.wait_for(2s) == std::future_status::ready);
   const auto stopped = forge::asio::blocking::run(runtime, owner.shutdown(20ms));
   BOOST_TEST(!stopped);
   release.set_value();
   unblock.set_active(false);
   worker.join();
   if (error) {
      std::rethrow_exception(error);
   }
   BOOST_TEST(rejected);
   BOOST_TEST(forge::asio::blocking::run(runtime, owner.shutdown(2s)));
   BOOST_TEST(owner.snapshot().queued_records == 0u);
}
} // namespace

BOOST_AUTO_TEST_SUITE(log_configuration_lifecycle)
BOOST_AUTO_TEST_CASE(shutdown_waits_for_admitted_preparation, * boost::unit_test::timeout(15)) {
   verify_shutdown_boundary(false);
}
BOOST_AUTO_TEST_CASE(shutdown_waits_for_prepared_operation_and_rejects_publication,
                     * boost::unit_test::timeout(15)) {
   verify_shutdown_boundary(true);
}
BOOST_AUTO_TEST_SUITE_END()
