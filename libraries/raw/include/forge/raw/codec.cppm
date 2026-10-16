module;

#include <boost/describe.hpp>
#include <boost/pfr/core.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module forge.raw.codec;

export import forge.raw.stream;
export import forge.raw.varint_value;
export import forge.raw.enum_wire_type;

namespace forge::raw::detail {

struct stream_probe_storage {};

} // namespace forge::raw::detail

namespace forge {

template <> class datastream<raw::detail::stream_probe_storage, void> {
 public:
   bool read(char*, std::size_t);
   bool write(const char*, std::size_t);
   bool put(char);
   bool get(char&);
   bool get(std::uint8_t&);
   bool skip(std::size_t);
   bool seekp(std::size_t);
   std::size_t tellp() const;
   std::size_t remaining() const;
   bool valid() const;
};

class raw_stream_probe : public datastream<raw::detail::stream_probe_storage> {};

template <typename Stream> class raw_concrete_stream_probe {
 public:
   operator Stream&() const;
};

} // namespace forge

export namespace forge::raw {

using bytes = std::vector<std::uint8_t>;

template <typename T> struct codec_traits {};

inline constexpr auto max_array_elements = std::uint32_t{1024U * 1024U};
inline constexpr auto max_byte_array_size = std::uint32_t{20U * 1024U * 1024U};

struct unpack_limits {
   std::uint32_t max_container_elements = max_array_elements;
   std::uint32_t max_total_container_elements = max_array_elements;
   std::uint32_t max_bytes = max_byte_array_size;
   std::uint32_t first_container_elements = max_array_elements;
};

} // namespace forge::raw

namespace forge::raw::detail {

datastream<std::vector<std::uint8_t>> make_input_stream(std::span<const std::uint8_t> input, unpack_limits limits = {});

} // namespace forge::raw::detail

