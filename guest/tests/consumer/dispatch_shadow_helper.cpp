#include <cstdint>
import forge.contract;

namespace dispatcher_shadow {
class [[forge::contract("dispatchshdw")]] receiver : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] std::uint32_t helper(std::uint32_t value) { return value + 4; }
};
}
