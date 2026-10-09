#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

import forge.contract;
import forge.contract.binary_extension;
import forge.contract.varint;

using str = std::string;
using my_account = forge::chain::protocol::account_name;

struct result_value {
   std::int32_t count = 0;
   float ratio = 0;
   forge::chain::protocol::name owner;
};

struct base_value {
   std::int32_t value = 0;
};

using base_alias = base_value;

struct derived_value : base_alias {};

template <std::uint64_t Tag> struct tagged_number {
   std::uint64_t value = 0;
};

struct indexed_record {
   std::uint64_t id = 0;
};

template <std::uint64_t Name, typename Value> struct multi_index {
   Value value{};
};

template <std::uint64_t Name, typename Value> struct singleton {
   Value value{};
};

struct [[forge::table("owned"), forge::contract("abifixture")]] owned_record {
   std::uint64_t id = 0;
};

struct [[forge::table("foreign"), forge::contract("otherfixture")]] foreign_record {
   std::uint64_t id = 0;
};

namespace dispatcher_shadow {

class [[forge::contract("abifixture")]] code : public forge::contract::context {
 public:
   using context::context;

   using index_fixture = multi_index<8417982951132397568ULL, indexed_record>;
   using singleton_fixture = singleton<14098176321150517248ULL, indexed_record>;

   struct [[forge::table("records")]] record {
      std::uint64_t id = 0;
      std::array<std::int32_t, 32> values{};
   };

   struct [[forge::table]] defaultrec {
      std::uint64_t id = 0;
   };

   struct [[forge::table("varints")]] varint_record {
      std::uint64_t id = 0;
      forge::unsigned_int unsigned_value;
      forge::signed_int signed_value;
   };

   [[forge::action]] void alias(std::variant<std::uint64_t, str> arg0) {
      static_cast<void>(arg0);
   }
   [[forge::action]] void named(my_account owner) {
      static_cast<void>(owner);
   }
   [[forge::action]] void nested(std::map<std::string, std::map<std::string, std::string>> arg0,
                                 const std::tuple<std::int32_t, double, std::string, std::vector<std::int32_t>>& arg1,
                                 std::optional<std::uint32_t> arg2) {
      static_cast<void>(arg0);
      static_cast<void>(arg1);
      static_cast<void>(arg2);
   }
   [[forge::action]] void inherited(derived_value arg0) {
      static_cast<void>(arg0);
   }
   [[forge::action]] void tagged(tagged_number<3472950412842106880ULL> arg0) {
      static_cast<void>(arg0);
   }
   [[forge::action]] result_value result() {
      return {};
   }
   [[forge::action]] void varintargs(forge::contract::unsigned_int unsigned_value,
                                     forge::contract::signed_int signed_value) {
      static_cast<void>(unsigned_value);
      static_cast<void>(signed_value);
   }
   [[forge::action]] void extension(forge::contract::binary_extension<std::uint32_t> value) {
      static_cast<void>(value);
   }
   [[forge::action]] void extrewards(forge::chain::protocol::extended_symbol rewards) {
      static_cast<void>(rewards);
   }

   [[forge::call]] std::uint32_t sum(std::uint32_t a, std::uint32_t b) {
      return a + b;
   }
};

} // namespace dispatcher_shadow
