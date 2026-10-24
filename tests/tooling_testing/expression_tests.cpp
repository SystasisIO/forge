#define BOOST_TEST_MODULE forge_tooling_testing_expression_tests
#include <boost/test/included/unit_test.hpp>

#include <array>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <utility>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

import forge.expression.artifact_codec;
import forge.expression.exceptions;
import forge.expression.program;
import forge.raw.codec;
import forge.tooling.testing.conversion;
import forge.tooling.testing.host;
import forge.vm.wasm.execution.profile;
import forge.vm.wasm.interpret.exceptions;

namespace {

namespace expression = forge::expression;
namespace testing = forge::tooling::testing;
using namespace std::chrono_literals;

// Literal WASM v1: four memory pages, apply(i64,i64,i64), and exact SDK
// imports read_action_data, the three expression_v1 functions,
// set_action_return_value and db_store_i64. The action selects compile (0),
// import (1), evaluate (2), or an out-of-bounds input pointer (3). A DB write
// precedes the host import so every failure must roll back observable state.
constexpr auto code = std::to_array<std::uint8_t>({
    0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x24, 0x05, 0x60, 0x03, 0x7e, 0x7e, 0x7e, 0x00, 0x60, 0x02,
    0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x04, 0x7f, 0x7f, 0x7f, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f, 0x00, 0x60, 0x06,
    0x7e, 0x7e, 0x7e, 0x7e, 0x7f, 0x7f, 0x01, 0x7f, 0x02, 0x9d, 0x01, 0x06, 0x03, 0x65, 0x6e, 0x76, 0x10, 0x72, 0x65,
    0x61, 0x64, 0x5f, 0x61, 0x63, 0x74, 0x69, 0x6f, 0x6e, 0x5f, 0x64, 0x61, 0x74, 0x61, 0x00, 0x01, 0x03, 0x65, 0x6e,
    0x76, 0x15, 0x65, 0x78, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x5f, 0x63, 0x6f, 0x6d, 0x70, 0x69, 0x6c,
    0x65, 0x5f, 0x76, 0x31, 0x00, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x14, 0x65, 0x78, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69,
    0x6f, 0x6e, 0x5f, 0x69, 0x6d, 0x70, 0x6f, 0x72, 0x74, 0x5f, 0x76, 0x31, 0x00, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x16,
    0x65, 0x78, 0x70, 0x72, 0x65, 0x73, 0x73, 0x69, 0x6f, 0x6e, 0x5f, 0x65, 0x76, 0x61, 0x6c, 0x75, 0x61, 0x74, 0x65,
    0x5f, 0x76, 0x31, 0x00, 0x02, 0x03, 0x65, 0x6e, 0x76, 0x17, 0x73, 0x65, 0x74, 0x5f, 0x61, 0x63, 0x74, 0x69, 0x6f,
    0x6e, 0x5f, 0x72, 0x65, 0x74, 0x75, 0x72, 0x6e, 0x5f, 0x76, 0x61, 0x6c, 0x75, 0x65, 0x00, 0x03, 0x03, 0x65, 0x6e,
    0x76, 0x0c, 0x64, 0x62, 0x5f, 0x73, 0x74, 0x6f, 0x72, 0x65, 0x5f, 0x69, 0x36, 0x34, 0x00, 0x04, 0x03, 0x02, 0x01,
    0x00, 0x05, 0x03, 0x01, 0x00, 0x04, 0x07, 0x09, 0x01, 0x05, 0x61, 0x70, 0x70, 0x6c, 0x79, 0x00, 0x06, 0x0a, 0x7a,
    0x01, 0x78, 0x01, 0x01, 0x7f, 0x42, 0x07, 0x42, 0x11, 0x42, 0x07, 0x42, 0x01, 0x41, 0x00, 0x41, 0x01, 0x10, 0x05,
    0x1a, 0x41, 0x00, 0x41, 0x80, 0x80, 0x08, 0x10, 0x00, 0x21, 0x03, 0x41, 0x80, 0x80, 0x08, 0x20, 0x02, 0x42, 0x00,
    0x51, 0x04, 0x7f, 0x41, 0x00, 0x20, 0x03, 0x41, 0x80, 0x80, 0x08, 0x41, 0x80, 0x80, 0x08, 0x10, 0x01, 0x05, 0x20,
    0x02, 0x42, 0x01, 0x51, 0x04, 0x7f, 0x41, 0x00, 0x20, 0x03, 0x41, 0x80, 0x80, 0x08, 0x41, 0x80, 0x80, 0x08, 0x10,
    0x02, 0x05, 0x20, 0x02, 0x42, 0x03, 0x51, 0x04, 0x7f, 0x41, 0x80, 0x80, 0x10, 0x41, 0x01, 0x41, 0x80, 0x80, 0x08,
    0x41, 0x80, 0x80, 0x08, 0x10, 0x01, 0x05, 0x41, 0x00, 0x20, 0x03, 0x41, 0x80, 0x80, 0x08, 0x41, 0x80, 0x80, 0x08,
    0x10, 0x03, 0x0b, 0x0b, 0x0b, 0x10, 0x04, 0x0b,
});
static_assert(code.size() == 350U);

forge::raw::bytes invoke(testing::host& host, std::uint64_t action, forge::raw::bytes input,
                         testing::invocation_options options = {}) {
   return host.invoke(code, 7U, 7U, action, std::move(input), std::move(options)).return_value;
}

expression::artifact prepare(expression::runtime& runtime, std::string source = "x + 1") {
   return expression::program{runtime, source, std::array{expression::variable{"x", expression::type::int64}}}
       .artifact();
}

// This wrapper only reports its PID and execs the genuine worker. It owns no
// protocol implementation. SIGSTOP then makes an in-flight import wait until
// the shared deadline/cancellation terminates and reaps that exact child.
class identified_worker {
 public:
   identified_worker() {
      auto pattern = (std::filesystem::temp_directory_path() / "forge-contract-expression-XXXXXX").string();
      const auto* directory = ::mkdtemp(pattern.data());
      BOOST_REQUIRE(directory != nullptr);
      _directory = directory;
      helper = _directory / "worker";
      _identity = _directory / "pid";
      auto script = std::ofstream{helper};
      script << "#!/bin/sh\nprintf '%s\\n' \"$$\" > " << quoted(_identity.string()) << "\nexec "
             << quoted(FORGE_EXPRESSION_WORKER) << '\n';
      script.close();
      BOOST_REQUIRE(script.good());
      BOOST_REQUIRE_EQUAL(::chmod(helper.c_str(), 0700), 0);
   }

