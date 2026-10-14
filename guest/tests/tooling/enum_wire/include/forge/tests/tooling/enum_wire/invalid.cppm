module;

#include <cstddef>
#include <cstdint>

export module forge.tests.tooling.enum_wire.invalid;

import forge.raw.enum_wire_type;

export namespace forge::tests::tooling::enum_wire {

enum class bad_flag : std::uint16_t { value = 1 };
enum class mutable_flag : std::uint16_t { value = 1 };
enum class missing_type : std::uint16_t { value = 1 };
enum class non_type_alias : std::uint16_t { value = 1 };
enum class floating_type : std::uint16_t { value = 1 };
enum class boolean_type : std::uint16_t { value = 1 };
enum class wide_type : std::uint16_t { value = 1 };
enum class signed_overflow : std::uint16_t { value = 128 };
enum class signed_underflow : std::int16_t { value = -129 };
enum class unsigned_negative : std::int16_t { value = -1 };
enum class unsigned_to_signed64 : std::uint64_t { value = 18446744073709551615ULL };
enum class platform_long : std::uint16_t { value = 1 };
enum class platform_unsigned_long : std::uint16_t { value = 1 };
enum class platform_size : std::uint16_t { value = 1 };
enum class char_underlying : char { value = 1 };
enum class private_shared_base : std::uint16_t { value = 1 };
enum class protected_shared_base : std::uint16_t { value = 1 };
enum class private_type_base : std::uint16_t { value = 1 };
enum class protected_type_base : std::uint16_t { value = 1 };
enum class private_shared_using : std::uint16_t { value = 1 };
enum class protected_shared_using : std::uint16_t { value = 1 };
enum class private_type_using : std::uint16_t { value = 1 };
enum class protected_type_using : std::uint16_t { value = 1 };

struct shared_member {
   static constexpr bool shared = true;
};

struct type_member {
   using type = std::int64_t;
};

using long_alias = long;
using size_alias = std::size_t;

} // namespace forge::tests::tooling::enum_wire

export namespace forge::raw {

template <> struct enum_wire_type<forge::tests::tooling::enum_wire::bad_flag> {
   using type = std::int64_t;
   static constexpr int shared = 1;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::mutable_flag> {
   using type = std::int64_t;
   static bool shared;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::missing_type> {
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::non_type_alias> {
   static constexpr int type = 1;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::floating_type> {
   using type = double;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::boolean_type> {
   using type = bool;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::wide_type> {
   using type = __int128;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed_overflow> {
   using type = std::int8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::signed_underflow> {
   using type = std::int8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned_negative> {
   using type = std::uint8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::unsigned_to_signed64> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::platform_long> {
   using type = forge::tests::tooling::enum_wire::long_alias;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::platform_unsigned_long> {
   using type = unsigned long;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::platform_size> {
   using type = forge::tests::tooling::enum_wire::size_alias;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::char_underlying> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::private_shared_base>
    : private forge::tests::tooling::enum_wire::shared_member {
   using type = std::int64_t;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::protected_shared_base>
    : protected forge::tests::tooling::enum_wire::shared_member {
   using type = std::int64_t;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::private_type_base>
    : private forge::tests::tooling::enum_wire::type_member {
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::protected_type_base>
    : protected forge::tests::tooling::enum_wire::type_member {
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::private_shared_using>
    : public forge::tests::tooling::enum_wire::shared_member {
   using type = std::int64_t;
 private:
   using shared_member::shared;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::protected_shared_using>
    : public forge::tests::tooling::enum_wire::shared_member {
   using type = std::int64_t;
 protected:
   using shared_member::shared;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::private_type_using>
    : public forge::tests::tooling::enum_wire::type_member {
   static constexpr bool shared = true;
 private:
   using type_member::type;
};
template <> struct enum_wire_type<forge::tests::tooling::enum_wire::protected_type_using>
    : public forge::tests::tooling::enum_wire::type_member {
   static constexpr bool shared = true;
 protected:
   using type_member::type;
};

} // namespace forge::raw
