module;

#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

export module forge.chain.protocol.values;

import forge.raw.codec;

export namespace forge::chain::protocol {

namespace detail {
[[noreturn]] void fail_value(const char* message);
}

using int128_t = __int128;
using uint128_t = unsigned __int128;

constexpr std::uint64_t encode_symbol_code(std::string_view code);

struct name {
   enum class raw : std::uint64_t {};

   std::uint64_t value = 0;

   constexpr name(std::uint64_t raw_value = 0) : value(raw_value) {}
   constexpr explicit name(name::raw raw_value) : value(static_cast<std::uint64_t>(raw_value)) {}
   constexpr explicit name(std::string_view text);

   [[nodiscard]] static constexpr std::uint8_t char_to_value(char value) {
      if (value == '.') {
         return 0U;
      }
      if (value >= '1' && value <= '5') {
         return static_cast<std::uint8_t>(value - '1' + 1);
      }
      if (value >= 'a' && value <= 'z') {
         return static_cast<std::uint8_t>(value - 'a' + 6);
      }
      // Pinned CDT calls eosio::check(false) here; its following return is unreachable warning suppression.
      detail::fail_value("character is not in allowed character set for names");
   }

   constexpr operator raw() const noexcept {
      return raw{value};
   }

   constexpr explicit operator bool() const noexcept {
      return value != 0;
   }

   [[nodiscard]] constexpr std::uint8_t length() const noexcept {
      constexpr auto mask = std::uint64_t{0xf800000000000000ULL};
      if (value == 0U) {
         return 0U;
      }

      auto result = std::uint8_t{};
      auto current = value;
      for (auto index = std::uint8_t{}; index < 13U; ++index, current <<= 5U) {
         if ((current & mask) != 0U) {
            result = index;
         }
      }
      return static_cast<std::uint8_t>(result + 1U);
   }

   [[nodiscard]] std::string to_string() const;

   [[nodiscard]] constexpr name suffix() const noexcept {
      auto remaining_bits_after_last_actual_dot = std::uint32_t{};
      auto last_dot_bits = std::uint32_t{};
      for (auto remaining_bits = std::int32_t{59}; remaining_bits >= 4; remaining_bits -= 5) {
         const auto character = (value >> remaining_bits) & 0x1fULL;
         if (character == 0U) {
            last_dot_bits = static_cast<std::uint32_t>(remaining_bits);
         } else {
            remaining_bits_after_last_actual_dot = last_dot_bits;
         }
      }

      const auto thirteenth_character = value & 0x0fULL;
      if (thirteenth_character != 0U) {
         remaining_bits_after_last_actual_dot = last_dot_bits;
      }
      if (remaining_bits_after_last_actual_dot == 0U) {
         return *this;
      }

      const auto mask = (std::uint64_t{1} << remaining_bits_after_last_actual_dot) - 16U;
      const auto shift = 64U - remaining_bits_after_last_actual_dot;
      return name{((value & mask) << shift) + (thirteenth_character << (shift - 1U))};
   }

   [[nodiscard]] constexpr name prefix() const noexcept {
      auto result = value;
      auto saw_non_dot = false;
      auto mask = std::uint64_t{0x0fULL};
      for (auto offset = std::int32_t{}; offset <= 59;) {
         const auto character = (value >> offset) & mask;
         if (character == 0U) {
            if (saw_non_dot) {
               result = (value >> offset) << offset;
               break;
            }
         } else {
            saw_non_dot = true;
         }

         if (offset == 0) {
            offset += 4;
            mask = 0x1fULL;
         } else {
            offset += 5;
         }
      }
      return name{result};
   }

   constexpr bool operator==(const name&) const = default;
   constexpr auto operator<=>(const name&) const = default;
};

using account_name = name;
using action_name = name;
using permission_name = name;
using table_name = name;

struct permission_level {
   account_name actor;
   permission_name permission;