   ~identified_worker() {
      std::error_code ignored;
      std::filesystem::remove_all(_directory, ignored);
   }

   void suspend() const {
      auto input = std::ifstream{_identity};
      auto pid = pid_t{-1};
      input >> pid;
      BOOST_REQUIRE(pid > 1);
      BOOST_REQUIRE_EQUAL(::kill(pid, SIGSTOP), 0);
   }

   std::filesystem::path helper;

 private:
   static std::string quoted(std::string_view value) {
      auto result = std::string{"'"};
      for (const auto character : value) {
         if (character == '\'')
            result += "'\\''";
         else
            result += character;
      }
      return result + "'";
   }

   std::filesystem::path _directory;
   std::filesystem::path _identity;
};

} // namespace

BOOST_AUTO_TEST_CASE(optional_expression_capability_and_compute_profile_remain_separate) {
   auto host = testing::host{};
   BOOST_CHECK_THROW(invoke(host, 0U, {}), testing::exceptions::expression_unavailable);
   BOOST_TEST(!host.find_primary(7U, 7U, 17U, 1U).has_value());
   BOOST_TEST(forge::vm::wasm::execution::intrinsic_names.size() == 5U);
   for (const auto name : {"expression_compile_v1", "expression_import_v1", "expression_evaluate_v1"})
      BOOST_TEST(!forge::vm::wasm::execution::allows_intrinsic("env", name));
}

BOOST_AUTO_TEST_CASE(invocation_precancel_and_expired_deadline_admit_no_effects) {
   auto host = testing::host{};
   auto stop = std::stop_source{};
   stop.request_stop();
   BOOST_CHECK_THROW(invoke(host, 0U, {}, {.cancel = stop.get_token()}), testing::exceptions::execution_cancelled);
   BOOST_CHECK_THROW(invoke(host, 0U, {}, {.deadline = std::chrono::steady_clock::now()}),
                     testing::exceptions::execution_timeout);
   BOOST_TEST(!host.find_table(7U, 7U, 17U).has_value());
}

