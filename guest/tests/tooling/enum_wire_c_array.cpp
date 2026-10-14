#include <array>
#include <vector>

import forge.contract;
import forge.tests.tooling.enum_wire.values;

#ifndef FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE
#define FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE 1
#endif

using status = forge::tests::tooling::enum_wire::shared_status;
using status_array = status[2];
using status_array_alias = status_array;

struct array_holder {
#if FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE == 1
   status values[2];
#elif FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE == 2
   status_array_alias values;
#elif FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE == 3
   std::array<status_array_alias, 2> values;
#elif FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE == 4
   status_array_alias values;
#else
#error Unsupported FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE
#endif
};

#if FORGE_TEST_ENUM_WIRE_C_ARRAY_CASE == 4
struct input {
   std::vector<array_holder> values;
};
#else
using input = array_holder;
#endif

// All cases must reject count-prefixed Raw C-arrays instead of emitting fixed[N] ABI.
class [[forge::contract("carraywire")]] carraywire : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(input value) {
      static_cast<void>(value);
   }
};
