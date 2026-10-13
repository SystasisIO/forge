#if !defined(FORGE_CONTRACT_GUEST)
#error "contract analysis and compilation must use the guest translation unit"
#endif

#include <cstdint>
#include <string>
#include <string_view>

import forge.contract;

static_assert(CONSUMER_DECLARATION_VALUE == 42);
static_assert(std::string_view{CONSUMER_DECLARATION_TEXT} == "quoted \"value\" with space");

class [[forge::contract("guestmacro")]] guestmacro : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] void run(std::uint64_t value) {
      forge::contract::check(value != 0, "value must not be zero");
   }

   // The same definitions must reach both ABI analysis and dispatcher compilation.
#if CONSUMER_DECLARATION_VALUE == 42
   [[forge::action]] std::string declared() {
      return CONSUMER_DECLARATION_TEXT;
   }
#endif
};