BOOST_AUTO_TEST_CASE(raw_artifact_survives_runtime_restart_and_real_wasm_import_evaluation) {
   auto saved = forge::raw::bytes{};
   {
      auto runtime = expression::runtime{{.helper = FORGE_EXPRESSION_WORKER}};
      auto host = testing::host{runtime};
      const auto request = expression::compilation{"x + 1", {{"x", expression::type::int64}}};
      saved = invoke(host, 0U, forge::raw::pack(request));
      const auto artifact = expression::decode_artifact(saved);
      BOOST_TEST(forge::raw::pack(artifact) == saved, boost::test_tools::per_element());
      BOOST_TEST(host.find_primary(7U, 7U, 17U, 1U).has_value());
   }
   auto runtime = expression::runtime{{.helper = FORGE_EXPRESSION_WORKER}};
   auto importer = testing::host{runtime};
   BOOST_TEST(invoke(importer, 1U, saved) == saved, boost::test_tools::per_element());
   auto evaluator = testing::host{runtime};
   const auto request = expression::calculation{expression::decode_artifact(saved), {{"x", std::int64_t{41}}}};
   const auto bytes = invoke(evaluator, 2U, forge::raw::pack(request));
   const auto result = forge::raw::unpack_exact<expression::result>(bytes);
   BOOST_TEST(std::get<std::int64_t>(result.value) == 42);
   BOOST_TEST(result.ast_events > 0U);
   BOOST_TEST(forge::raw::pack(result) == bytes, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(malformed_inputs_and_forged_artifacts_rollback_database_effects) {
   auto runtime = expression::runtime{{.helper = FORGE_EXPRESSION_WORKER}};
   auto host = testing::host{runtime};
   auto invalid = expression::compilation{"x +", {{"x", expression::type::int64}}};
   BOOST_CHECK_THROW(invoke(host, 0U, forge::raw::pack(invalid)), expression::exceptions::invalid_program);
   auto trailing = forge::raw::pack(expression::compilation{"1"});
   trailing.push_back(0U);
   BOOST_CHECK_THROW(invoke(host, 0U, trailing), expression::exceptions::invalid_input);
   auto forged = prepare(runtime);
   forged.digest.data()[0] ^= 1;
   BOOST_CHECK_THROW(invoke(host, 1U, forge::raw::pack(forged)), expression::exceptions::invalid_program);
   BOOST_CHECK_EXCEPTION(invoke(host, 3U, {}), forge::exceptions::base, [](const auto& error) {
      return error.code() == forge::vm::wasm::interpret::exceptions::make_error_code(
                                 forge::vm::wasm::interpret::exceptions::code::memory);
   });
   BOOST_TEST(!host.find_table(7U, 7U, 17U).has_value());
   const auto valid = invoke(host, 0U, forge::raw::pack(expression::compilation{"1"}));
   BOOST_TEST(!valid.empty());
}

BOOST_AUTO_TEST_CASE(evaluation_errors_and_duplicate_inputs_preserve_typed_failure_and_rollback) {
   auto runtime = expression::runtime{{.helper = FORGE_EXPRESSION_WORKER}};
   auto host = testing::host{runtime};
   auto request = expression::calculation{prepare(runtime, "10 / x"), {{"x", std::int64_t{0}}}};
   BOOST_CHECK_THROW(invoke(host, 2U, forge::raw::pack(request)), expression::exceptions::evaluation_failed);
   request.inputs = {{"x", std::int64_t{1}}, {"x", std::int64_t{2}}};
   BOOST_CHECK_THROW(invoke(host, 2U, forge::raw::pack(request)), expression::exceptions::invalid_input);
   BOOST_TEST(!host.find_table(7U, 7U, 17U).has_value());
}

BOOST_AUTO_TEST_CASE(activation_conversion_preserves_all_admitted_collection_and_scalar_types) {
   const auto inputs = expression::arguments{{"signed", std::int64_t{-1}},
                                             {"unsigned", std::uint64_t{2}},
                                             {"flag", true},
                                             {"text", std::string{"value"}},
                                             {"signed_list", std::vector<std::int64_t>{-3, 4}},
                                             {"unsigned_list", std::vector<std::uint64_t>{5, 6}},
                                             {"signed_map", std::map<std::string, std::int64_t>{{"a", -7}}},
                                             {"unsigned_map", std::map<std::string, std::uint64_t>{{"b", 8}}}};
   const auto values = forge::core::conversion<forge::variant_object>{inputs}.finish();
   BOOST_TEST(values["signed"].is_int64());
   BOOST_TEST(values["signed"].as_int64() == -1);
   BOOST_TEST(values["unsigned"].is_uint64());
   BOOST_TEST(values["flag"].as_bool());
   BOOST_TEST(values["text"].as_string() == "value");
   BOOST_TEST(values["signed_list"].get_array()[0].as_int64() == -3);
   BOOST_TEST(values["unsigned_list"].get_array()[1].is_uint64());
   BOOST_TEST(values["signed_map"].get_object()["a"].as_int64() == -7);
   BOOST_TEST(values["unsigned_map"].get_object()["b"].is_uint64());
}

BOOST_AUTO_TEST_CASE(host_deadline_bounds_a_stalled_native_import) {
   auto fixture = identified_worker{};
   auto runtime = expression::runtime{{.helper = fixture.helper, .timeout = 5s}};
   fixture.suspend();
   auto host = testing::host{runtime, {.timeout = 100ms}};
   const auto started = std::chrono::steady_clock::now();
   BOOST_CHECK_THROW(invoke(host, 0U, forge::raw::pack(expression::compilation{"1"})),
                     expression::exceptions::deadline_exceeded);
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
   BOOST_CHECK_LT(elapsed, 2000);
   BOOST_TEST(!host.find_table(7U, 7U, 17U).has_value());
}

BOOST_AUTO_TEST_CASE(host_cancellation_reaches_a_stalled_native_import_and_joins_cleanup) {
   auto fixture = identified_worker{};
   auto runtime = expression::runtime{{.helper = fixture.helper, .timeout = 5s}};
   fixture.suspend();
   auto host = testing::host{runtime};
   auto stop = std::stop_source{};
   const auto started = std::chrono::steady_clock::now();
   auto cancel = std::jthread{[&] {
      std::this_thread::sleep_for(100ms);
      stop.request_stop();
   }};
   BOOST_CHECK_THROW(invoke(host, 0U, forge::raw::pack(expression::compilation{"1"}), {.cancel = stop.get_token()}),
                     expression::exceptions::cancelled);
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
   BOOST_CHECK_LT(elapsed, 2000);
   BOOST_TEST(!host.find_table(7U, 7U, 17U).has_value());
}
