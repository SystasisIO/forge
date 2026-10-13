#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

import forge.asio.affine;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.mdbx.driver;
import forge.db.mdbx.diagnostics;
import forge.db.mdbx.exceptions;
import forge.log.logger;

namespace {

class mdbx_log_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      const auto lock = std::scoped_lock{mutex};
      records.push_back(record);
   }
   std::mutex mutex;
   std::vector<forge::log_record> records;
};

std::vector<std::byte> logging_bytes(const std::string& value) {
   return {reinterpret_cast<const std::byte*>(value.data()),
           reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

} // namespace

BOOST_AUTO_TEST_CASE(mdbx_logging_keeps_commit_and_rollback_outcomes_without_payload_or_path) {
   const auto root = std::filesystem::temp_directory_path() /
      ("forge_mdbx_logging_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   forge::db::mdbx::diagnostics::drain();
   auto previous = forge::logger::get("forge.db.mdbx");
   auto route = forge::logger{"forge.db.mdbx"};
   auto captured = std::make_shared<mdbx_log_capture_sink>();
   route.set_log_level(forge::log_level::all);
   route.add_sink(captured);
   forge::logger::update("forge.db.mdbx", route);
   auto cleanup = boost::scope::scope_exit{[&] {
      forge::logger::update("forge.db.mdbx", previous);
      std::filesystem::remove_all(root);
   }};
   const auto secret = std::string{"mdbx-secret-key-and-payload-38569"};
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto config = forge::db::mdbx::config{.path = (root / secret).string(), .families = {"records"}};
      config.map.lower_size = 1024U * 1024U;
      config.map.current_size = 1024U * 1024U;
      config.map.upper_size = 1024U * 1024U;
      auto driver = co_await forge::db::mdbx::driver::open(config, lane.get_executor());
      const auto before_transactions = captured->records.size();
      {
         auto active = co_await driver->begin_transaction();
         co_await active.put(forge::db::core::family{"records"},
                             forge::db::core::record_key{logging_bytes(secret)}, logging_bytes(secret));
         co_await active.commit();
      }
      {
         auto active = co_await driver->begin_transaction();
         const auto payload = std::vector<std::byte>(64U * 1024U, std::byte{0x5a});
         auto full = false;
         for (auto ordinal = 0; ordinal < 64 && !full; ++ordinal) {
            try {
               co_await active.put(forge::db::core::family{"records"},
                                   forge::db::core::record_key{logging_bytes(secret + std::to_string(ordinal))}, payload);
            } catch (const forge::db::mdbx::exceptions::map_full&) {
               full = true;
            }
         }
         BOOST_REQUIRE(full);
         co_await active.rollback();
      }
      // Neither live writer operations nor commit/rollback call arbitrary sinks.
      BOOST_TEST(captured->records.size() == before_transactions);
      {
         auto snapshot = co_await driver->begin_read();
         BOOST_REQUIRE((co_await snapshot.get(forge::db::core::family{"records"},
            forge::db::core::record_key{logging_bytes(secret)})).has_value());
         BOOST_CHECK(!(co_await snapshot.get(forge::db::core::family{"records"},
            forge::db::core::record_key{logging_bytes("missing")})).has_value());
         BOOST_CHECK(!(co_await snapshot.get(forge::db::core::family{"records"},
            forge::db::core::record_key{logging_bytes(secret + "0")})).has_value());
      }
      co_await driver->async_flush(false);
      co_await driver->async_flush(true);
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());
   auto commits = std::size_t{0};
   auto rollbacks = std::size_t{0};
   auto errors = std::size_t{0};
   auto poll_requests = std::size_t{0};
   auto sync_requests = std::size_t{0};
   for (const auto& record : captured->records) {
      BOOST_TEST(record.logger == "forge.db.mdbx");
      BOOST_CHECK(forge::format_json_log_record(record).find(secret) == std::string::npos);
      if (record.level == forge::log_level::warn) {
         BOOST_CHECK(record.message == "native diagnostics observed");
      }
      for (const auto& field : record.fields) {
         if (field.key != "event") { continue; }
         if (field.value == "transaction.outcomes") {
            for (const auto& outcome : record.fields) {
               if (outcome.key == "committed") { commits += std::stoull(outcome.value); }
               if (outcome.key == "rolled_back") { rollbacks += std::stoull(outcome.value); }
            }
            BOOST_CHECK(record.level == forge::log_level::debug);
         }
         if (field.value == "operation.failed") { ++errors; }
         BOOST_CHECK(field.value != "environment.flushed");
         if (field.value == "environment.flush_request_completed") {
            BOOST_CHECK(record.level == forge::log_level::debug);
            for (const auto& request : record.fields) {
               if (request.key == "sync" && request.value == "false") { ++poll_requests; }
               if (request.key == "sync" && request.value == "true") { ++sync_requests; }
            }
         }
      }
   }
   BOOST_TEST(commits == 1U);
   BOOST_TEST(rollbacks == 1U);
   BOOST_TEST(errors >= 1U);
   BOOST_TEST(poll_requests == 1U);
   BOOST_TEST(sync_requests == 1U);
}