   constexpr bool operator==(const permission_level&) const = default;
   constexpr auto operator<=>(const permission_level&) const = default;
};

struct symbol_code {
   std::uint64_t value = 0;

   constexpr symbol_code() = default;
   constexpr explicit symbol_code(std::uint64_t raw_value) : value(raw_value) {}
   constexpr explicit symbol_code(std::string_view text) : value(encode_symbol_code(text)) {}

   [[nodiscard]] static symbol_code from_string(std::string_view text);
   [[nodiscard]] std::string to_string() const;

   constexpr std::uint64_t raw() const noexcept {
      return value;
   }

   [[nodiscard]] constexpr bool is_valid() const noexcept {
      auto current = value;
      if (current == 0U) {
         return false;
      }
      for (auto index = std::size_t{0}; index < 7U; ++index) {
         const auto character = static_cast<std::uint8_t>(current & 0xffU);
         if (character == 0U) {
            return (current >> 8U) == 0U;
         }
         if (character < 'A' || character > 'Z') {
            return false;
         }
         current >>= 8U;
      }
      return current == 0U;
   }

   [[nodiscard]] constexpr std::uint32_t length() const noexcept {
      auto current = value;
      auto result = std::uint32_t{};
      while ((current & 0xffU) != 0U && result < 7U) {
         ++result;
         current >>= 8U;
      }
      return result;
   }

   constexpr bool operator==(const symbol_code&) const = default;
   constexpr auto operator<=>(const symbol_code&) const = default;
};

struct symbol {
   std::uint64_t value = 0;

   constexpr symbol(std::uint64_t raw_value = 0) : value(raw_value) {}
   constexpr symbol(symbol_code code, std::uint8_t precision) : value((code.raw() << 8U) | precision) {}
   constexpr symbol(std::string_view code, std::uint8_t precision) : symbol(symbol_code{code}, precision) {}

   constexpr std::uint64_t raw() const noexcept {
      return value;
   }

   constexpr std::uint8_t precision() const noexcept {
      return static_cast<std::uint8_t>(value & 0xffU);
   }

   constexpr symbol_code code() const noexcept {
      return symbol_code{value >> 8U};
   }

   [[nodiscard]] constexpr bool is_valid() const noexcept {
      return code().is_valid();
   }

   constexpr bool operator==(const symbol&) const = default;
   constexpr auto operator<=>(const symbol&) const = default;
};

struct asset {
   static constexpr std::int64_t max_amount = (std::int64_t{1} << 62U) - 1;

   std::int64_t amount = 0;
   ::forge::chain::protocol::symbol sym{};

   constexpr asset(std::int64_t raw_amount = 0) : amount(raw_amount) {}
   asset(std::int64_t raw_amount, ::forge::chain::protocol::symbol raw_symbol);

   [[nodiscard]] constexpr bool is_amount_within_range() const noexcept {
      return -max_amount <= amount && amount <= max_amount;
   }

   [[nodiscard]] constexpr bool is_valid() const noexcept {
      return is_amount_within_range() && sym.is_valid();
   }

   void set_amount(std::int64_t value);

   asset operator-() const;
   asset& operator+=(const asset& value);
   asset& operator-=(const asset& value);
   asset& operator*=(std::int64_t value);
   asset& operator/=(std::int64_t value);

   friend asset operator+(asset left, const asset& right) {
      return left += right;
   }

   friend asset operator-(asset left, const asset& right) {
      return left -= right;
   }

   friend asset operator*(asset value, std::int64_t multiplier) {
      return value *= multiplier;
   }

   friend asset operator*(std::int64_t multiplier, asset value) {
      return value *= multiplier;
   }

   friend asset operator/(asset value, std::int64_t divisor) {
      return value /= divisor;
   }

   friend std::int64_t operator/(const asset& left, const asset& right);
   friend bool operator==(const asset& left, const asset& right);
   friend std::strong_ordering operator<=>(const asset& left, const asset& right);
};

[[nodiscard]] std::string to_string(const asset& value);

struct extended_symbol {
   ::forge::chain::protocol::symbol symbol{};
   account_name contract{};

