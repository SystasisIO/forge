#include <boost/test/unit_test.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include "../vm_wasm_execution/fixtures.hxx"

import forge.tooling.validation.validator;

namespace {
namespace validation = forge::tooling::validation;

fixtures::bytes contract(unsigned parameters = 3, bool memory_export = false, bool trapped_start = false,
                         bool memory_import = false, bool argument = false) {
   auto wasm = fixtures::bytes{0, 0x61, 0x73, 0x6d, 1, 0, 0, 0};
   auto types = fixtures::bytes{3, 0x60, static_cast<std::uint8_t>(argument ? 1 : 0)};
   if (argument) types.push_back(0x7f);
   types.insert(types.end(), {1, 0x7f, 0x60});
   fixtures::size(types, parameters);
   types.insert(types.end(), parameters, 0x7e);
   types.insert(types.end(), {0, 0x60, 0, 0});
   fixtures::section(wasm, 1, types);
   auto imports = fixtures::bytes{static_cast<std::uint8_t>(memory_import ? 2 : 1)};
   fixtures::text(imports, "env");
   fixtures::text(imports, "answer");
   imports.insert(imports.end(), {0, 0});
   if (memory_import) {
      fixtures::text(imports, "env");
      fixtures::text(imports, "memory");
      imports.insert(imports.end(), {2, 0, 1});
   }
   fixtures::section(wasm, 2, imports);
   fixtures::section(wasm, 3, trapped_start ? fixtures::bytes{2, 1, 2} : fixtures::bytes{1, 1});
   if (memory_export) fixtures::section(wasm, 5, {1, 0, 1});
   auto exports = fixtures::bytes{1};
   fixtures::text(exports, "apply");
   exports.insert(exports.end(), {static_cast<std::uint8_t>(memory_export ? 2 : 0),
                                  static_cast<std::uint8_t>(memory_export ? 0 : 1)});
   fixtures::section(wasm, 7, exports);
   if (trapped_start) fixtures::section(wasm, 8, {2});
   auto instructions = fixtures::bytes{0};
   if (argument) instructions.insert(instructions.end(), {0x41, 0});
   instructions.insert(instructions.end(), {0x10, 0, 0x1a, 0x0b});
   auto body = fixtures::bytes{static_cast<std::uint8_t>(trapped_start ? 2 : 1)};
   fixtures::size(body, instructions.size());
   body.insert(body.end(), instructions.begin(), instructions.end());
   if (trapped_start) body.insert(body.end(), {3, 0, 0x00, 0x0b});
   fixtures::section(wasm, 10, body);
   return wasm;
}

struct files {
   std::filesystem::path directory;
   validation::request request;

   files() {
      const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
      for (unsigned attempt = 0; attempt != 32; ++attempt) {
         auto candidate = std::filesystem::temp_directory_path() /
            ("forge-validation-" + std::to_string(stamp) + '-' + std::to_string(attempt));
         if (std::filesystem::create_directory(candidate)) { directory = std::move(candidate); break; }
      }
      if (directory.empty()) throw std::runtime_error{"Cannot create validation fixture directory"};
      request = {directory / "contract.wasm", directory / "contract.abi", directory / "imports.json"};
      text(request.abi, R"({"version":"eosio::abi/1.2","types":[],"structs":[],"actions":[],"tables":[],"ricardian_clauses":[],"error_messages":[],"abi_extensions":[],"variants":[],"action_results":[]})");
      manifest();
      wasm(contract());
   }
   ~files() {
      auto error = std::error_code{};
      std::filesystem::remove_all(directory, error);
   }
   static void text(const std::filesystem::path& path, std::string_view value) {
      auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
      output.write(value.data(), value.size());
      if (!output) throw std::runtime_error{"Cannot write validation fixture"};
   }
   void wasm(const fixtures::bytes& value) const {
      text(request.wasm, {reinterpret_cast<const char*>(value.data()), value.size()});
   }
   void manifest(std::string_view parameters = "[]", std::string_view result = "\"i32\"") const {
      text(request.imports,
         std::string{R"({"schema_version":1,"interface_version":1,"imports":[{"module":"env","import":"answer","wasm_parameters":)"} +
         std::string{parameters} + R"(,"wasm_result":)" + std::string{result} + "}]}");
   }
};
}

BOOST_AUTO_TEST_SUITE(contract_validation)

BOOST_AUTO_TEST_CASE(validates_import_and_apply_signatures) {
   auto input = files{};
   BOOST_CHECK_NO_THROW(validation::validate(input.request));
}

BOOST_AUTO_TEST_CASE(validation_does_not_execute_start) {
   auto input = files{};
   input.wasm(contract(3, false, true));
   BOOST_CHECK_NO_THROW(validation::validate(input.request));
}

BOOST_AUTO_TEST_CASE(rejects_wrong_import_parameter_and_result_types) {
   auto input = files{};
   input.manifest("[\"i32\"]");
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
   input.manifest("[]", "\"f32\"");
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
   input.manifest();
   BOOST_CHECK_NO_THROW(validation::validate(input.request));
   input.wasm(contract(3, false, false, false, true));
   input.manifest("[\"f32\"]");
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
   input.manifest("[\"i32\"]");
   BOOST_CHECK_NO_THROW(validation::validate(input.request));
}

BOOST_AUTO_TEST_CASE(rejects_missing_and_nonfunction_required_exports) {
   auto input = files{};
   input.request.required_export = "absent";
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
   input.request.required_export = "apply";
   input.wasm(contract(3, true));
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rejects_wrong_apply_signature) {
   auto input = files{};
   input.wasm(contract(2));
   BOOST_CHECK_THROW(validation::validate(input.request), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(rejects_nonfunction_imports) {
   auto input = files{};
   input.wasm(contract(3, false, false, true));
   BOOST_CHECK_THROW(validation::validate(input.request), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()
