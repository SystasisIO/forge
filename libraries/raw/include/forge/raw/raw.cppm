module;

#include <forge/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <boost/dynamic_bitset.hpp>
#include <boost/multiprecision/cpp_int.hpp>

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <flat_map>
#include <list>
#include <limits>
#include <map>
#include <memory>
#include <new>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

export module forge.raw.raw;

export import forge.raw.codec;

import forge.core.utility;
import forge.core.uint128;
import forge.exceptions;
import forge.raw.datastream;
import forge.raw.exceptions;
import forge.raw.varint;
import forge.reflect.reflect;
import forge.variant.chrono;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.dynamic_bitset;
import forge.variant.exceptions;
import forge.variant.format;
import forge.variant.multiprecision;
import forge.variant.static_variant;
import forge.variant.value;

export namespace forge::raw {

template <std::size_t Size>
using UInt = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    Size, Size, boost::multiprecision::unsigned_magnitude, boost::multiprecision::unchecked, void>>;

template <std::size_t Size>
using Int = boost::multiprecision::number<boost::multiprecision::cpp_int_backend<
    Size, Size, boost::multiprecision::signed_magnitude, boost::multiprecision::unchecked, void>>;

template <typename Stream> class variant_packer : public variant::visitor {
 public:
   explicit variant_packer(Stream& stream) : stream_(stream) {}

   void handle() const override {}

   void handle(const std::int64_t& value) const override {
      pack(stream_, value);
   }

   void handle(const std::uint64_t& value) const override {
      pack(stream_, value);
   }

   void handle(const double& value) const override {
      pack(stream_, value);
   }

   void handle(const bool& value) const override {
      pack(stream_, value);
   }

   void handle(const std::string& value) const override {
      pack(stream_, value);
   }

   void handle(const variant_object& value) const override {
      pack(stream_, value);
   }

   void handle(const variants& value) const override {
      pack(stream_, value);
   }

   void handle(const blob& value) const override {
      pack(stream_, value);
   }

 private:
   Stream& stream_;
};

template <typename Stream> void host_pack(Stream& stream, const variant& value) {
   pack(stream, static_cast<std::uint8_t>(value.get_type()));
   value.visit(variant_packer<Stream>{stream});
}

template <typename Stream> void host_unpack(Stream& stream, variant& value) {
   auto type = std::uint8_t{};
   unpack(stream, type);
   switch (type) {
   case variant::null_type:
      value = variant{};
      return;
   case variant::int64_type: {
      auto decoded = std::int64_t{};
      unpack(stream, decoded);
      value = decoded;
      return;
   }
   case variant::uint64_type: {
      auto decoded = std::uint64_t{};
      unpack(stream, decoded);
      value = decoded;
      return;
   }
   case variant::double_type: {
      auto decoded = double{};
      unpack(stream, decoded);
      value = decoded;
      return;
   }
   case variant::bool_type: {
      auto decoded = false;
      unpack(stream, decoded);
      value = decoded;
      return;
   }
   case variant::string_type: {
      auto decoded = std::string{};
      unpack(stream, decoded);
      value = std::move(decoded);
      return;
   }
   case variant::array_type: {
      auto decoded = variants{};
      unpack(stream, decoded);
      value = std::move(decoded);
      return;
   }
   case variant::object_type: {
      auto decoded = variant_object{};
      unpack(stream, decoded);
      value = std::move(decoded);
      return;
   }
   case variant::blob_type: {
      auto decoded = blob{};
      unpack(stream, decoded);
      value = std::move(decoded);
      return;
   }
   default:
      FORGE_THROW_EXCEPTION(forge::raw::exceptions::codec_error, "unknown variant type",
                            forge::exceptions::ctx("type", type));
   }
}

template <typename Stream> void host_pack(Stream& stream, const variant_object& value) {
   pack(stream, unsigned_int{value.size()});
   for (const auto& item : value) {
      pack(stream, item.key());
      pack(stream, item.value());
   }
}

template <typename Stream> void host_unpack(Stream& stream, variant_object& value) {
   auto size = unsigned_int{};
   unpack(stream, size);
   detail::require_container_allocation(stream, size.value, "raw variant object exceeds the element limit");
   auto decoded = mutable_variant_object{};
   decoded.reserve(size.value);
   for (auto index = std::uint32_t{0}; index < size.value; ++index) {
      auto key = std::string{};
      auto item = variant{};
      unpack(stream, key);
      unpack(stream, item);
      decoded.set(std::move(key), std::move(item));
   }
   value = std::move(decoded);
}

template <typename Stream> void host_pack(Stream& stream, const std::filesystem::path& value) {
   pack(stream, value.generic_string());
}

template <typename Stream> void host_unpack(Stream& stream, std::filesystem::path& value) {
   auto decoded = std::string{};
   unpack(stream, decoded);
   value = std::move(decoded);
}

template <typename Stream> void host_pack(Stream& stream, const std::chrono::sys_seconds& value) {
   const auto count = value.time_since_epoch().count();
   if (count < 0 || count > std::numeric_limits<std::uint32_t>::max()) {
      throw std::out_of_range{"sys_seconds is outside FC time_point_sec wire range"};
   }
   pack(stream, static_cast<std::uint32_t>(count));
}

template <typename Stream> void host_unpack(Stream& stream, std::chrono::sys_seconds& value) {
   auto seconds = std::uint32_t{};
   unpack(stream, seconds);
   value = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
}

template <typename Stream>
void host_pack(Stream& stream, const std::chrono::sys_time<std::chrono::microseconds>& value) {
   pack(stream, static_cast<std::uint64_t>(value.time_since_epoch().count()));
}

template <typename Stream> void host_unpack(Stream& stream, std::chrono::sys_time<std::chrono::microseconds>& value) {
   auto microseconds = std::uint64_t{};
   unpack(stream, microseconds);
   value = std::chrono::sys_time<std::chrono::microseconds>{
       std::chrono::microseconds{static_cast<std::int64_t>(microseconds)}};
}

template <typename Stream> void host_pack(Stream& stream, const std::chrono::microseconds& value) {
   pack(stream, static_cast<std::uint64_t>(value.count()));
}

template <typename Stream> void host_unpack(Stream& stream, std::chrono::microseconds& value) {
   auto microseconds = std::uint64_t{};
   unpack(stream, microseconds);
   value = std::chrono::microseconds{static_cast<std::int64_t>(microseconds)};
}

template <typename Stream> void host_pack(Stream& stream, const forge::uint128& value) {
   detail::write_object(stream, value);
}

template <typename Stream> void host_unpack(Stream& stream, forge::uint128& value) {
   detail::read_object(stream, value);
}

template <typename Stream, typename T, std::size_t Size>
   requires(!std::is_same_v<std::remove_cv_t<T>, char>)
void host_pack(Stream& stream, const T (&value)[Size]) {
   pack(stream, unsigned_int{Size});
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T, std::size_t Size>
   requires(!std::is_same_v<std::remove_cv_t<T>, char>)
void host_unpack(Stream& stream, T (&value)[Size]) {
   auto size = unsigned_int{};
   unpack(stream, size);
   FORGE_ASSERT(size.value == Size);
   for (auto& item : value) {
      unpack(stream, item);
   }
}

template <typename Stream, typename T> void host_pack(Stream& stream, const std::shared_ptr<T>& value) {
   pack(stream, static_cast<bool>(value));
   if (value) {
      pack(stream, *value);
   }
}

template <typename Stream, typename T> void host_unpack(Stream& stream, std::shared_ptr<T>& value) {
   auto present = false;
   unpack(stream, present);
   if (!present) {
      value.reset();
      return;
   }
   auto decoded = std::make_shared<std::remove_const_t<T>>();
   unpack(stream, *decoded);
   value = std::move(decoded);
}

template <typename Stream, typename T> void host_pack(Stream& stream, const std::unordered_set<T>& value) {
   FORGE_ASSERT(value.size() <= max_array_elements);
   pack(stream, unsigned_int{value.size()});
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename T> void host_unpack(Stream& stream, std::unordered_set<T>& value) {
   auto size = unsigned_int{};
   unpack(stream, size);
   detail::require_container_allocation(stream, size.value, "raw unordered set exceeds the element limit");
   value.clear();
   value.reserve(size.value);
   for (auto index = std::uint32_t{0}; index < size.value; ++index) {
      auto item = T{};
      unpack(stream, item);
      value.insert(std::move(item));
   }
}

template <typename Stream, typename Key, typename Value>
void host_pack(Stream& stream, const std::unordered_map<Key, Value>& value) {
   FORGE_ASSERT(value.size() <= max_array_elements);
   pack(stream, unsigned_int{value.size()});
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename Key, typename Value>
void host_unpack(Stream& stream, std::unordered_map<Key, Value>& value) {
   auto size = unsigned_int{};
   unpack(stream, size);
   detail::require_container_allocation(stream, size.value, "raw unordered map exceeds the element limit");
   value.clear();
   value.reserve(size.value);
   for (auto index = std::uint32_t{0}; index < size.value; ++index) {
      auto item = std::pair<Key, Value>{};
      unpack(stream, item);
      value.insert(std::move(item));
   }
}

template <typename Stream, typename Key, typename Value, typename Compare, typename Keys, typename Values>
void host_pack(Stream& stream, const std::flat_map<Key, Value, Compare, Keys, Values>& value) {
   FORGE_ASSERT(value.size() <= max_array_elements);
   pack(stream, unsigned_int{value.size()});
   for (const auto& item : value) {
      pack(stream, item);
   }
}

template <typename Stream, typename Key, typename Value, typename Compare, typename Keys, typename Values>
void host_unpack(Stream& stream, std::flat_map<Key, Value, Compare, Keys, Values>& value) {
   auto size = unsigned_int{};
   unpack(stream, size);
   detail::require_container_allocation(stream, size.value, "raw flat map exceeds the element limit");
   value.clear();
   for (auto index = std::uint32_t{0}; index < size.value; ++index) {
      auto item = std::pair<Key, Value>{};
      unpack(stream, item);
      value.insert(value.end(), std::move(item));
   }
}

template <typename Stream> void host_pack(Stream& stream, const forge::dynamic_bitset& value) {
   const auto block_count = value.num_blocks();
   FORGE_ASSERT(block_count <= max_array_elements);
   pack(stream, unsigned_int{value.size()});
   auto blocks = std::vector<forge::dynamic_bitset::block_type>(block_count);
   boost::to_block_range(value, blocks.begin());
   for (const auto block : blocks) {
      pack(stream, block);
   }
}

template <typename Stream> void host_unpack(Stream& stream, forge::dynamic_bitset& value) {
   auto size = unsigned_int{};
   unpack(stream, size);
   constexpr auto bits_per_block = sizeof(forge::dynamic_bitset::block_type) * CHAR_BIT;
   const auto block_count = (size.value + bits_per_block - 1U) / bits_per_block;
   detail::require_container_allocation(stream, block_count, "raw dynamic bitset exceeds the element limit");
   auto blocks = std::vector<forge::dynamic_bitset::block_type>(block_count);
   for (auto& block : blocks) {
      unpack(stream, block);
   }
   value = forge::dynamic_bitset{blocks.cbegin(), blocks.cend()};
   value.resize(size.value);
}

template <typename Stream, typename T> void host_pack(Stream& stream, const boost::multiprecision::number<T>& value) {
   static_assert(sizeof(value) == (std::numeric_limits<boost::multiprecision::number<T>>::digits + 1) / 8,
                 "unexpected multiprecision padding");
   detail::write_object(stream, value);
}

template <typename Stream, typename T> void host_unpack(Stream& stream, boost::multiprecision::number<T>& value) {
   static_assert(sizeof(value) == (std::numeric_limits<boost::multiprecision::number<T>>::digits + 1) / 8,
                 "unexpected multiprecision padding");
   detail::read_object(stream, value);
}

template <typename Stream> void host_pack(Stream& stream, const UInt<256>& value) {
   pack(stream, static_cast<UInt<128>>(value));
   pack(stream, static_cast<UInt<128>>(value >> 128U));
}

template <typename Stream> void host_unpack(Stream& stream, UInt<256>& value) {
   auto words = std::array<UInt<128>, 2>{};
   unpack(stream, words[0]);
   unpack(stream, words[1]);
   value = words[1];
   value <<= 128U;
   value |= words[0];
}

namespace detail {

template <typename T>
concept host_codec = requires(forge::datastream<std::size_t>& writer, forge::datastream<const std::uint8_t*>& reader,
                              const T& input, T& output) {
   host_pack(writer, input);
   host_unpack(reader, output);
};

} // namespace detail

template <typename T>
   requires detail::host_codec<T>
struct codec_traits<T> {
   template <typename Stream> static void pack(Stream& stream, const T& value) {
      host_pack(stream, value);
   }

   template <typename Stream> static void unpack(Stream& stream, T& value) {
      host_unpack(stream, value);
   }
};

template <typename T>
   requires(!detail::host_codec<T> &&
            (forge::reflect::is_described_enum_v<T> || forge::reflect::is_described_object_v<T>))
struct codec_traits<T> {
   template <typename Stream> static void pack(Stream& stream, const T& value) {
      if constexpr (forge::reflect::is_described_enum_v<T>) {
         if constexpr (shared_enum_wire_v<T>) {
            detail::pack_enum(stream, value);
         } else {
            forge::raw::pack(stream, static_cast<enum_wire_type_t<T>>(value));
         }
      } else {
         forge::reflect::for_each_member<T>([&](const char*, auto member) { forge::raw::pack(stream, value.*member); });
      }
   }

   template <typename Stream> static void unpack(Stream& stream, T& value) {
      if constexpr (forge::reflect::is_described_enum_v<T>) {
         if constexpr (shared_enum_wire_v<T>) {
            detail::unpack_enum(stream, value);
         } else {
            auto encoded = enum_wire_type_t<T>{};
            forge::raw::unpack(stream, encoded);
            value = static_cast<T>(encoded);
         }
      } else {
         forge::reflect::for_each_member<T>([&](const char*, auto member) {
            using member_type = std::remove_reference_t<decltype(value.*member)>;
            forge::raw::unpack(stream, const_cast<std::remove_const_t<member_type>&>(value.*member));
         });
      }
   }
};

} // namespace forge::raw
