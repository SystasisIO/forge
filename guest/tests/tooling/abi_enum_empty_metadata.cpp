#include <cstdint>

import forge.contract;

enum class flag : std::uint8_t {};

class [[forge::contract("emptyenum")]] emptyenum : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(flag value) { static_cast<void>(value); }
};