   constexpr extended_symbol() = default;
   constexpr extended_symbol(::forge::chain::protocol::symbol raw_symbol, account_name raw_contract)
       : symbol(raw_symbol), contract(raw_contract) {}

   [[nodiscard]] constexpr auto get_symbol() const noexcept {
      return symbol;
   }

   [[nodiscard]] constexpr auto get_contract() const noexcept {
      return contract;
   }

   constexpr bool operator==(const extended_symbol&) const = default;
   constexpr auto operator<=>(const extended_symbol&) const = default;
};

struct extended_asset {
   asset quantity{};
   account_name contract{};

   constexpr extended_asset() = default;
   extended_asset(std::int64_t amount, extended_symbol symbol);
   constexpr extended_asset(asset value, account_name raw_contract) : quantity(value), contract(raw_contract) {}

   [[nodiscard]] constexpr extended_symbol get_extended_symbol() const noexcept {
      return {quantity.sym, contract};
   }

   extended_asset operator-() const;
   extended_asset& operator+=(const extended_asset& value);
   extended_asset& operator-=(const extended_asset& value);
   extended_asset& operator*=(std::int64_t value);
   extended_asset& operator/=(std::int64_t value);

   friend extended_asset operator+(extended_asset left, const extended_asset& right) {
      return left += right;
   }

   friend extended_asset operator-(extended_asset left, const extended_asset& right) {
      return left -= right;
   }

   friend extended_asset operator*(extended_asset value, std::int64_t multiplier) {
      return value *= multiplier;
   }

   friend extended_asset operator/(extended_asset value, std::int64_t divisor) {
      return value /= divisor;
   }

