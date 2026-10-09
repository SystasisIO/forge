#define BOOST_TEST_MODULE forge_contract_donor_compatibility_tests
#include <boost/test/included/unit_test.hpp>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

import forge.chain.protocol.authority;
import forge.chain.protocol.types;
import forge.raw.codec;
import forge.variant.value;
import forge.vm.wasm.interpret.backend;

namespace {
namespace protocol = forge::chain::protocol;
namespace wasm = forge::vm::wasm::interpret;

struct contract_failure : std::runtime_error { using std::runtime_error::runtime_error; };
struct contract_exit { std::int32_t code; };

struct invocation {
   std::vector<std::uint8_t> arguments;
   std::vector<std::uint8_t> result;
   std::string console;
   void eosio_assert_message(std::uint32_t success, wasm::span<const char> message) {
      if (!success) throw contract_failure{std::string{message.data(), message.size()}};
   }
   [[noreturn]] void eosio_exit(std::int32_t code) { throw contract_exit{code}; }
   std::uint32_t action_data_size() const { return static_cast<std::uint32_t>(arguments.size()); }
   std::uint32_t read_action_data(wasm::span<char> output) {
      const auto size = std::min(output.size(), arguments.size());
      std::copy_n(arguments.begin(), size, output.begin());
      return static_cast<std::uint32_t>(size);
   }
   void set_action_return_value(wasm::span<const char> input) {
      result.assign(reinterpret_cast<const std::uint8_t*>(input.data()),
                    reinterpret_cast<const std::uint8_t*>(input.data()) + input.size());
   }
   void prints_l(wasm::span<const char> value) { console.append(value.data(), value.size()); }
};
using functions = wasm::registered_host_functions<invocation>;

void register_intrinsics() {
   static const auto once = [] {
      functions::add<&invocation::eosio_assert_message>("env", "eosio_assert_message");
      functions::add<&invocation::eosio_exit>("env", "eosio_exit");
      functions::add<&invocation::action_data_size>("env", "action_data_size");
      functions::add<&invocation::read_action_data>("env", "read_action_data");
      functions::add<&invocation::set_action_return_value>("env", "set_action_return_value");
      functions::add<&invocation::prints_l>("env", "prints_l");
      return true;
   }();
   static_cast<void>(once);
}

wasm::wasm_code read_code() {
   auto input = std::ifstream{FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM, std::ios::binary | std::ios::ate};
   if (!input) throw std::runtime_error{"missing donor compatibility fixture"};
   auto code = wasm::wasm_code(static_cast<std::size_t>(input.tellg()));
   input.seekg(0);
   input.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size()));
   return code;
}

struct allocator {
   wasm::wasm_allocator value;
   ~allocator() { value.free(); }
};

