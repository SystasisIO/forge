#include <cstdint>
import forge.contract;

namespace dispatcher_shadow {
class [[forge::contract("dispatchshdw")]] code : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] std::uint32_t action(std::uint32_t value) { return value + 1; }
   [[forge::call]] std::uint32_t call(std::uint32_t value) { return value + 2; }
   [[forge::on_notify("eosio.token::transfer")]] std::uint32_t transfer(std::uint32_t value) {
      return value + 3;
   }
};
}