export namespace forge::raw {

static_assert(CHAR_BIT == 8, "Forge raw serialization requires 8-bit bytes");

namespace detail {

[[noreturn]] void fail_codec(const char* message);
[[noreturn]] void fail_allocation(const char* message);

inline void require(bool condition, const char* message) {
   if (!condition) {
      fail_codec(message);
   }
}

template <typename Stream> void require_container_allocation(Stream& stream, std::size_t size, const char* message) {
   require(size <= max_array_elements, message);
   if constexpr (requires { stream.consume_container_elements(size); }) {
      if (!stream.consume_container_elements(size)) {
         fail_allocation(message);
      }
   }
}

template <typename Stream> void require_byte_allocation(const Stream& stream, std::size_t size, const char* message) {
   require(size <= max_byte_array_size, message);
   if constexpr (requires { stream.allocation_limits(); }) {
      if (size > stream.allocation_limits().bytes) {
         fail_allocation(message);
      }
   }
}

template <typename Stream> void write_bytes(Stream& stream, std::span<const std::byte> value) {
   stream.write(reinterpret_cast<const char*>(value.data()), value.size());
}

template <typename Stream> void read_bytes(Stream& stream, std::span<std::byte> value) {
   stream.read(reinterpret_cast<char*>(value.data()), value.size());
}

template <typename Stream, typename T> void write_object(Stream& stream, const T& value) {
   write_bytes(stream, std::as_bytes(std::span{&value, std::size_t{1}}));
}

template <typename Stream, typename T> void read_object(Stream& stream, T& value) {
   read_bytes(stream, std::as_writable_bytes(std::span{&value, std::size_t{1}}));
}

template <typename Stream, typename T>
concept adl_packable = requires(Stream& stream, const T& value) { raw_pack(stream, value); };

template <typename Stream, typename T>
concept adl_unpackable = requires(Stream& stream, T& value) { raw_unpack(stream, value); };

template <typename T> struct built_in_codec : std::false_type {};

template <> struct built_in_codec<std::string> : std::true_type {};

template <typename T, typename Allocator> struct built_in_codec<std::vector<T, Allocator>> : std::true_type {};

template <typename T, typename Allocator> struct built_in_codec<std::deque<T, Allocator>> : std::true_type {};

template <typename T, typename Allocator> struct built_in_codec<std::list<T, Allocator>> : std::true_type {};

template <typename Key, typename Compare, typename Allocator>
struct built_in_codec<std::set<Key, Compare, Allocator>> : std::true_type {};

template <typename Key, typename Value, typename Compare, typename Allocator>
struct built_in_codec<std::map<Key, Value, Compare, Allocator>> : std::true_type {};

template <typename T> struct built_in_codec<std::optional<T>> : std::true_type {};

template <typename First, typename Second> struct built_in_codec<std::pair<First, Second>> : std::true_type {};

template <typename... T> struct built_in_codec<std::tuple<T...>> : std::true_type {};

template <typename T, std::size_t Size> struct built_in_codec<std::array<T, Size>> : std::true_type {};

template <typename... T> struct built_in_codec<std::variant<T...>> : std::true_type {};

template <> struct built_in_codec<unsigned_int> : std::true_type {};

template <> struct built_in_codec<signed_int> : std::true_type {};

template <> struct built_in_codec<bool> : std::true_type {};

template <> struct built_in_codec<__int128> : std::true_type {};

template <> struct built_in_codec<unsigned __int128> : std::true_type {};

template <typename T> inline constexpr auto built_in_codec_v = built_in_codec<std::remove_cv_t<T>>::value;

template <typename T>
concept templated_stream_packable = requires(forge::raw_stream_probe& stream, const T& value) { stream << value; };

template <typename T>
concept templated_stream_unpackable = requires(forge::raw_stream_probe& stream, T& value) { stream >> value; };

template <typename Stream, typename T>
concept concrete_stream_packable =
    requires(forge::raw_concrete_stream_probe<Stream>& stream, const T& value) { stream << value; };

template <typename Stream, typename T>
concept concrete_stream_unpackable =
    requires(forge::raw_concrete_stream_probe<Stream>& stream, T& value) { stream >> value; };

template <typename Stream, typename T>
concept custom_stream_packable = templated_stream_packable<T> || concrete_stream_packable<Stream, T>;

template <typename Stream, typename T>
concept custom_stream_unpackable = templated_stream_unpackable<T> || concrete_stream_unpackable<Stream, T>;

template <typename Stream, typename T>
concept stream_packable =
    !std::is_arithmetic_v<T> && !std::is_enum_v<T> && !built_in_codec_v<T> && custom_stream_packable<Stream, T> &&
    requires(Stream& stream, const T& value) { stream << value; };

template <typename Stream, typename T>
concept stream_unpackable =
    !std::is_arithmetic_v<T> && !std::is_enum_v<T> && !built_in_codec_v<T> && custom_stream_unpackable<Stream, T> &&
    requires(Stream& stream, T& value) { stream >> value; };

template <typename Stream, typename T>
concept traits_packable =
    requires(Stream& stream, const T& value) { codec_traits<std::remove_cv_t<T>>::pack(stream, value); };

template <typename Stream, typename T>
concept traits_unpackable =
    requires(Stream& stream, T& value) { codec_traits<std::remove_cv_t<T>>::unpack(stream, value); };

template <std::size_t Index = 0, typename... T> void select_variant(std::variant<T...>& value, std::size_t selected) {
   if constexpr (Index < sizeof...(T)) {
      if (Index == selected) {
         value.template emplace<Index>();
         return;
      }
      select_variant<Index + 1>(value, selected);
   } else {
      fail_codec("raw variant index is out of range");
   }
}

template <typename Stream> void pack_unsigned_int(Stream& stream, std::uint32_t value) {
   auto encoded = static_cast<std::uint64_t>(value);
   do {
      auto byte = static_cast<std::uint8_t>(encoded & 0x7fU);
      encoded >>= 7U;
      byte |= static_cast<std::uint8_t>((encoded > 0U) << 7U);
      write_object(stream, byte);
   } while (encoded != 0U);
}

template <typename Stream> std::uint32_t unpack_unsigned_int(Stream& stream) {
   auto encoded = std::uint32_t{0};
   auto byte = char{0};
   auto shift = std::uint8_t{0};
   do {
      require(shift < 35U, "raw unsigned varint is too long");
      stream.get(byte);
      const auto octet = static_cast<std::uint8_t>(byte);
      if (shift == 28U) {
         require((octet & 0xf0U) == 0U, "raw unsigned varint overflows uint32");
      }
      encoded |= static_cast<std::uint32_t>(octet & 0x7fU) << shift;
      shift += 7U;
   } while ((static_cast<std::uint8_t>(byte) & 0x80U) != 0U);
   return encoded;
}

} // namespace detail

template <typename Stream, typename T>
   requires(std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
void pack(Stream& stream, const T& value) {
   detail::write_object(stream, value);
}

template <typename Stream, typename T>
   requires(std::is_arithmetic_v<T> && !std::is_same_v<std::remove_cv_t<T>, bool>)
void unpack(Stream& stream, T& value) {
   detail::read_object(stream, value);
}

template <typename Stream> void pack(Stream& stream, const __int128& value) {
   detail::write_object(stream, value);
}

template <typename Stream> void unpack(Stream& stream, __int128& value) {
   detail::read_object(stream, value);
}

template <typename Stream> void pack(Stream& stream, const unsigned __int128& value) {
   detail::write_object(stream, value);
}

template <typename Stream> void unpack(Stream& stream, unsigned __int128& value) {
   detail::read_object(stream, value);
}

namespace detail {

template <typename T>
using enum_codec_type_t = std::conditional_t<shared_enum_wire_v<T>, enum_wire_type_t<T>, std::underlying_type_t<T>>;

template <typename Stream, typename T> void pack_enum(Stream& stream, T value) {
   using wire = enum_codec_type_t<T>;
   const auto underlying = static_cast<std::underlying_type_t<T>>(value);
   if constexpr (shared_enum_wire_v<T>) {
      require(std::in_range<wire>(underlying), "enum value is outside its wire range");
   }
   write_object(stream, static_cast<wire>(underlying));
}

template <typename Stream, typename T> void unpack_enum(Stream& stream, T& value) {
   auto encoded = enum_codec_type_t<T>{};
   read_object(stream, encoded);
   if constexpr (shared_enum_wire_v<T>) {
      require(std::in_range<std::underlying_type_t<T>>(encoded), "enum wire value is outside its underlying range");
   }
   value = static_cast<T>(encoded);
}

} // namespace detail

template <typename Stream, typename T>
   requires(std::is_enum_v<T> && !detail::traits_packable<Stream, T>)
void pack(Stream& stream, const T& value) {
   detail::pack_enum(stream, value);
}

template <typename Stream, typename T>
   requires(std::is_enum_v<T> && !detail::traits_unpackable<Stream, T>)
void unpack(Stream& stream, T& value) {
   detail::unpack_enum(stream, value);
}

template <typename Stream> void pack(Stream& stream, const std::byte& value) {
   detail::write_object(stream, value);
}

template <typename Stream> void unpack(Stream& stream, std::byte& value) {
   detail::read_object(stream, value);
}

template <typename Stream, typename T>
   requires detail::adl_packable<Stream, T>
void pack(Stream& stream, const T& value) {
   raw_pack(stream, value);
}

template <typename Stream, typename T>
   requires detail::adl_unpackable<Stream, T>
void unpack(Stream& stream, T& value) {
   raw_unpack(stream, value);
}

template <typename Stream, typename T>
   requires(detail::stream_packable<Stream, T> && !detail::adl_packable<Stream, T>)
void pack(Stream& stream, const T& value) {
   stream << value;
}

template <typename Stream, typename T>
   requires(detail::stream_unpackable<Stream, T> && !detail::adl_unpackable<Stream, T>)
void unpack(Stream& stream, T& value) {
   stream >> value;
}

template <typename Stream, typename T>
   requires(detail::traits_packable<Stream, T> && !detail::adl_packable<Stream, T> &&
            !detail::stream_packable<Stream, T>)
void pack(Stream& stream, const T& value) {
   codec_traits<std::remove_cv_t<T>>::pack(stream, value);
}

template <typename Stream, typename T>
   requires(detail::traits_unpackable<Stream, T> && !detail::adl_unpackable<Stream, T> &&
            !detail::stream_unpackable<Stream, T>)
void unpack(Stream& stream, T& value) {
   codec_traits<std::remove_cv_t<T>>::unpack(stream, value);
}

template <typename Stream, typename T, std::size_t Size> void pack(Stream& stream, const T (&value)[Size]);

template <typename Stream, typename T, std::size_t Size> void unpack(Stream& stream, T (&value)[Size]);

template <typename Stream, typename T>
   requires(std::is_class_v<T> && std::is_aggregate_v<T> && !detail::adl_packable<Stream, T> &&
            !detail::traits_packable<Stream, T> && !detail::custom_stream_packable<Stream, T> &&
            !detail::built_in_codec_v<T>)
void pack(Stream& stream, const T& value);

template <typename Stream, typename T>
   requires(std::is_class_v<T> && std::is_aggregate_v<T> && !detail::adl_unpackable<Stream, T> &&
            !detail::traits_unpackable<Stream, T> && !detail::custom_stream_unpackable<Stream, T> &&
            !detail::built_in_codec_v<T>)
void unpack(Stream& stream, T& value);

template <typename Stream> void pack(Stream& stream, const std::string& value);

template <typename Stream> void unpack(Stream& stream, std::string& value);

template <typename Stream, typename T> void pack(Stream& stream, const std::vector<T>& value);

template <typename Stream, typename T> void unpack(Stream& stream, std::vector<T>& value);

template <typename Stream, typename T, typename Allocator>
void pack(Stream& stream, const std::deque<T, Allocator>& value);

template <typename Stream, typename T, typename Allocator> void unpack(Stream& stream, std::deque<T, Allocator>& value);

template <typename Stream, typename T, typename Allocator>
void pack(Stream& stream, const std::list<T, Allocator>& value);

template <typename Stream, typename T, typename Allocator> void unpack(Stream& stream, std::list<T, Allocator>& value);

template <typename Stream, typename Key, typename Compare, typename Allocator>
void pack(Stream& stream, const std::set<Key, Compare, Allocator>& value);

template <typename Stream, typename Key, typename Compare, typename Allocator>
void unpack(Stream& stream, std::set<Key, Compare, Allocator>& value);

template <typename Stream, typename Key, typename Value, typename Compare, typename Allocator>
void pack(Stream& stream, const std::map<Key, Value, Compare, Allocator>& value);

template <typename Stream, typename Key, typename Value, typename Compare, typename Allocator>
void unpack(Stream& stream, std::map<Key, Value, Compare, Allocator>& value);

template <typename Stream, typename T> void pack(Stream& stream, const std::optional<T>& value);

template <typename Stream, typename T> void unpack(Stream& stream, std::optional<T>& value);

template <typename Stream, typename First, typename Second>
void pack(Stream& stream, const std::pair<First, Second>& value);

template <typename Stream, typename First, typename Second>
void unpack(Stream& stream, std::pair<First, Second>& value);

template <typename Stream, typename... T> void pack(Stream& stream, const std::tuple<T...>& value);

template <typename Stream, typename... T> void unpack(Stream& stream, std::tuple<T...>& value);

template <typename Stream, typename T, std::size_t Size> void pack(Stream& stream, const std::array<T, Size>& value);

template <typename Stream, typename T, std::size_t Size> void unpack(Stream& stream, std::array<T, Size>& value);

template <typename Stream, typename... T> void pack(Stream& stream, const std::variant<T...>& value);

template <typename Stream, typename... T> void unpack(Stream& stream, std::variant<T...>& value);

template <typename Stream> void pack(Stream& stream, const bool& value);

template <typename Stream> void unpack(Stream& stream, bool& value);

template <typename Stream> void pack(Stream& stream, const signed_int& value) {
   auto encoded = (static_cast<std::uint32_t>(value.value) << 1U) ^ static_cast<std::uint32_t>(value.value >> 31U);
   do {
      auto byte = static_cast<std::uint8_t>(encoded & 0x7fU);
      encoded >>= 7U;
      byte |= static_cast<std::uint8_t>((encoded > 0U) << 7U);
      detail::write_object(stream, byte);
   } while (encoded != 0U);
}

template <typename Stream> void pack(Stream& stream, const unsigned_int& value) {
   detail::pack_unsigned_int(stream, value.value);
}

template <typename Stream> void unpack(Stream& stream, signed_int& value) {
   auto encoded = std::uint32_t{0};
   auto byte = char{0};
   auto shift = std::uint8_t{0};
   do {
      detail::require(shift < 35U, "raw signed varint is too long");
      stream.get(byte);
      const auto octet = static_cast<std::uint8_t>(byte);
      if (shift == 28U) {
         detail::require((octet & 0xf0U) == 0U, "raw signed varint overflows int32");
      }
      encoded |= static_cast<std::uint32_t>(octet & 0x7fU) << shift;
      shift += 7U;
   } while ((static_cast<std::uint8_t>(byte) & 0x80U) != 0U);
   value.value = static_cast<std::int32_t>((encoded >> 1U) ^ (0U - (encoded & 1U)));
}

template <typename Stream> void unpack(Stream& stream, unsigned_int& value) {
   value.value = detail::unpack_unsigned_int(stream);
}

template <typename Stream> void pack(Stream& stream, const bool& value) {
   pack(stream, static_cast<std::uint8_t>(value));
}

template <typename Stream> void unpack(Stream& stream, bool& value) {
   auto encoded = std::uint8_t{0};
   unpack(stream, encoded);
   detail::require(encoded <= 1U, "raw bool is not canonical");
   value = encoded != 0U;
}

template <typename Stream> void pack(Stream& stream, const char* value) {
   const auto size = std::strlen(value);
   detail::require(size <= max_byte_array_size, "raw string exceeds the byte limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(size));
   if (size != 0U) {
      stream.write(value, size);
   }
}

template <typename Stream> void pack(Stream& stream, const std::string& value) {
   detail::require(value.size() <= max_byte_array_size, "raw string exceeds the byte limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   if (!value.empty()) {
      stream.write(value.data(), value.size());
   }
}

template <typename Stream> void unpack(Stream& stream, std::string& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_byte_allocation(stream, size, "raw string exceeds the byte limit");
   value.resize(size);
   if (!value.empty()) {
      stream.read(value.data(), value.size());
   }
}

namespace detail {

template <typename Stream, typename Byte> void pack_byte_vector(Stream& stream, const std::vector<Byte>& value) {
   require(value.size() <= max_byte_array_size, "raw byte vector exceeds the byte limit");
   pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   if (!value.empty()) {
      write_bytes(stream, std::as_bytes(std::span{value}));
   }
}

template <typename Stream, typename Byte> void unpack_byte_vector(Stream& stream, std::vector<Byte>& value) {
   const auto size = unpack_unsigned_int(stream);
   require_byte_allocation(stream, size, "raw byte vector exceeds the byte limit");
   value.resize(size);
   if (!value.empty()) {
      read_bytes(stream, std::as_writable_bytes(std::span{value}));
   }
}

} // namespace detail

template <typename Stream> void pack(Stream& stream, const std::vector<char>& value) {
   detail::pack_byte_vector(stream, value);
}

template <typename Stream> void unpack(Stream& stream, std::vector<char>& value) {
   detail::unpack_byte_vector(stream, value);
}

template <typename Stream> void pack(Stream& stream, const std::vector<std::uint8_t>& value) {
   detail::pack_byte_vector(stream, value);
}

template <typename Stream> void unpack(Stream& stream, std::vector<std::uint8_t>& value) {
   detail::unpack_byte_vector(stream, value);
}

template <typename Stream> void pack(Stream& stream, const std::vector<std::byte>& value) {
   detail::pack_byte_vector(stream, value);
}

template <typename Stream> void unpack(Stream& stream, std::vector<std::byte>& value) {
   detail::unpack_byte_vector(stream, value);
}

template <typename Stream, typename T> void pack(Stream& stream, const std::vector<T>& value) {
   detail::require(value.size() <= max_array_elements, "raw vector exceeds the element limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T> void unpack(Stream& stream, std::vector<T>& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_container_allocation(stream, size, "raw vector exceeds the element limit");
   value.resize(size);
   for (auto& item : value) {
      unpack(stream, item);
   }
}

template <typename Stream, typename T, typename Allocator>
void pack(Stream& stream, const std::deque<T, Allocator>& value) {
   detail::require(value.size() <= max_array_elements, "raw deque exceeds the element limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T, typename Allocator>
void unpack(Stream& stream, std::deque<T, Allocator>& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_container_allocation(stream, size, "raw deque exceeds the element limit");
   value.resize(size);
   for (auto& item : value) {
      unpack(stream, item);
   }
}

template <typename Stream, typename T, typename Allocator>
void pack(Stream& stream, const std::list<T, Allocator>& value) {
   detail::require(value.size() <= max_array_elements, "raw list exceeds the element limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T, typename Allocator> void unpack(Stream& stream, std::list<T, Allocator>& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_container_allocation(stream, size, "raw list exceeds the element limit");
   value.clear();
   for (auto index = std::uint32_t{0}; index < size; ++index) {
      auto item = T{};
      unpack(stream, item);
      value.emplace_back(std::move(item));
   }
}

template <typename Stream, typename Key, typename Compare, typename Allocator>
void pack(Stream& stream, const std::set<Key, Compare, Allocator>& value) {
   detail::require(value.size() <= max_array_elements, "raw set exceeds the element limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename Key, typename Compare, typename Allocator>
void unpack(Stream& stream, std::set<Key, Compare, Allocator>& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_container_allocation(stream, size, "raw set exceeds the element limit");
   value.clear();
   for (auto index = std::uint32_t{0}; index < size; ++index) {
      auto item = Key{};
      unpack(stream, item);
      value.insert(std::move(item));
   }
}

template <typename Stream, typename Key, typename Value, typename Compare, typename Allocator>
void pack(Stream& stream, const std::map<Key, Value, Compare, Allocator>& value) {
   detail::require(value.size() <= max_array_elements, "raw map exceeds the element limit");
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.size()));
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename Key, typename Value, typename Compare, typename Allocator>
void unpack(Stream& stream, std::map<Key, Value, Compare, Allocator>& value) {
   const auto size = detail::unpack_unsigned_int(stream);
   detail::require_container_allocation(stream, size, "raw map exceeds the element limit");
   value.clear();
   for (auto index = std::uint32_t{0}; index < size; ++index) {
      auto item = std::pair<Key, Value>{};
      unpack(stream, item);
      value.insert(std::move(item));
   }
}

template <typename Stream, typename T> void pack(Stream& stream, const std::optional<T>& value) {
   pack(stream, value.has_value());
   if (value) {
      pack(stream, *value);
   }
}

template <typename Stream, typename T> void unpack(Stream& stream, std::optional<T>& value) {
   auto present = false;
   unpack(stream, present);
   if (!present) {
      value.reset();
      return;
   }
   value.emplace(T{});
   unpack(stream, *value);
}

template <typename Stream, typename First, typename Second>
void pack(Stream& stream, const std::pair<First, Second>& value) {
   pack(stream, value.first);
   pack(stream, value.second);
}

template <typename Stream, typename First, typename Second>
void unpack(Stream& stream, std::pair<First, Second>& value) {
   unpack(stream, value.first);
   unpack(stream, value.second);
}

template <typename Stream, typename... T> void pack(Stream& stream, const std::tuple<T...>& value) {
   std::apply([&](const auto&... item) { (pack(stream, item), ...); }, value);
}

template <typename Stream, typename... T> void unpack(Stream& stream, std::tuple<T...>& value) {
   std::apply([&](auto&... item) { (unpack(stream, item), ...); }, value);
}

template <typename Stream, typename T, std::size_t Size> void pack(Stream& stream, const std::array<T, Size>& value) {
   static_assert(Size <= max_array_elements, "raw array exceeds the element limit");
   if constexpr ((std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<std::remove_cv_t<T>, bool>) {
      detail::write_bytes(stream, std::as_bytes(std::span{value}));
   } else {
      for (const auto& item : value) {
         pack(stream, item);
      }
   }
}

template <typename Stream, typename T, std::size_t Size> void unpack(Stream& stream, std::array<T, Size>& value) {
   static_assert(Size <= max_array_elements, "raw array exceeds the element limit");
   if constexpr ((std::is_arithmetic_v<T> || std::is_enum_v<T>) && !std::is_same_v<std::remove_cv_t<T>, bool>) {
      detail::read_bytes(stream, std::as_writable_bytes(std::span{value}));
   } else {
      for (auto& item : value) {
         unpack(stream, item);
      }
   }
}

template <typename Stream, typename... T> void pack(Stream& stream, const std::variant<T...>& value) {
   detail::pack_unsigned_int(stream, static_cast<std::uint32_t>(value.index()));
   std::visit([&](const auto& selected) { pack(stream, selected); }, value);
}

template <typename Stream, typename... T> void unpack(Stream& stream, std::variant<T...>& value) {
   const auto selected = detail::unpack_unsigned_int(stream);
   detail::select_variant(value, selected);
   std::visit([&](auto& item) { unpack(stream, item); }, value);
}

template <typename Stream, typename T, std::size_t Size> void pack(Stream& stream, const T (&value)[Size]) {
   static_assert(Size <= max_array_elements, "raw C array exceeds the element limit");
   pack(stream, unsigned_int{Size});
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T, std::size_t Size> void unpack(Stream& stream, T (&value)[Size]) {
   static_assert(Size <= max_array_elements, "raw C array exceeds the element limit");
   auto size = unsigned_int{};
   unpack(stream, size);
   detail::require(size.value == Size, "T[] size and unpacked size don't match");
   for (auto& item : value) {
      unpack(stream, item);
   }
}

template <typename Stream, typename T>
   requires(std::is_class_v<T> && std::is_aggregate_v<T> && !detail::adl_packable<Stream, T> &&
            !detail::traits_packable<Stream, T> && !detail::custom_stream_packable<Stream, T> &&
            !detail::built_in_codec_v<T>)
void pack(Stream& stream, const T& value) {
   boost::pfr::for_each_field(value, [&](const auto& field) { pack(stream, field); });
}

template <typename Stream, typename T>
   requires(std::is_class_v<T> && std::is_aggregate_v<T> && !detail::adl_unpackable<Stream, T> &&
            !detail::traits_unpackable<Stream, T> && !detail::custom_stream_unpackable<Stream, T> &&
            !detail::built_in_codec_v<T>)
void unpack(Stream& stream, T& value) {
   boost::pfr::for_each_field(value, [&](auto& field) { unpack(stream, field); });
}

template <typename Stream, typename First, typename Second, typename... Rest>
void pack(Stream& stream, const First& first, const Second& second, const Rest&... rest) {
   pack(stream, first);
   pack(stream, second);
   (pack(stream, rest), ...);
}

template <typename Stream, typename First, typename Second, typename... Rest>
void unpack(Stream& stream, First& first, Second& second, Rest&... rest) {
   unpack(stream, first);
   unpack(stream, second);
   (unpack(stream, rest), ...);
}

template <typename T> std::size_t pack_size(const T& value) {
   // Use the same stream kind as one-shot packing so nested concrete codecs cannot select a different wire path.
   auto stream = datastream<std::vector<std::uint8_t>>{};
   pack(stream, value);
   return stream.tellp();
}

template <typename T> bytes pack(const T& value) {
   auto stream = datastream<std::vector<std::uint8_t>>{};
   pack(stream, value);
   return std::move(stream.storage());
}

template <typename T> void pack(std::vector<std::uint8_t>& output, const T& value) {
   output = pack(value);
}

template <typename T, typename... Rest> bytes pack(const T& value, const Rest&... rest) {
   auto stream = datastream<std::vector<std::uint8_t>>{};
   pack(stream, value, rest...);
   return std::move(stream.storage());
}

template <typename T> T unpack(std::span<const std::uint8_t> input) {
   auto value = T{};
   auto stream = detail::make_input_stream(input);
   unpack(stream, value);
   return value;
}

template <typename T> void unpack(std::span<const std::uint8_t> input, T& value) {
   auto stream = detail::make_input_stream(input);
   unpack(stream, value);
}

template <typename T> void unpack(std::span<const std::uint8_t> input, T& value, unpack_limits limits) {
   auto stream = detail::make_input_stream(input, limits);
   unpack(stream, value);
}

template <typename T> T unpack(std::span<const std::uint8_t> input, unpack_limits limits) {
   auto value = T{};
   unpack(input, value, limits);
   return value;
}

template <typename T> void unpack_exact(std::span<const std::uint8_t> input, T& value) {
   auto stream = detail::make_input_stream(input);
   unpack(stream, value);
   detail::require(stream.remaining() == 0U, "raw input contains trailing bytes");
}

template <typename T> void unpack_exact(std::span<const std::uint8_t> input, T& value, unpack_limits limits) {
   auto stream = detail::make_input_stream(input, limits);
   unpack(stream, value);
   detail::require(stream.remaining() == 0U, "raw input contains trailing bytes");
}

template <typename T> T unpack_exact(std::span<const std::uint8_t> input) {
   auto value = T{};
   auto stream = detail::make_input_stream(input);
   unpack(stream, value);
   detail::require(stream.remaining() == 0U, "raw input contains trailing bytes");
   return value;
}

template <typename T> T unpack_exact(std::span<const std::uint8_t> input, unpack_limits limits) {
   auto value = T{};
   unpack_exact(input, value, limits);
   return value;
}

template <typename T, typename ParentStream>
T unpack_nested_exact(ParentStream& parent, std::span<const std::uint8_t> input) {
   const auto frame_limit =
       static_cast<std::uint32_t>(std::min<std::size_t>(input.size(), std::numeric_limits<std::uint32_t>::max()));
   auto limits = unpack_limits{.max_container_elements = frame_limit,
                               .max_total_container_elements = frame_limit,
                               .max_bytes = frame_limit,
                               .first_container_elements = frame_limit};

   if constexpr (requires {
                    parent.allocation_limits();
                    parent.remaining_container_elements();
                    parent.consume_cumulative_container_elements(std::size_t{});
                 }) {
      const auto& parent_limits = parent.allocation_limits();
      limits.max_container_elements = static_cast<std::uint32_t>(
          std::min<std::size_t>({parent_limits.elements, input.size(), std::numeric_limits<std::uint32_t>::max()}));
      limits.max_total_container_elements = static_cast<std::uint32_t>(
          std::min<std::size_t>(parent.remaining_container_elements(), std::numeric_limits<std::uint32_t>::max()));
      limits.max_bytes = static_cast<std::uint32_t>(
          std::min<std::size_t>({parent_limits.bytes, input.size(), std::numeric_limits<std::uint32_t>::max()}));
      limits.first_container_elements = limits.max_container_elements;

      auto value = T{};
      auto nested = detail::make_input_stream(input, limits);
      unpack(nested, value);
      detail::require(nested.remaining() == 0U, "raw input contains trailing bytes");
      const auto consumed =
          static_cast<std::size_t>(limits.max_total_container_elements) - nested.remaining_container_elements();
      if (!parent.consume_cumulative_container_elements(consumed)) {
         detail::fail_allocation("raw nested input exceeds the cumulative element limit");
      }
      return value;
   } else {
      return unpack_exact<T>(input, limits);
   }
}

template <typename T> T unpack(const std::vector<std::uint8_t>& input) {
   return unpack<T>(std::span<const std::uint8_t>{input});
}

template <typename T> void unpack(const std::vector<std::uint8_t>& input, T& value) {
   unpack(std::span<const std::uint8_t>{input}, value);
}

template <typename T> void pack(std::uint8_t* destination, std::uint32_t size, const T& value) {
   auto stream = datastream<std::uint8_t*>{destination, size};
   pack(stream, value);
}

template <typename T> T unpack(const std::uint8_t* data, std::uint32_t size) {
   return unpack<T>(std::span<const std::uint8_t>{data, size});
}

template <typename T> void unpack(const std::uint8_t* data, std::uint32_t size, T& value) {
   unpack(std::span<const std::uint8_t>{data, size}, value);
}

} // namespace forge::raw

export namespace forge {

namespace raw::detail {

template <typename T>
inline constexpr bool character_pointer_v =
    std::is_pointer_v<std::remove_cv_t<T>> &&
    std::same_as<std::remove_cv_t<std::remove_pointer_t<std::remove_cv_t<T>>>, char>;

template <typename Stream, typename T>
inline constexpr bool stream_codec_writable_v =
    std::is_arithmetic_v<T> || std::is_enum_v<T> || std::same_as<std::remove_cv_t<T>, std::byte> ||
    character_pointer_v<T> || std::is_array_v<T> || built_in_codec_v<T> || adl_packable<Stream, T> ||
    traits_packable<Stream, T> || (std::is_class_v<T> && std::is_aggregate_v<T>);

template <typename Stream, typename T>
inline constexpr bool stream_codec_readable_v =
    std::is_arithmetic_v<T> || std::is_enum_v<T> || std::same_as<std::remove_cv_t<T>, std::byte> ||
    std::is_array_v<T> || built_in_codec_v<T> || adl_unpackable<Stream, T> || traits_unpackable<Stream, T> ||
    (std::is_class_v<T> && std::is_aggregate_v<T>);

} // namespace raw::detail

template <typename Storage, typename Enable, typename T>
   requires(!std::same_as<Storage, raw::detail::stream_probe_storage> &&
            !raw::detail::custom_stream_packable<datastream<Storage, Enable>, T> &&
            raw::detail::stream_codec_writable_v<datastream<Storage, Enable>, T>)
datastream<Storage, Enable>& operator<<(datastream<Storage, Enable>& stream, const T& value) {
   if constexpr (raw::detail::adl_packable<datastream<Storage, Enable>, T>) {
      raw_pack(stream, value);
   } else if constexpr (raw::detail::traits_packable<datastream<Storage, Enable>, T>) {
      raw::codec_traits<std::remove_cv_t<T>>::pack(stream, value);
   } else {
      raw::pack(stream, value);
   }
   return stream;
}

template <typename Storage, typename Enable, typename T>
   requires(!std::same_as<Storage, raw::detail::stream_probe_storage> &&
            !raw::detail::custom_stream_unpackable<datastream<Storage, Enable>, T> &&
            raw::detail::stream_codec_readable_v<datastream<Storage, Enable>, T>)
datastream<Storage, Enable>& operator>>(datastream<Storage, Enable>& stream, T& value) {
   if constexpr (raw::detail::adl_unpackable<datastream<Storage, Enable>, T>) {
      raw_unpack(stream, value);
   } else if constexpr (raw::detail::traits_unpackable<datastream<Storage, Enable>, T>) {
      raw::codec_traits<std::remove_cv_t<T>>::unpack(stream, value);
   } else {
      raw::unpack(stream, value);
   }
   return stream;
}

} // namespace forge

export namespace forge::raw {

BOOST_DESCRIBE_STRUCT(unpack_limits, (),
                      (max_container_elements, max_total_container_elements, max_bytes, first_container_elements))

} // namespace forge::raw
