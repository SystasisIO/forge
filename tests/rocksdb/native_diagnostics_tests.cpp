module;

#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <rocksdb/env.h>
#include <rocksdb/listener.h>

module forge.rocksdb.store;

import forge.log.logger;

#include "../../libraries/rocksdb/details/native_diagnostics.hxx"

namespace {

class native_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      records.push_back(record);
      if (reenter) { reenter(); }
   }
   std::vector<forge::log_record> records;
   std::function<void()> reenter;
};

void native_message(forge::rocksdb::detail::native_diagnostics& diagnostics,
                    ::rocksdb::InfoLogLevel level, const char* format, ...) {
   va_list args;
   va_start(args, format);
   diagnostics.Logv(level, format, args);
   va_end(args);
}

} // namespace

BOOST_AUTO_TEST_CASE(rocksdb_native_callbacks_are_deferred_redacted_and_keep_background_failure) {
   auto previous = forge::logger::get("forge.db.rocksdb");
   auto route = forge::logger{"forge.db.rocksdb"};
   auto captured = std::make_shared<native_capture_sink>();
   route.set_log_level(forge::log_level::all);
   route.add_sink(captured);
   forge::logger::update("forge.db.rocksdb", route);
   auto restore = boost::scope::scope_exit{[&] { forge::logger::update("forge.db.rocksdb", previous); }};
   auto diagnostics = forge::rocksdb::detail::native_diagnostics{};
   const auto secret = std::string{"native-secret-key-must-not-escape-94137"};
   auto status = ::rocksdb::Status{::rocksdb::Status::Corruption(secret), ::rocksdb::Status::kFatalError};
   diagnostics.OnBackgroundError(::rocksdb::BackgroundErrorReason::kCompaction, &status);
   native_message(diagnostics, ::rocksdb::ERROR_LEVEL, "%s", secret.c_str());
   native_message(diagnostics, ::rocksdb::FATAL_LEVEL, "%s", secret.c_str());
   native_message(diagnostics, ::rocksdb::WARN_LEVEL, "%s", secret.c_str());
   diagnostics.record_result(::rocksdb::Status::Corruption(secret));
   BOOST_CHECK(captured->records.empty());
   BOOST_CHECK(status.IsCorruption());
   BOOST_CHECK(status.severity() == ::rocksdb::Status::kFatalError);
   captured->reenter = [&] { diagnostics.drain(); };
   diagnostics.drain();
   BOOST_REQUIRE_EQUAL(captured->records.size(), 5U);
   auto warnings = std::size_t{0};
   for (const auto& record : captured->records) {
      if (record.level == forge::log_level::warn) { ++warnings; }
      else { BOOST_CHECK(record.level == forge::log_level::error); }
      BOOST_CHECK(forge::format_json_log_record(record).find(secret) == std::string::npos);
   }
   BOOST_TEST(warnings == 1U);
   const auto count = captured->records.size();
   diagnostics.drain();
   BOOST_TEST(captured->records.size() == count);
   diagnostics.record_result(::rocksdb::Status::Busy(secret));
   BOOST_TEST(captured->records.size() == count);
   diagnostics.drain();
   BOOST_REQUIRE_EQUAL(captured->records.size(), count + 1U);
   BOOST_CHECK(captured->records.back().level == forge::log_level::debug);
}
