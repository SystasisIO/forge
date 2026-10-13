module;

#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <mdbx.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

module forge.db.mdbx.diagnostics;

import forge.log.logger;

#include "../../libraries/db/mdbx/details/native_diagnostics.hxx"

namespace {

class mdbx_native_capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      records.push_back(record);
      if (reenter) { reenter(); }
   }
   std::vector<forge::log_record> records;
   std::function<void()> reenter;
};

void mdbx_native_message(MDBX_log_level_t level, const char* format, ...) {
   va_list args;
   va_start(args, format);
   forge::db::mdbx::detail::native_diagnostics::capture(level, "vendor_function", 42, format, args);
   va_end(args);
}

} // namespace

BOOST_AUTO_TEST_CASE(mdbx_process_diagnostic_ownership_preserves_flags_and_defers_all_sinks) {
   const auto before = mdbx_setup_debug(MDBX_LOG_DONTCHANGE, MDBX_DBG_DONTCHANGE, MDBX_LOGGER_DONTCHANGE);
   forge::db::mdbx::diagnostics::claim_process();
   forge::db::mdbx::diagnostics::claim_process();
   BOOST_CHECK(forge::db::mdbx::diagnostics::installed());
   BOOST_TEST(mdbx_setup_debug(MDBX_LOG_DONTCHANGE, MDBX_DBG_DONTCHANGE, MDBX_LOGGER_DONTCHANGE) == before);
   forge::db::mdbx::diagnostics::drain();

   auto previous = forge::logger::get("forge.db.mdbx");
   auto route = forge::logger{"forge.db.mdbx"};
   auto captured = std::make_shared<mdbx_native_capture_sink>();
   route.set_log_level(forge::log_level::all);
   route.add_sink(captured);
   forge::logger::update("forge.db.mdbx", route);
   auto restore = boost::scope::scope_exit{[&] { forge::logger::update("forge.db.mdbx", previous); }};
   const auto secret = std::string{"mdbx-key-dump-must-never-escape-27598"};
   mdbx_native_message(MDBX_LOG_WARN, "%s", secret.c_str());
   mdbx_native_message(MDBX_LOG_ERROR, "%s", secret.c_str());
   // Synthetic callback only: no native panic/abort is suppressed or replaced.
   const auto saved_errno = errno;
   errno = EAGAIN;
#if !defined(_WIN32)
   auto output = std::array<int, 2>{};
   BOOST_REQUIRE(::pipe(output.data()) == 0);
   BOOST_REQUIRE(::fcntl(output[0], F_SETFL, O_NONBLOCK) == 0);
   const auto original_stderr = ::dup(STDERR_FILENO);
   BOOST_REQUIRE(original_stderr >= 0);
   auto restore_stderr = boost::scope::scope_exit{[&] {
      static_cast<void>(::dup2(original_stderr, STDERR_FILENO));
      static_cast<void>(::close(original_stderr));
      static_cast<void>(::close(output[0]));
      static_cast<void>(::close(output[1]));
   }};
   BOOST_REQUIRE(::dup2(output[1], STDERR_FILENO) >= 0);
   errno = EAGAIN;
#endif
   mdbx_native_message(MDBX_LOG_FATAL, "%s", secret.c_str());
   BOOST_TEST(errno == EAGAIN);
   errno = saved_errno;
#if !defined(_WIN32)
   BOOST_REQUIRE(::dup2(original_stderr, STDERR_FILENO) >= 0);
   auto buffer = std::array<char, 128>{};
   const auto read = ::read(output[0], buffer.data(), buffer.size());
   BOOST_REQUIRE(read > 0);
   const auto notice = std::string{buffer.data(), static_cast<std::size_t>(read)};
   BOOST_TEST(notice == "forge.db.mdbx native.fatal: diagnostic text redacted\n");
   BOOST_CHECK(notice.find(secret) == std::string::npos);
#endif
   BOOST_CHECK(captured->records.empty());
   captured->reenter = [] { forge::db::mdbx::diagnostics::drain(); };
   forge::db::mdbx::diagnostics::drain();
   BOOST_REQUIRE_EQUAL(captured->records.size(), 3U);
   auto critical = std::size_t{0};
   for (const auto& record : captured->records) {
      BOOST_TEST(record.logger == "forge.db.mdbx");
      BOOST_CHECK(forge::format_json_log_record(record).find(secret) == std::string::npos);
      auto scope = std::string{};
      for (const auto& field : record.fields) {
         BOOST_CHECK(field.key != "store");
         BOOST_CHECK(field.key != "path");
         if (field.key == "scope") { scope = field.value; }
      }
      BOOST_TEST(scope == "process");
      if (record.level == forge::log_level::error) { ++critical; }
   }
   BOOST_TEST(critical == 2U);
   BOOST_CHECK(captured->records.back().level == forge::log_level::warn);
   const auto count = captured->records.size();
   forge::db::mdbx::diagnostics::drain();
   BOOST_TEST(captured->records.size() == count);
}
