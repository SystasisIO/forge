module;

#include <compare>
#include <cstdint>
#include <string>

export module forge.contract.compatibility_asset;

import forge.chain.protocol.values;
import forge.contract.compatibility_name;
import forge.raw.codec;

export namespace forge::contract::compatibility {

struct asset {
   static constexpr std::int64_t max_amount = chain::protocol::asset::max_amount;

   std::int64_t amount = 0;
   chain::protocol::symbol symbol{};

   constexpr asset(std::int64_t raw_amount = 0) : amount(raw_amount) {}

   asset(std::int64_t raw_amount, chain::protocol::symbol raw_symbol);

   constexpr asset(const chain::protocol::asset& value) : amount(value.amount), symbol(value.sym) {}

   [[nodiscard]] constexpr bool is_amount_within_range() const noexcept {
      return -max_amount <= amount && amount <= max_amount;
   }

   [[nodiscard]] constexpr bool is_valid() const noexcept {
      return is_amount_within_range() && symbol.is_valid();
   }

   void set_amount(std::int64_t value);
   [[nodiscard]] std::string to_string() const;
   asset operator-() const;
   asset& operator+=(const asset& value);
   asset& operator-=(const asset& value);
   asset& operator*=(std::int64_t value);
   asset& operator/=(std::int64_t value);

   friend asset operator+(asset left, const asset& right);
   friend asset operator-(asset left, const asset& right);
   friend asset operator*(asset value, std::int64_t multiplier);
   friend asset operator*(std::int64_t multiplier, asset value);
   friend asset operator/(asset value, std::int64_t divisor);
   friend std::int64_t operator/(const asset& left, const asset& right);
   friend bool operator==(const asset& left, const asset& right);
   friend std::strong_ordering operator<=>(const asset& left, const asset& right);

   template <typename Stream> friend void raw_pack(Stream& stream, const asset& value) {
      chain::protocol::raw_pack(stream, value.protocol_value());
   }

   template <typename Stream> friend void raw_unpack(Stream& stream, asset& value) {
      auto converted = chain::protocol::asset{};
      chain::protocol::raw_unpack(stream, converted);
      value.assign(converted);
   }

 private:
   friend struct extended_asset;

   [[nodiscard]] constexpr chain::protocol::asset protocol_value() const noexcept {
      auto value = chain::protocol::asset{};
      value.amount = amount;
      value.sym = symbol;
      return value;
   }

   constexpr void assign(const chain::protocol::asset& value) noexcept {
      amount = value.amount;
      symbol = value.sym;
   }
};

struct extended_asset {
   asset quantity{};
   name contract{};

   constexpr extended_asset() = default;

   extended_asset(std::int64_t amount, chain::protocol::extended_symbol symbol);

   constexpr extended_asset(asset value, name raw_contract) : quantity(value), contract(raw_contract) {}

   constexpr extended_asset(const chain::protocol::extended_asset& value)
       : quantity(value.quantity), contract(value.contract) {}

   [[nodiscard]] constexpr chain::protocol::extended_symbol get_extended_symbol() const noexcept {
      return {quantity.symbol, contract};
   }

   extended_asset operator-() const;
   extended_asset& operator+=(const extended_asset& value);
   extended_asset& operator-=(const extended_asset& value);
   extended_asset& operator*=(std::int64_t value);
   extended_asset& operator/=(std::int64_t value);

   friend extended_asset operator+(extended_asset left, const extended_asset& right);
   friend extended_asset operator-(extended_asset left, const extended_asset& right);
   friend extended_asset operator*(extended_asset value, std::int64_t multiplier);
   friend extended_asset operator*(std::int64_t multiplier, extended_asset value);
   friend extended_asset operator/(extended_asset value, std::int64_t divisor);
   friend std::int64_t operator/(const extended_asset& left, const extended_asset& right);
   friend bool operator==(const extended_asset& left, const extended_asset& right);
   friend std::strong_ordering operator<=>(const extended_asset& left, const extended_asset& right);

   template <typename Stream> friend void raw_pack(Stream& stream, const extended_asset& value) {
      chain::protocol::raw_pack(stream, value.protocol_value());
   }

   template <typename Stream> friend void raw_unpack(Stream& stream, extended_asset& value) {
      auto converted = chain::protocol::extended_asset{};
      chain::protocol::raw_unpack(stream, converted);
      value.assign(converted);
   }

 private:
   [[nodiscard]] constexpr chain::protocol::extended_asset protocol_value() const noexcept {
      return {quantity.protocol_value(), static_cast<const name::base&>(contract)};
   }

   constexpr void assign(const chain::protocol::extended_asset& value) noexcept {
      quantity = asset{value.quantity};
      contract = name{value.contract};
   }
};

} // namespace forge::contract::compatibility
