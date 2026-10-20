#define BOOST_TEST_MODULE forge_contract_corpus_tests

#include <boost/test/included/unit_test.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import forge.chain.protocol.transaction;
import forge.chain.protocol.values;
import forge.tooling.testing.host;
import forge.crypto.digest.sha256;
import forge.exceptions;
import forge.raw.codec;
import forge.vm.wasm.interpret.backend;

namespace {

namespace protocol = forge::chain::protocol;
namespace testing = forge::tooling::testing;
namespace wasm = forge::vm::wasm::interpret;

const auto source_root = std::filesystem::path{FORGE_CONTRACT_CORPUS_SOURCE_DIR};
const auto build_root = std::filesystem::path{FORGE_CONTRACT_CORPUS_BUILD_DIR};

struct contract_fixture {
   std::string target;
   std::filesystem::path donor;
};

const auto fixtures = std::vector<contract_fixture>{
    {"spring_eosio_boot", source_root / "spring/contracts/eosio.boot/eosio.boot.wasm"},
    {"spring_eosio_token", source_root / "spring/contracts/eosio.token/eosio.token.wasm"},
    {"spring_eosio_msig", source_root / "spring/contracts/eosio.msig/eosio.msig.wasm"},
    {"spring_eosio_wrap", source_root / "spring/contracts/eosio.wrap/eosio.wrap.wasm"},
    {"spring_eosio_system", source_root / "spring/contracts/eosio.system/eosio.system.wasm"},
    {"spring_test_api", source_root / "spring/test-contracts/test_api/test_api.wasm"},
    {"spring_test_api_db", source_root / "spring/test-contracts/test_api_db/test_api_db.wasm"},
    {"spring_test_api_multi_index",
     source_root / "spring/test-contracts/test_api_multi_index/test_api_multi_index.wasm"},
    {"eosio_bios", source_root / "eosio/contracts/eosio.bios/bin/eosio.bios.wasm"},
    {"eosio_boot", source_root / "eosio/contracts/eosio.boot/bin/eosio.boot.wasm"},
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
   auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
   if (!input) {
      throw std::runtime_error{"cannot open contract fixture: " + path.string()};
   }
   const auto size = input.tellg();
   if (size < 0) {
      throw std::runtime_error{"cannot determine contract fixture size: " + path.string()};
   }
   auto result = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
   input.seekg(0);
   if (!result.empty() && !input.read(reinterpret_cast<char*>(result.data()), size)) {
      throw std::runtime_error{"cannot read contract fixture: " + path.string()};
   }
   return result;
}

std::string import_text(const std::vector<std::uint8_t>& value) {
   return {reinterpret_cast<const char*>(value.data()), value.size()};
}

void validate_contract(const std::filesystem::path& path) {
   const auto bytes = read_bytes(path);
   auto code = wasm::wasm_code{bytes.begin(), bytes.end()};
   using validator = wasm::backend<std::nullptr_t, wasm::null_backend, wasm::compatibility_options>;
   auto parsed = validator{code, static_cast<wasm::wasm_allocator*>(nullptr)};
   auto& module = parsed.get_module();
   BOOST_TEST(module.get_exported_function("apply") != std::numeric_limits<std::uint32_t>::max());
   for (std::uint32_t index = 0; index < module.imports.size(); ++index) {
      BOOST_TEST(module.imports[index].kind == wasm::external_kind::Function);
      BOOST_TEST(import_text(module.imports[index].module_str) == "env");
   }
}

std::vector<std::uint8_t> default_blockchain_parameters() {
   return forge::raw::pack(std::uint64_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{},
                           std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{},
                           std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{}, std::uint32_t{},
                           std::uint16_t{}, std::uint16_t{});
}

testing::oracle_state initial_state(std::uint64_t receiver) {
   const auto eosio = protocol::make_name("eosio").value;
   const auto token = protocol::make_name("eosio.token").value;
   const auto msig = protocol::make_name("eosio.msig").value;
   const auto wrap = protocol::make_name("eosio.wrap").value;
   const auto testapi = protocol::make_name("testapi").value;
   const auto alice = protocol::make_name("alice").value;
   const auto bob = protocol::make_name("bob").value;
   auto state = testing::oracle_state{};
   state.accounts = {receiver, eosio, token, msig, wrap, testapi, alice, bob};
   state.authorized_accounts = state.accounts;
   state.privileged_accounts.insert(receiver);
   state.current_time = 1'000'000;
   state.publication_time = 1'000'000;
   state.block_num = 42;
   state.blockchain_parameters = default_blockchain_parameters();
   return state;
}

struct outcome {
   bool success = false;
   testing::invocation_result result;
   testing::oracle_state state;
   std::vector<std::uint8_t> database;
   std::string error;
};

outcome invoke(const std::filesystem::path& code_path, std::uint64_t receiver, std::uint64_t action,
               const std::vector<std::uint8_t>& data, testing::oracle_state state) {
   auto host = testing::host{};
   host.configure(std::move(state));
   auto result = outcome{};
   const auto code = read_bytes(code_path);
   try {
      result.result = host.invoke(code, receiver, receiver, action, data);
      result.success = true;
   } catch (const forge::exceptions::base& error) {
      result.error = error.message();
   } catch (const std::exception& error) {
      result.error = error.what();
   }
   result.state = host.state();
   result.database = host.snapshot();
   return result;
}

void compare_action(const contract_fixture& fixture, std::string_view receiver_name, std::uint64_t action,
                    std::vector<std::uint8_t> data, bool expected_success = true,
                    std::optional<testing::oracle_state> configured = std::nullopt) {
   const auto receiver = protocol::make_name(receiver_name).value;
   const auto state = configured.value_or(initial_state(receiver));
   const auto donor = invoke(fixture.donor, receiver, action, data, state);
   const auto current = invoke(build_root / (fixture.target + ".wasm"), receiver, action, data, state);

   BOOST_TEST_CONTEXT(fixture.target) {
      BOOST_TEST(donor.success == expected_success);
      BOOST_TEST(current.success == donor.success);
      BOOST_TEST(current.error == donor.error);
      BOOST_TEST(current.result.return_value == donor.result.return_value, boost::test_tools::per_element());
      BOOST_CHECK(current.result.exit_code == donor.result.exit_code);
      BOOST_CHECK(current.state == donor.state);
      BOOST_TEST(current.database == donor.database, boost::test_tools::per_element());
   }
}

constexpr std::uint32_t djbh(std::string_view value) {
   auto result = std::uint32_t{5381};
   for (const auto byte : value) {
      result = result * 33U ^ static_cast<unsigned char>(byte);
   }
   return result;
}

constexpr std::uint64_t test_action(std::string_view type, std::string_view method) {
   return static_cast<std::uint64_t>(djbh(type)) << 32U | djbh(method);
}

const contract_fixture& fixture(std::string_view target) {
   const auto found = std::ranges::find(fixtures, target, &contract_fixture::target);
   if (found == fixtures.end()) {
      throw std::runtime_error{"unknown corpus target"};
   }
   return *found;
}

} // namespace

