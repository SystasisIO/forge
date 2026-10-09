#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <source_location>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

import forge.exceptions;

namespace test_http_exceptions {

enum class code : int {
   not_found = 404,
   internal = 500,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "test.http")

using not_found = forge::exceptions::coded_exception<code, code::not_found>;

} // namespace test_http_exceptions

namespace test_product_exceptions {

enum class code : std::uint8_t {
   chunk_not_found = 1,
   busy = 2,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "test.cache")

using chunk_not_found = forge::exceptions::coded_exception<code, code::chunk_not_found>;

struct retry_details {
   std::chrono::milliseconds after;
   std::unique_ptr<std::string> opaque;
};

class busy : public forge::exceptions::coded_exception<code, code::busy> {
 public:
   busy(retry_details data, std::string message, forge::exceptions::fields context,
        std::source_location location)
       : coded_exception(std::move(message), std::move(context), location), _data(std::move(data)) {}

   const retry_details& data() const noexcept { return _data; }

 private:
   retry_details _data;
};

} // namespace test_product_exceptions

BOOST_AUTO_TEST_SUITE(exception_test_suite)

BOOST_AUTO_TEST_CASE(context_fields_are_formatted_and_redacted) {
   const forge::exceptions::context_error error{
       "open vault",
       {forge::exceptions::ctx("path", "/tmp/forge.vault"), forge::exceptions::secret("passphrase", "correct horse battery staple")},
       std::source_location::current()};

   const std::string text = error.what();
   BOOST_CHECK(text.find("open vault") != std::string::npos);
   BOOST_CHECK(text.find("path=/tmp/forge.vault") != std::string::npos);
   BOOST_CHECK(text.find("passphrase=<redacted>") != std::string::npos);
   BOOST_CHECK(text.find("correct horse battery staple") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(context_error_keeps_optional_error_code) {
   const forge::exceptions::context_error error{"deadline exceeded",
                                         {forge::exceptions::ctx("phase", "startup")},
                                         std::source_location::current(),
                                         std::make_error_code(std::errc::timed_out)};

   BOOST_CHECK_EQUAL(error.message(), "deadline exceeded");
   BOOST_CHECK_EQUAL(error.context().size(), 1u);
   BOOST_CHECK(error.code() == std::make_error_code(std::errc::timed_out));
   BOOST_CHECK(std::string(error.what()).find("timed out") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(throw_exception_throws_concrete_coded_exception) {
   try {
      FORGE_THROW_EXCEPTION(test_http_exceptions::not_found, "route not found", forge::exceptions::ctx("path", "/missing"));
   } catch (const test_http_exceptions::not_found& error) {
      BOOST_CHECK_EQUAL(error.message(), "route not found");
      BOOST_CHECK_EQUAL(error.code().value(), 404);
      BOOST_CHECK_EQUAL(std::string(error.code().category().name()), "test.http");
      BOOST_CHECK(std::string(error.what()).find("path=/missing") != std::string::npos);
      return;
   }

   BOOST_FAIL("expected concrete typed exception");
}

BOOST_AUTO_TEST_CASE(custom_uint8_exception_category_works) {
   try {
      FORGE_THROW_EXCEPTION(test_product_exceptions::chunk_not_found, "chunk not found");
   } catch (const test_product_exceptions::chunk_not_found& error) {
      BOOST_CHECK_EQUAL(error.code().value(), 1);
      BOOST_CHECK_EQUAL(std::string(error.code().category().name()), "test.cache");
      return;
   }

   BOOST_FAIL("expected product typed exception");
}

BOOST_AUTO_TEST_CASE(throw_code_throws_runtime_coded_exception_with_call_site_context) {
   const auto runtime_code = test_http_exceptions::code::internal;
   unsigned expected_line = 0;

   try {
      expected_line = __LINE__ + 1;
      FORGE_THROW_CODE(runtime_code, "dynamic failure", forge::exceptions::ctx("phase", "runtime-map"));
   } catch (const forge::exceptions::runtime_coded_exception<test_http_exceptions::code>& error) {
      BOOST_CHECK_EQUAL(static_cast<int>(error.value()), static_cast<int>(test_http_exceptions::code::internal));
      BOOST_CHECK_EQUAL(error.message(), "dynamic failure");
      BOOST_CHECK_EQUAL(error.code().value(), 500);
      BOOST_CHECK_EQUAL(std::string(error.code().category().name()), "test.http");
      BOOST_CHECK_EQUAL(error.context().size(), 1u);
      BOOST_CHECK_EQUAL(error.context().front().key, "phase");
      BOOST_CHECK_EQUAL(error.context().front().value, "runtime-map");
      BOOST_CHECK_EQUAL(error.location().line(), expected_line);
      BOOST_CHECK(std::string(error.location().file_name()).find("exception_tests.cpp") != std::string::npos);
      return;
   }

   BOOST_FAIL("expected runtime coded exception");
}

BOOST_AUTO_TEST_CASE(typed_data_throw_preserves_move_only_data_and_call_site) {
   unsigned expected_line = 0;
   unsigned evaluations = 0;
   auto data = [&] {
      ++evaluations;
      return test_product_exceptions::retry_details{
         std::chrono::milliseconds{250}, std::make_unique<std::string>("private operation data")};
   };

   try {
      try {
         expected_line = __LINE__ + 2;
         FORGE_THROW_EXCEPTION_WITH_DATA(test_product_exceptions::busy, data(), "service is busy",
                                         forge::exceptions::secret("credential", "private diagnostic data"));
      }
      FORGE_CAPTURE_AND_RETHROW("service call", forge::exceptions::ctx("phase", "admission"))
   } catch (const test_product_exceptions::busy& error) {
      BOOST_CHECK_EQUAL(evaluations, 1u);
      BOOST_CHECK_EQUAL(error.data().after.count(), 250);
      BOOST_REQUIRE(error.data().opaque);
      BOOST_CHECK_EQUAL(*error.data().opaque, "private operation data");
      BOOST_CHECK_EQUAL(error.code().value(), 2);
      BOOST_CHECK_EQUAL(std::string(error.code().category().name()), "test.cache");
      BOOST_CHECK_EQUAL(error.location().line(), expected_line);
      BOOST_CHECK_EQUAL(error.context_frames().size(), 1u);
      const auto diagnostic = forge::exceptions::format_exception_chain(error);
      BOOST_CHECK(diagnostic.find("private operation data") == std::string::npos);
      BOOST_CHECK(diagnostic.find("private diagnostic data") == std::string::npos);
      BOOST_CHECK(diagnostic.find("credential=<redacted>") != std::string::npos);
      return;
   }

   BOOST_FAIL("expected original exception and typed operational data");
}

BOOST_AUTO_TEST_CASE(capture_and_rethrow_preserves_nested_exception) {
   try {
      try {
         throw std::runtime_error("inner failure");
      }
      FORGE_CAPTURE_AND_RETHROW("outer context", forge::exceptions::ctx("phase", "initialize"))
   } catch (const forge::exceptions::context_error& error) {
      const auto chain = forge::exceptions::format_exception_chain(error);
      BOOST_CHECK(chain.find("outer context") != std::string::npos);
      BOOST_CHECK(chain.find("phase=initialize") != std::string::npos);
      BOOST_CHECK(chain.find("inner failure") != std::string::npos);
      return;
   }

   BOOST_FAIL("expected context_error");
}

BOOST_AUTO_TEST_CASE(capture_and_rethrow_preserves_forge_exceptions_dynamic_type) {
   try {
      try {
         FORGE_THROW_EXCEPTION(test_product_exceptions::chunk_not_found, "chunk not found",
                             forge::exceptions::ctx("ref", "bafk..."));
      }
      FORGE_CAPTURE_AND_RETHROW("read cache", forge::exceptions::ctx("phase", "lookup"))
   } catch (const test_product_exceptions::chunk_not_found& error) {
      const std::string text = error.what();
      BOOST_CHECK_EQUAL(error.code().value(), 1);
      BOOST_CHECK_EQUAL(error.context_frames().size(), 1u);
      BOOST_CHECK(text.find("chunk not found") != std::string::npos);
      BOOST_CHECK(text.find("read cache") != std::string::npos);
      BOOST_CHECK(text.find("phase=lookup") != std::string::npos);
      return;
   }

   BOOST_FAIL("expected original typed exception to be preserved");
}

BOOST_AUTO_TEST_CASE(assert_macro_throws_std_compatible_context_error) {
   BOOST_CHECK_EXCEPTION(FORGE_ASSERT(false, "broken invariant", forge::exceptions::ctx("value", 42)), forge::exceptions::context_error,
                         [](const forge::exceptions::context_error& error) {
                            const std::string text = error.what();
                            return error.code() == std::make_error_code(std::errc::invalid_argument) &&
                                   text.find("broken invariant") != std::string::npos &&
                                   text.find("expression=false") != std::string::npos &&
                                   text.find("value=42") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_CASE(deadline_macro_throws_timeout_context_error) {
   BOOST_CHECK_EXCEPTION(FORGE_CHECK_DEADLINE(std::chrono::system_clock::now() - std::chrono::milliseconds(1),
                                            forge::exceptions::ctx("phase", "render")),
                         forge::exceptions::context_error, [](const forge::exceptions::context_error& error) {
                            return error.code() == std::make_error_code(std::errc::timed_out) &&
                                   std::string(error.what()).find("phase=render") != std::string::npos;
                         });
}

BOOST_AUTO_TEST_SUITE_END()
