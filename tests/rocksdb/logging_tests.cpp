#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

import forge.log.logger;
import forge.rocksdb.exceptions;
import forge.rocksdb.store;

namespace {

class db_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override { records.push_back(record); }
   std::vector<forge::log_record> records;
};

class db_failing_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override { throw std::runtime_error{"sink-failed"}; }
};

std::string event_of(const forge::log_record& record) {
   for (const auto& field : record.fields) {
      if (field.key == "event") {
         return field.value;
      }
   }
   return {};
}

} // namespace

BOOST_AUTO_TEST_CASE(rocksdb_logging_is_structured_redacted_and_creates_no_native_log) {
   const auto root = std::filesystem::temp_directory_path() /
      ("forge_rocksdb_logging_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   auto previous = forge::logger::get("forge.db.rocksdb");
   auto route = forge::logger{"forge.db.rocksdb"};
   auto captured = std::make_shared<db_capture_sink>();
   route.set_log_level(forge::log_level::all);
   route.add_sink(captured);
   forge::logger::update("forge.db.rocksdb", route);
   auto cleanup = boost::scope::scope_exit{[&] {
      forge::logger::update("forge.db.rocksdb", previous);
      std::filesystem::remove_all(root);
   }};

   const auto secret = std::string{"private-key-and-payload-must-never-appear-93842"};
   {
      auto db = forge::rocksdb::store{forge::rocksdb::config{
         .path = (root / secret).string(), .column_families = {"records"}}};
      const auto family = forge::rocksdb::family{"records"};
      const auto before_transactions = captured->records.size();
      db.put(family, forge::rocksdb::to_bytes(secret), forge::rocksdb::to_bytes(secret), {.sync = true});
      BOOST_REQUIRE(db.get(family, forge::rocksdb::to_bytes(secret)).has_value());
      BOOST_CHECK(!db.get(family, forge::rocksdb::to_bytes("missing")).has_value());
      auto transaction = db.begin();
      transaction.put(family, forge::rocksdb::to_bytes("rolled-back"), forge::rocksdb::to_bytes(secret));
      transaction.rollback();
      BOOST_TEST(captured->records.size() == before_transactions);
      db.flush_wal(true);
      BOOST_TEST(captured->records.size() == before_transactions);
   }
   BOOST_CHECK_THROW(((void)forge::rocksdb::store{forge::rocksdb::config{
      .path = (root / "missing" / secret).string(), .create_if_missing = false}}),
      forge::rocksdb::exceptions::invalid_argument);

   auto commits = std::size_t{0};
   auto errors = std::size_t{0};
   for (const auto& record : captured->records) {
      BOOST_TEST(record.logger == "forge.db.rocksdb");
      BOOST_TEST(!event_of(record).empty());
      BOOST_CHECK(forge::format_json_log_record(record).find(secret) == std::string::npos);
      if (record.level == forge::log_level::warn) {
         BOOST_CHECK(event_of(record) == "native.diagnostics");
      }
      if (event_of(record) == "transaction.outcomes") {
         for (const auto& field : record.fields) {
            if (field.key == "committed") { commits += std::stoull(field.value); }
         }
         BOOST_CHECK(record.level == forge::log_level::debug);
      }
      if (event_of(record) == "operation.failed") {
         ++errors;
      }
   }
   BOOST_TEST(commits == 1U);
   BOOST_TEST(errors >= 1U);
   for (const auto& entry : std::filesystem::recursive_directory_iterator{root}) {
      BOOST_CHECK(!entry.path().filename().string().starts_with("LOG"));
   }
}

BOOST_AUTO_TEST_CASE(rocksdb_logging_filter_and_sink_failure_do_not_change_commits) {
   const auto root = std::filesystem::temp_directory_path() /
      ("forge_rocksdb_log_failure_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
   auto previous = forge::logger::get("forge.db.rocksdb");
   auto route = forge::logger{"forge.db.rocksdb"};
   route.set_log_level(forge::log_level::all);
   route.add_sink(std::make_shared<db_failing_sink>());
   forge::logger::update("forge.db.rocksdb", route);
   auto cleanup = boost::scope::scope_exit{[&] {
      forge::logger::update("forge.db.rocksdb", previous);
      std::filesystem::remove_all(root);
   }};
   {
      auto db = forge::rocksdb::store{forge::rocksdb::config{.path = root.string()}};
      db.put(forge::rocksdb::family{"default"}, forge::rocksdb::to_bytes("key"),
             forge::rocksdb::to_bytes("one"), {.sync = true});
      route.set_enabled(false);
      db.put(forge::rocksdb::family{"default"}, forge::rocksdb::to_bytes("key"),
             forge::rocksdb::to_bytes("two"), {.sync = true});
   }
   auto reopened = forge::rocksdb::store{forge::rocksdb::config{.path = root.string()}};
   const auto value = reopened.get(forge::rocksdb::family{"default"}, forge::rocksdb::to_bytes("key"));
   BOOST_REQUIRE(value.has_value());
   BOOST_TEST(forge::rocksdb::to_string(*value) == "two");
}
