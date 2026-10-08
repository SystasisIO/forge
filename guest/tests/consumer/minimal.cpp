#if defined(CONSUMER_DECLARATION_VALUE) || defined(CONSUMER_DECLARATION_TEXT)
#error "contract declaration definitions must not leak into a neighboring target"
#endif

import forge.contract;

class [[forge::contract("minimal")]] minimal : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] void ping(unsigned long long value) {
      static_cast<void>(value);
   }
};
