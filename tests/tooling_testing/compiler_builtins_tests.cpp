module;

#define BOOST_TEST_MODULE forge_tooling_testing_tests
#include <boost/test/included/unit_test.hpp>

#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

module forge.tooling.testing.host;

import forge.tooling.testing.exceptions;
import forge.tooling.testing.schema;
import forge.db.object.index;
import forge.vm.wasm.interpret.host_function;

#include "details/compiler_builtins.hxx"

namespace {

constexpr auto binary128_one_high = std::uint64_t{0x3fff000000000000ULL};
constexpr auto binary128_quiet_nan_high = std::uint64_t{0x7fff800000000000ULL};

// Literal WASM: apply is empty; the start function loops forever, without
// linear memory or host imports. Guest initialization must already be timed.
const std::vector<std::uint8_t> start_loop{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0a, 0x02, 0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x60,
    0x00, 0x00, 0x03, 0x03, 0x02, 0x00, 0x01, 0x07, 0x09, 0x01, 0x05, 0x61, 0x70, 0x70, 0x6c, 0x79, 0x00, 0x00,
    0x08, 0x01, 0x01, 0x0a, 0x0c, 0x02, 0x02, 0x00, 0x0b, 0x07, 0x00, 0x03, 0x40, 0x0c, 0x00, 0x0b, 0x0b,
};

// Literal WASM: apply invokes receiver 9 with the action parameter as flags.
const std::vector<std::uint8_t> nested_caller{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x0f, 0x02, 0x60, 0x04, 0x7e, 0x7e, 0x7f, 0x7f, 0x01, 0x7e,
    0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x02, 0x0c, 0x01, 0x03, 0x65, 0x6e, 0x76, 0x04, 0x63, 0x61, 0x6c, 0x6c, 0x00,
    0x00, 0x03, 0x02, 0x01, 0x01, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x09, 0x01, 0x05, 0x61, 0x70, 0x70, 0x6c, 0x79,
    0x00, 0x01, 0x0a, 0x0f, 0x01, 0x0d, 0x00, 0x42, 0x09, 0x20, 0x02, 0x41, 0x00, 0x41, 0x00, 0x10, 0x00, 0x1a, 0x0b,
};

// Literal WASM: __forge_call is empty; start writes table 17 under receiver 9.
const std::vector<std::uint8_t> nested_start_writer{
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x13, 0x03, 0x60, 0x06, 0x7e, 0x7e, 0x7e, 0x7e, 0x7f,
    0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7e, 0x7e, 0x00, 0x60, 0x00, 0x00, 0x02, 0x14, 0x01, 0x03, 0x65, 0x6e, 0x76,
    0x0c, 0x64, 0x62, 0x5f, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x5f, 0x69, 0x36, 0x34, 0x00, 0x00, 0x03, 0x03, 0x02,
    0x01, 0x02, 0x05, 0x03, 0x01, 0x00, 0x01, 0x07, 0x10, 0x01, 0x0c, 0x5f, 0x5f, 0x66, 0x6f, 0x72, 0x67, 0x65,
    0x5f, 0x63, 0x61, 0x6c, 0x6c, 0x00, 0x01, 0x08, 0x01, 0x02, 0x0a, 0x16, 0x02, 0x02, 0x00, 0x0b, 0x11, 0x00,
    0x42, 0x09, 0x42, 0x11, 0x42, 0x09, 0x42, 0x01, 0x41, 0x00, 0x41, 0x01, 0x10, 0x00, 0x1a, 0x0b,
};

bool has_schema_source_location(const forge::tooling::testing::exceptions::database_error& error) {
   const auto file = std::string_view{error.location().file_name()};
   return file.ends_with("schema.cppm") && error.location().line() != 0U;
}

} // namespace

BOOST_AUTO_TEST_CASE(quad_comparison_builtins_preserve_compiler_rt_unordered_results) {
   const auto builtins = forge::tooling::testing::compiler_builtins{};

   BOOST_TEST(builtins.__gttf2(0, binary128_quiet_nan_high, 0, binary128_one_high) == -1);
   BOOST_TEST(builtins.__gttf2(0, binary128_one_high, 0, binary128_quiet_nan_high) == -1);
   BOOST_TEST(builtins.__lttf2(0, binary128_quiet_nan_high, 0, binary128_one_high) == 1);
   BOOST_TEST(builtins.__lttf2(0, binary128_one_high, 0, binary128_quiet_nan_high) == 1);
}

BOOST_AUTO_TEST_CASE(nan_sort_keys_report_typed_database_errors) {
   const auto double_nan = forge::tooling::testing::float64{.bits = 0x7ff8'0000'0000'0000ULL};
   const auto quad_nan = forge::tooling::testing::float128{.words = {0U, binary128_quiet_nan_high}};

   BOOST_CHECK_EXCEPTION(static_cast<void>(forge::db::object::sort_key<forge::tooling::testing::float64>{}(double_nan)),
                         forge::tooling::testing::exceptions::database_error, has_schema_source_location);
   BOOST_CHECK_EXCEPTION(static_cast<void>(forge::db::object::sort_key<forge::tooling::testing::float128>{}(quad_nan)),
                         forge::tooling::testing::exceptions::database_error, has_schema_source_location);
}

BOOST_AUTO_TEST_CASE(contract_host_rejects_non_positive_execution_timeouts) {
   using namespace std::chrono_literals;

   BOOST_CHECK_THROW(forge::tooling::testing::host{forge::tooling::testing::execution_limits{.timeout = 0ms}},
                     forge::tooling::testing::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(contract_host_times_guest_start_before_apply_and_remains_reusable) {
   using namespace std::chrono_literals;
   auto host = forge::tooling::testing::host{{.timeout = 100ms}};
   const auto started = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(host.invoke(start_loop, 7U, 7U, 0U), forge::tooling::testing::exceptions::execution_timeout);
   BOOST_CHECK_LT(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count(), 2000);
   BOOST_CHECK_NO_THROW(host.invoke(nested_caller, 7U, 7U, 0U));
}

BOOST_AUTO_TEST_CASE(contract_host_cancels_guest_start_before_apply_and_remains_reusable) {
   using namespace std::chrono_literals;
   auto host = forge::tooling::testing::host{};
   auto stop = std::stop_source{};
   auto cancel = std::jthread{[&] {
      std::this_thread::sleep_for(100ms);
      stop.request_stop();
   }};
   BOOST_CHECK_THROW(host.invoke(start_loop, 7U, 7U, 0U, {}, {.cancel = stop.get_token()}),
                     forge::tooling::testing::exceptions::execution_cancelled);
   BOOST_CHECK_NO_THROW(host.invoke(nested_caller, 7U, 7U, 0U));
}

BOOST_AUTO_TEST_CASE(contract_host_installs_nested_receiver_and_read_only_state_before_start) {
   auto host = forge::tooling::testing::host{};
   host.register_contract(9U, nested_start_writer);
   const auto before = host.snapshot();
   BOOST_CHECK_THROW(host.invoke(nested_caller, 7U, 7U, 1U), forge::tooling::testing::exceptions::database_error);
   BOOST_TEST(host.snapshot() == before, boost::test_tools::per_element());
   BOOST_TEST(!host.find_table(7U, 9U, 17U).has_value());
   BOOST_TEST(!host.find_table(9U, 9U, 17U).has_value());
   BOOST_CHECK_NO_THROW(host.invoke(nested_caller, 7U, 7U, 0U));
   BOOST_TEST(host.find_primary(9U, 9U, 17U, 1U).has_value());
   BOOST_TEST(!host.find_table(7U, 9U, 17U).has_value());
}