BOOST_AUTO_TEST_CASE(all_donor_and_forge_wasm_modules_validate_and_export_apply) {
   for (const auto& entry : fixtures) {
      BOOST_TEST_CONTEXT(entry.target + " donor") {
         BOOST_CHECK_NO_THROW(validate_contract(entry.donor));
      }
      BOOST_TEST_CONTEXT(entry.target + " forge") {
         BOOST_CHECK_NO_THROW(validate_contract(build_root / (entry.target + ".wasm")));
      }
   }
}

BOOST_AUTO_TEST_CASE(spring_boot_matches_donor_feature_activation) {
   const auto digest = forge::crypto::digest::sha256::hash("spring-boot", 11U);
   compare_action(fixture("spring_eosio_boot"), "eosio", protocol::make_name("activate").value,
                  forge::raw::pack(digest));
}

BOOST_AUTO_TEST_CASE(spring_token_matches_donor_create_state) {
   const auto receiver = protocol::make_name("eosio.token").value;
   const auto symbol = protocol::make_symbol("SYS", 4);
   compare_action(fixture("spring_eosio_token"), "eosio.token", protocol::make_name("create").value,
                  forge::raw::pack(protocol::make_name("alice"), protocol::asset{1'000'000, symbol}), true,
                  initial_state(receiver));
}

BOOST_AUTO_TEST_CASE(spring_msig_matches_donor_missing_proposal_failure) {
   compare_action(
       fixture("spring_eosio_msig"), "eosio.msig", protocol::make_name("cancel").value,
       forge::raw::pack(protocol::make_name("alice"), protocol::make_name("proposal"), protocol::make_name("alice")),
       false);
}

BOOST_AUTO_TEST_CASE(spring_wrap_matches_donor_deferred_queue) {
   auto transaction = protocol::transaction{};
   transaction.expiration = protocol::time_point_sec{2};
   compare_action(fixture("spring_eosio_wrap"), "eosio.wrap", protocol::make_name("exec").value,
                  forge::raw::pack(protocol::make_name("alice"), transaction));
}

BOOST_AUTO_TEST_CASE(spring_system_matches_donor_revision_state) {
   compare_action(fixture("spring_eosio_system"), "eosio", protocol::make_name("updtrevision").value,
                  forge::raw::pack(std::uint8_t{1}));
}

BOOST_AUTO_TEST_CASE(spring_test_api_matches_donor_types_scenario) {
   compare_action(fixture("spring_test_api"), "testapi", test_action("test_types", "types_size"), {});
}

BOOST_AUTO_TEST_CASE(spring_database_intrinsics_match_donor_state) {
   compare_action(fixture("spring_test_api_db"), "testapi", protocol::make_name("pg").value, {});
}

BOOST_AUTO_TEST_CASE(spring_multi_index_matches_donor_state) {
   compare_action(fixture("spring_test_api_multi_index"), "testapi", protocol::make_name("s1g").value, {});
}

BOOST_AUTO_TEST_CASE(legacy_eosio_bios_matches_donor_authorization) {
   compare_action(fixture("eosio_bios"), "eosio", protocol::make_name("reqauth").value,
                  forge::raw::pack(protocol::make_name("alice")));
}

BOOST_AUTO_TEST_CASE(legacy_eosio_boot_matches_donor_feature_activation) {
   const auto digest = forge::crypto::digest::sha256::hash("eosio-boot", 10U);
   compare_action(fixture("eosio_boot"), "eosio", protocol::make_name("activate").value, forge::raw::pack(digest));
}