   friend bool operator==(const extended_asset& left, const extended_asset& right);
   friend std::strong_ordering operator<=>(const extended_asset& left, const extended_asset& right);
};

[[noreturn]] inline void fail_invalid_argument(const char* message) {
   detail::fail_value(message);
}

constexpr std::uint64_t encode_name(std::string_view text) {
   constexpr auto alphabet = std::string_view{".12345abcdefghijklmnopqrstuvwxyz"};
   if (text.size() > 13U) {
      fail_invalid_argument("chain name is longer than 13 characters");
   }

   auto result = std::uint64_t{0};
   for (auto index = std::size_t{0}; index < 13U; ++index) {
      auto encoded = std::uint8_t{0};
      if (index < text.size()) {
         const auto found = alphabet.find(text[index]);
         if (found == std::string_view::npos) {
            fail_invalid_argument("invalid chain name character");
         }
         encoded = static_cast<std::uint8_t>(found);
      }

      if (index < 12U) {
         result |= (static_cast<std::uint64_t>(encoded) & 0x1fULL) << (64U - 5U * (index + 1U));
      } else {
         if (encoded > 0x0fU) {
            fail_invalid_argument("chain name 13th character is outside the allowed range");
         }
         result |= static_cast<std::uint64_t>(encoded);
      }
   }
   return result;
}

constexpr name::name(std::string_view text) : value(encode_name(text)) {}

inline std::string decode_name(std::uint64_t raw) {
   constexpr auto alphabet = std::string_view{".12345abcdefghijklmnopqrstuvwxyz"};
   auto result = std::string(13U, '.');
   for (auto index = std::uint32_t{0}; index <= 12U; ++index) {
      result[12U - index] = alphabet[raw & (index == 0U ? 0x0fU : 0x1fU)];
      raw >>= index == 0U ? 4U : 5U;
   }
   const auto last = result.find_last_not_of('.');
   if (last == std::string::npos) {
      return {};
   }
   result.resize(last + 1U);
   return result;
}

constexpr name make_name(std::string_view value) {
   return name{encode_name(value)};
}

namespace literals {

consteval name operator""_n(const char* value, std::size_t size) {
   return make_name(std::string_view{value, size});
}

} // namespace literals

inline std::string to_string(const name& value) {
   return decode_name(value.value);
}

inline std::string name::to_string() const {
   return protocol::to_string(*this);
}

constexpr std::uint64_t encode_symbol_code(std::string_view code) {
   if (code.empty() || code.size() > 7U) {
      fail_invalid_argument("chain symbol code size is invalid");
   }
   auto result = std::uint64_t{0};
   for (auto index = std::size_t{0}; index < code.size(); ++index) {
      if (code[index] < 'A' || code[index] > 'Z') {
         fail_invalid_argument("chain symbol code must use A-Z");
      }
      result |= static_cast<std::uint64_t>(code[index]) << (8U * index);
   }
   return result;
}

inline std::string decode_symbol_code(std::uint64_t raw) {
   auto result = std::string{};
   for (auto index = std::size_t{0}; index < 7U; ++index) {
      const auto value = static_cast<char>((raw >> (8U * index)) & 0xffU);
      if (value == 0) {
         break;
      }
      result.push_back(value);
   }
   return result;
}

inline symbol_code make_symbol_code(std::string_view code) {
   return symbol_code{encode_symbol_code(code)};
}

inline symbol make_symbol(std::string_view code, std::uint8_t precision) {
   return symbol{make_symbol_code(code), precision};
}

inline std::string to_string(const symbol_code& value) {
   return decode_symbol_code(value.value);
}

inline symbol_code symbol_code::from_string(std::string_view text) {
   return symbol_code{text};
}

inline std::string symbol_code::to_string() const {
   return protocol::to_string(*this);
}

inline std::string to_string(const symbol& value) {
   return std::to_string(value.precision()) + "," + to_string(value.code());
}

template <typename Stream> void raw_pack(Stream& stream, const name& value) {
   forge::raw::pack(stream, value.value);
}

template <typename Stream> void raw_unpack(Stream& stream, name& value) {
   forge::raw::unpack(stream, value.value);
}

template <typename Stream> void raw_pack(Stream& stream, const permission_level& value) {
   forge::raw::pack(stream, value.actor);
   forge::raw::pack(stream, value.permission);
}

template <typename Stream> void raw_unpack(Stream& stream, permission_level& value) {
   forge::raw::unpack(stream, value.actor);
   forge::raw::unpack(stream, value.permission);
}

template <typename Stream> void raw_pack(Stream& stream, const symbol_code& value) {
   forge::raw::pack(stream, value.value);
}

template <typename Stream> void raw_unpack(Stream& stream, symbol_code& value) {
   forge::raw::unpack(stream, value.value);
}

template <typename Stream> void raw_pack(Stream& stream, const symbol& value) {
   forge::raw::pack(stream, value.value);
}

template <typename Stream> void raw_unpack(Stream& stream, symbol& value) {
   forge::raw::unpack(stream, value.value);
}

template <typename Stream> void raw_pack(Stream& stream, const asset& value) {
   forge::raw::pack(stream, value.amount);
   forge::raw::pack(stream, value.sym);
}

template <typename Stream> void raw_unpack(Stream& stream, asset& value) {
   forge::raw::unpack(stream, value.amount);
   forge::raw::unpack(stream, value.sym);
}

template <typename Stream> void raw_pack(Stream& stream, const extended_symbol& value) {
   forge::raw::pack(stream, value.symbol);
   forge::raw::pack(stream, value.contract);
}

template <typename Stream> void raw_unpack(Stream& stream, extended_symbol& value) {
   forge::raw::unpack(stream, value.symbol);
   forge::raw::unpack(stream, value.contract);
}

template <typename Stream> void raw_pack(Stream& stream, const extended_asset& value) {
   forge::raw::pack(stream, value.quantity);
   forge::raw::pack(stream, value.contract);
}

template <typename Stream> void raw_unpack(Stream& stream, extended_asset& value) {
   forge::raw::unpack(stream, value.quantity);
   forge::raw::unpack(stream, value.contract);
}

} // namespace forge::chain::protocol
