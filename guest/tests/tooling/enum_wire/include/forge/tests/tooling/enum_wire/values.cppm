module;

#include <array>
#include <cstdint>
#include <vector>

export module forge.tests.tooling.enum_wire.values;

import forge.raw.enum_wire_type;

export namespace forge::tests::tooling::enum_wire {

using signed_64_alias = std::int64_t;
using signed_64_alias_chain = signed_64_alias;

enum class shared_status : std::uint16_t { invalid_argument = 400, bad_gateway = 502 };
enum class signed8 : std::int16_t { minimum = -128, maximum = 127 };
enum class unsigned8 : std::uint16_t { minimum = 0, maximum = 255 };
enum class signed16 : std::int32_t { minimum = -32768, maximum = 32767 };
enum class unsigned16 : std::uint32_t { minimum = 0, maximum = 65535 };
enum class signed32 : std::int64_t { minimum = -2147483647LL - 1LL, maximum = 2147483647LL };
enum class unsigned32 : std::uint64_t { minimum = 0, maximum = 4294967295ULL };
enum class signed64 : std::int64_t { minimum = -9223372036854775807LL - 1LL, maximum = 9223372036854775807LL };
enum class unsigned64 : std::uint64_t { minimum = 0, maximum = 18446744073709551615ULL };
enum class legacy_status : std::uint16_t { value = 7 };
enum class opted_out_status : std::uint8_t { value = 9 };
enum class plain_status : std::uint16_t { value = 11 };
enum class public_inherited : std::uint16_t { value = 41 };
enum class public_using_private : std::uint16_t { value = 42 };
enum class public_using_protected : std::uint16_t { value = 43 };

struct public_wire_members {
   using type = std::int64_t;
   static constexpr bool shared = true;
};

struct protected_wire_members {
 protected:
   using type = std::int64_t;
   static constexpr bool shared = true;
};

struct access_payload {
   public_inherited inherited;
   public_using_private private_base;
   public_using_protected protected_base;
};

struct payload {
   shared_status scalar;
   std::array<shared_status, 2> fixed;
   std::vector<shared_status> sequence;
   std::array<signed8, 2> narrow_fixed;
   signed8 s8;
   unsigned8 u8;
   signed16 s16;
   unsigned16 u16;
   signed32 s32;
   unsigned32 u32;
   signed64 s64;
   unsigned64 u64;
   legacy_status legacy;
   opted_out_status opted_out;
   plain_status plain;
   std::array<plain_status, 2> plain_fixed;
};

} // namespace forge::tests::tooling::enum_wire

export namespace forge::raw {

template <> struct enum_wire_type<forge::tests::tooling::enum_wire::shared_status> {
   using type = forge::tests::tooling::enum_wire::signed_64_alias_chain;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed8> {
   using type = std::int8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned8> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed16> {
   using type = std::int16_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned16> {
   using type = std::uint16_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed32> {
   using type = std::int32_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned32> {
   using type = std::uint32_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed64> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned64> {
   using type = std::uint64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::legacy_status> {
   using type = std::int32_t;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::opted_out_status> {
   using type = double;
   static constexpr bool shared = false;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::public_inherited>
    : public forge::tests::tooling::enum_wire::public_wire_members {};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::public_using_private>
    : private forge::tests::tooling::enum_wire::public_wire_members {
   using public_wire_members::shared;
   using public_wire_members::type;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::public_using_protected>
    : protected forge::tests::tooling::enum_wire::protected_wire_members {
   using protected_wire_members::shared;
   using protected_wire_members::type;
};

} // namespace forge::raw
