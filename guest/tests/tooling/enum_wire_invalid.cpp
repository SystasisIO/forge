import forge.contract;
import forge.tests.tooling.enum_wire.invalid;

// Run each case both with and without --metadata; range rejection must not depend on that output.
// Each selected specialization is imported and no pack/unpack instantiation forces its discovery.
#ifndef FORGE_TEST_ENUM_WIRE_FAILURE
#define FORGE_TEST_ENUM_WIRE_FAILURE 1
#endif

namespace fixture = forge::tests::tooling::enum_wire;

#if FORGE_TEST_ENUM_WIRE_FAILURE == 1
using rejected = fixture::bad_flag;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 2
using rejected = fixture::mutable_flag;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 3
using rejected = fixture::missing_type;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 4
using rejected = fixture::non_type_alias;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 5
using rejected = fixture::floating_type;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 6
using rejected = fixture::boolean_type;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 7
using rejected = fixture::wide_type;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 8
using rejected = fixture::signed_overflow;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 9
using rejected = fixture::signed_underflow;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 10
using rejected = fixture::unsigned_negative;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 11
using rejected = fixture::unsigned_to_signed64;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 12
using rejected = fixture::platform_long;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 13
using rejected = fixture::platform_unsigned_long;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 14
using rejected = fixture::platform_size;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 15
using rejected = fixture::char_underlying;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 16
using rejected = fixture::private_shared_base;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 17
using rejected = fixture::protected_shared_base;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 18
using rejected = fixture::private_type_base;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 19
using rejected = fixture::protected_type_base;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 20
using rejected = fixture::private_shared_using;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 21
using rejected = fixture::protected_shared_using;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 22
using rejected = fixture::private_type_using;
#elif FORGE_TEST_ENUM_WIRE_FAILURE == 23
using rejected = fixture::protected_type_using;
#else
#error Unsupported FORGE_TEST_ENUM_WIRE_FAILURE case
#endif

class [[forge::contract("badwire")]] badwire : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(rejected value) {
      static_cast<void>(value);
   }
};
