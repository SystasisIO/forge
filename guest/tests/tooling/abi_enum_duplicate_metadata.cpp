#include <cstdint>

import forge.contract;

enum class flag : std::uint8_t { first = 1, other = 1 };

class [[forge::contract("enumfixture")]] enumfixture : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(flag value) { static_cast<void>(value); }
};
