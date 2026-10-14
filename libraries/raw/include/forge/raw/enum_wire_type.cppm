module;

#include <cstdint>
#include <type_traits>

export module forge.raw.enum_wire_type;

namespace forge::raw::detail {

template <typename T> inline constexpr bool enum_wire_integer_v =
   std::is_same_v<T, std::int8_t> || std::is_same_v<T, std::uint8_t> ||
   std::is_same_v<T, std::int16_t> || std::is_same_v<T, std::uint16_t> ||
   std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint32_t> ||
   std::is_same_v<T, std::int64_t> || std::is_same_v<T, std::uint64_t>;

} // namespace forge::raw::detail

export namespace forge::raw {

// The default remains the legacy native described-enum contract. Only an
// explicit shared specialization also controls the portable codec and Abigen.
template <typename Enum> struct enum_wire_type {
   static_assert(std::is_enum_v<Enum>);
   using type = std::int64_t;
   static constexpr bool shared = false;
};

template <typename Enum> using enum_wire_type_t = typename enum_wire_type<Enum>::type;

template <typename Enum> inline constexpr bool shared_enum_wire_v = [] {
   static_assert(std::is_enum_v<Enum>);
   if constexpr (requires { enum_wire_type<Enum>::shared; }) {
      static_assert(std::is_same_v<std::remove_cv_t<decltype(enum_wire_type<Enum>::shared)>, bool>,
                    "enum wire shared marker must be a constexpr bool");
      if constexpr (enum_wire_type<Enum>::shared) {
         using wire = enum_wire_type_t<Enum>;
         static_assert(detail::enum_wire_integer_v<wire>,
                       "shared enum wire type must be a fixed-width signed or unsigned integer");
         static_assert(detail::enum_wire_integer_v<std::underlying_type_t<Enum>>,
                       "shared enum underlying type must be a fixed-width signed or unsigned integer");
         return true;
      }
   }
   return false;
}();

} // namespace forge::raw
