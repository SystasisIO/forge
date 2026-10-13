#define BOOST_TEST_MODULE forge_contract_protocol_value_tests
#include <boost/test/included/unit_test.hpp>
#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

import forge.chain.protocol.types;
import forge.raw.codec;
import forge.variant.value;

namespace protocol = forge::chain::protocol;
static_assert(protocol::symbol_code{}.raw() == 0);
static_assert(!std::convertible_to<std::uint64_t, protocol::symbol_code>);
static_assert(sizeof(protocol::symbol_code) == sizeof(std::uint64_t));

BOOST_AUTO_TEST_CASE(canonical_asset_formatter_preserves_text_and_wire) {
   constexpr auto amounts = std::array<std::int64_t, 5>{0, 42, -42, protocol::asset::max_amount,
                                                       -protocol::asset::max_amount};
   const auto expected = std::array<std::array<std::string, 5>, 4>{
      std::array<std::string, 5>{"0", "42", "-42", "4611686018427387903", "-4611686018427387903"},
      std::array<std::string, 5>{"0.0000", "0.0042", "-0.0042", "461168601842738.7903", "-461168601842738.7903"},
      std::array<std::string, 5>{"0.000000000000000000", "0.000000000000000042", "-0.000000000000000042",
                                "4.611686018427387903", "-4.611686018427387903"},
      std::array<std::string, 5>{"0." + std::string(255, '0'), "0." + std::string(253, '0') + "42",
                                "-0." + std::string(253, '0') + "42",
                                "0." + std::string(236, '0') + "4611686018427387903",
                                "-0." + std::string(236, '0') + "4611686018427387903"}};
   constexpr auto precisions = std::array<std::uint8_t, 4>{0, 4, 18, 255};
   for (auto p = std::size_t{}; p < precisions.size(); ++p) {
      for (auto a = std::size_t{}; a < amounts.size(); ++a) {
         const auto value = protocol::asset{amounts[a], protocol::symbol{"ABCDEFG", precisions[p]}};
         const auto text = expected[p][a] + " ABCDEFG";
         BOOST_TEST(protocol::to_string(value) == text);
         auto variant = forge::variant{};
         protocol::to_variant(value, variant);
         BOOST_TEST(variant.as_string() == text);
         auto decoded = protocol::asset{};
         protocol::from_variant(variant, decoded);
         BOOST_TEST(decoded.amount == value.amount);
         BOOST_TEST(decoded.sym.raw() == value.sym.raw());

         auto raw = std::vector<std::uint8_t>{};
         for (auto i = 0U; i < 8U; ++i)
            raw.push_back(static_cast<std::uint8_t>(static_cast<std::uint64_t>(amounts[a]) >> (8U * i)));
         raw.push_back(precisions[p]);
         for (const auto letter : std::string{"ABCDEFG"}) raw.push_back(static_cast<std::uint8_t>(letter));
         BOOST_CHECK(forge::raw::pack(value) == raw);
      }
   }
   // Formatting a directly mutated out-of-range amount remains magnitude-safe;
   // constructors and arithmetic still enforce the original asset bounds.
   auto minimum = protocol::asset{};
   minimum.amount = std::numeric_limits<std::int64_t>::min();
   minimum.sym = protocol::symbol{"ABCDEFG", 0};
   BOOST_TEST(protocol::to_string(minimum) == "-9223372036854775808 ABCDEFG");
   BOOST_CHECK_THROW((protocol::asset{minimum.amount, minimum.sym}), std::invalid_argument);
}