template <typename Implementation> void reusable_invocation_checks() {
   register_intrinsics();
   auto code = read_code();
   auto memory = allocator{};
   auto host = invocation{};
   auto vm = wasm::backend<functions, Implementation, wasm::compatibility_options>{code, host, &memory.value};
   BOOST_REQUIRE_EQUAL(vm.get_module().memories.size(), 1U);
   const auto& limits = vm.get_module().memories[0].limits;
   BOOST_TEST(limits.flags);
   BOOST_TEST(limits.initial == 2U);
   BOOST_TEST(limits.maximum == 256U);
   BOOST_TEST(limits.maximum <= wasm::compatibility_options::max_pages);
   const auto account = protocol::make_name("donorcompat").value;
   auto run = [&](std::string_view action, std::vector<std::uint8_t> arguments = {}) {
      host.arguments = std::move(arguments);
      host.result.clear();
      host.console.clear();
      vm.initialize(host);
      BOOST_TEST_CHECKPOINT("action " << action);
      vm(host, "env", "apply", account, account, protocol::make_name(action).value);
      return host.result;
   };

   for (auto repeat = 0; repeat < 3; ++repeat) {
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("lifetime")) == 1U);
      BOOST_CHECK_THROW(run("trap"), contract_failure);
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("lifetime")) == 1U);
      BOOST_CHECK_EXCEPTION(run("eosioexit"), contract_exit, [](const contract_exit& exit) { return exit.code == 7; });
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("lifetime")) == 1U);
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("registry")) == 40U);
      BOOST_CHECK_EXCEPTION(run("cexit"), contract_exit, [](const contract_exit& exit) { return exit.code == 9; });
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("lifetime")) == 1U);
   }
   const auto heap_address = forge::raw::unpack_exact<std::uint32_t>(run("heap"));
   for (auto repeat = 0; repeat < 64; ++repeat)
      BOOST_TEST(forge::raw::unpack_exact<std::uint32_t>(run("heap")) == heap_address);

   BOOST_TEST(forge::raw::unpack_exact<bool>(run("cstdlib")));
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("numfacets")));
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("printfcheck")));
   BOOST_TEST(host.console == std::string{"printf:1.5|vprintf:1.5|vfprintf:1.5|stdout:1.5|"} + std::string(514, 'x'));
   BOOST_CHECK_THROW(run("trap"), contract_failure);
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("printfcheck")));
   BOOST_CHECK_EXCEPTION(run("eosioexit"), contract_exit, [](const contract_exit& exit) { return exit.code == 7; });
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("numfacets")));
   BOOST_CHECK_THROW(run("badlocale"), contract_failure);
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("cstdlib")));
   BOOST_CHECK_EXCEPTION(run("timezone"), contract_failure, [](const contract_failure& failure) {
      return std::string_view{failure.what()} == "strftime %Z not supported.";
   });
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("cstdlib")));

   const auto valid = std::string{"Qm"} + std::string(44, 'A');
   for (const auto& value : std::vector<std::string>{
           valid, "", valid.substr(0, 45), valid + "A", std::string{"Qm"} + std::string(44, '0'),
           std::string{"Qm"} + std::string(43, 'A') + "!", std::string{"Xm"} + std::string(44, 'A')}) {
      BOOST_TEST(forge::raw::unpack_exact<bool>(run("ipfs", forge::raw::pack(value))) == (value == valid));
   }
   BOOST_TEST(forge::raw::unpack_exact<bool>(run("ipfs", forge::raw::pack(valid))));

   const auto wrapped = forge::raw::unpack_exact<std::vector<std::uint8_t>>(run("wrapcheck"));
   const auto owner = protocol::authority{.threshold = 3,
      .accounts = {{{protocol::name{"bob"}, protocol::name{"active"}}, 5}}, .waits = {{17, 7}}};
   const auto active = protocol::authority{.threshold = 11, .waits = {{23, 13}}};
   const auto expected = forge::raw::pack(std::tuple{protocol::name{"eosio"}, protocol::name{"alice"}, owner, active});
   BOOST_CHECK(wrapped == expected);

   const auto rewards = protocol::extended_symbol{protocol::symbol{"EOS", 4}, protocol::name{"eosio.token"}};
   const auto extended = forge::raw::pack(rewards);
   BOOST_CHECK(forge::raw::unpack_exact<std::vector<std::uint8_t>>(run("extwire", extended)) == extended);

   const auto formatted = forge::raw::unpack_exact<std::vector<std::string>>(run("formatting"));
   BOOST_REQUIRE_EQUAL(formatted.size(), 20U);
   auto index = std::size_t{};
   for (const auto precision : {0U, 4U, 18U, 255U}) {
      for (const auto amount : {std::int64_t{0}, std::int64_t{42}, std::int64_t{-42},
                                protocol::asset::max_amount, -protocol::asset::max_amount}) {
         const auto value = protocol::asset{amount, protocol::symbol{"ABCDEFG", static_cast<std::uint8_t>(precision)}};
         auto variant = forge::variant{};
         protocol::to_variant(value, variant);
         BOOST_TEST(formatted[index++] == variant.as_string());
      }
   }
}
} // namespace

BOOST_AUTO_TEST_CASE(donor_compatibility_interpreter_reset_and_wire) {
   reusable_invocation_checks<wasm::interpreter>();
}

#if FORGE_VM_WASM_INTERPRET_HAS_JIT
BOOST_AUTO_TEST_CASE(donor_compatibility_cached_jit_reset_and_wire) {
   reusable_invocation_checks<wasm::jit>();
}
#endif
