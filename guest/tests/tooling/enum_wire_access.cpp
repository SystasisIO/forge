#include <cstdint>
#include <type_traits>

import forge.contract;
import forge.tests.tooling.enum_wire.values;

// First exercise imported lookup without instantiation; then check Raw's public access semantics.
#ifndef FORGE_TEST_ENUM_WIRE_RAW_ACCESS
#define FORGE_TEST_ENUM_WIRE_RAW_ACCESS 0
#endif

#if FORGE_TEST_ENUM_WIRE_RAW_ACCESS
import forge.raw.enum_wire_type;

static_assert(forge::raw::shared_enum_wire_v<forge::tests::tooling::enum_wire::public_inherited>);
static_assert(forge::raw::shared_enum_wire_v<forge::tests::tooling::enum_wire::public_using_private>);
static_assert(forge::raw::shared_enum_wire_v<forge::tests::tooling::enum_wire::public_using_protected>);
static_assert(std::is_same_v<
              forge::raw::enum_wire_type_t<forge::tests::tooling::enum_wire::public_inherited>, std::int64_t>);
static_assert(std::is_same_v<
              forge::raw::enum_wire_type_t<forge::tests::tooling::enum_wire::public_using_private>, std::int64_t>);
static_assert(std::is_same_v<
              forge::raw::enum_wire_type_t<forge::tests::tooling::enum_wire::public_using_protected>, std::int64_t>);
#endif

class [[forge::contract("accesswire")]] accesswire : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void submit(forge::tests::tooling::enum_wire::access_payload value) {
      static_cast<void>(value);
   }
};
