module;

#include <array>
#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

export module fixture.wire_values;

export import forge.api.core.types;
export import forge.net.http.method;
import forge.raw.enum_wire_type;

export namespace fixture {

enum class signed_value : std::int16_t { low = -32768, negative = -7, high = 32767 };
enum class unsigned_value : std::uint64_t { low = 0, high = 18446744073709551615ULL };
enum class narrow_value : std::int16_t { low = -128, high = 127 };
enum class signed_extreme : std::int64_t { low = (-9223372036854775807LL - 1), high = 9223372036854775807LL };
enum class legacy_value : std::uint16_t { value = 400 };

struct packet {
   forge::api::core::error_payload error;
   forge::net::http::method verb = forge::net::http::method::post;
   std::vector<forge::api::core::status> statuses;
   std::array<forge::api::core::status, 2> fixed{};
   signed_value signed_number = signed_value::negative;
   unsigned_value unsigned_number = unsigned_value::high;
   narrow_value narrow = narrow_value::low;
   std::optional<forge::api::core::status> optional;
   std::variant<forge::api::core::status, forge::net::http::method> choice;

   bool operator==(const packet&) const = default;
};

} // namespace fixture

export namespace forge::raw {

template <> struct enum_wire_type<fixture::signed_value> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<fixture::unsigned_value> {
   using type = std::uint64_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<fixture::narrow_value> {
   using type = std::int8_t;
   static constexpr bool shared = true;
};
template <> struct enum_wire_type<fixture::signed_extreme> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};

} // namespace forge::raw
