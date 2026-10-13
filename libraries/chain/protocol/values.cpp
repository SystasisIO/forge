module;

#include <forge/exceptions/policy.hpp>
#include <compare>
#include <cstdint>
#include <limits>
#include <string>

#if !defined(FORGE_CONTRACT_GUEST)
#include <stdexcept>
#endif

module forge.chain.protocol.values;

namespace forge::chain::protocol::detail {

[[noreturn]] void fail_value(const char* message) {
   FORGE_POLICY_THROW_STANDARD(std::invalid_argument, message);
}

} // namespace forge::chain::protocol::detail

namespace forge::chain::protocol {

namespace {

void require_value(bool condition, const char* message) {
   if (!condition) {
      detail::fail_value(message);
   }
}

void require_amount(int128_t value, const char* underflow, const char* overflow) {
   require_value(value >= -static_cast<int128_t>(asset::max_amount), underflow);
   require_value(value <= static_cast<int128_t>(asset::max_amount), overflow);
}

void require_same_symbol(const asset& left, const asset& right, const char* message) {
   require_value(left.sym == right.sym, message);
}

void require_same_contract(const extended_asset& left, const extended_asset& right) {
   require_value(left.contract == right.contract, "type mismatch");
}

std::string format_amount(std::int64_t amount, std::uint8_t precision) {
   const auto negative = amount < 0;
   const auto magnitude =
       negative ? std::uint64_t{0} - static_cast<std::uint64_t>(amount) : static_cast<std::uint64_t>(amount);
   auto digits = std::to_string(magnitude);
   auto result = negative ? std::string{"-"} : std::string{};
   if (precision == 0U) {
      return result + digits;
   }
   if (digits.size() <= precision) {
      result += "0.";
      result.append(static_cast<std::size_t>(precision) - digits.size(), '0');
      return result + digits;
   }
   const auto decimal = digits.size() - precision;
   result += digits.substr(0U, decimal);
   result.push_back('.');
   result += digits.substr(decimal);
   return result;
}


} // namespace

std::string to_string(const asset& value) {
   return format_amount(value.amount, value.sym.precision()) + " " + to_string(value.sym.code());
}

asset::asset(std::int64_t raw_amount, ::forge::chain::protocol::symbol raw_symbol)
    : amount(raw_amount), sym(raw_symbol) {
   require_value(is_amount_within_range(), "magnitude of asset amount must be less than 2^62");
   require_value(sym.is_valid(), "invalid symbol name");
}

void asset::set_amount(std::int64_t value) {
   amount = value;
   require_value(is_amount_within_range(), "magnitude of asset amount must be less than 2^62");
}

asset asset::operator-() const {
   require_value(amount != std::numeric_limits<std::int64_t>::min(), "signed negation overflow");
   return {-amount, sym};
}

asset& asset::operator+=(const asset& value) {
   require_same_symbol(*this, value, "attempt to add asset with different symbol");
   const auto result = static_cast<int128_t>(amount) + value.amount;
   require_amount(result, "addition underflow", "addition overflow");
   amount = static_cast<std::int64_t>(result);
   return *this;
}

asset& asset::operator-=(const asset& value) {
   require_same_symbol(*this, value, "attempt to subtract asset with different symbol");
   const auto result = static_cast<int128_t>(amount) - value.amount;
   require_amount(result, "subtraction underflow", "subtraction overflow");
   amount = static_cast<std::int64_t>(result);
   return *this;
}

asset& asset::operator*=(std::int64_t value) {
   const auto result = static_cast<int128_t>(amount) * value;
   require_amount(result, "multiplication underflow", "multiplication overflow");
   amount = static_cast<std::int64_t>(result);
   return *this;
}

asset& asset::operator/=(std::int64_t value) {
   require_value(value != 0, "divide by zero");
   require_value(!(amount == std::numeric_limits<std::int64_t>::min() && value == -1), "signed division overflow");
   amount /= value;
   return *this;
}

std::int64_t operator/(const asset& left, const asset& right) {
   require_value(right.amount != 0, "divide by zero");
   require_same_symbol(left, right, "attempt to divide assets with different symbol");
   return left.amount / right.amount;
}

bool operator==(const asset& left, const asset& right) {
   require_same_symbol(left, right, "comparison of assets with different symbols is not allowed");
   return left.amount == right.amount;
}

std::strong_ordering operator<=>(const asset& left, const asset& right) {
   require_same_symbol(left, right, "comparison of assets with different symbols is not allowed");
   return left.amount <=> right.amount;
}

extended_asset::extended_asset(std::int64_t amount, extended_symbol symbol)
    : quantity(amount, symbol.symbol), contract(symbol.contract) {}

extended_asset extended_asset::operator-() const {
   return {-quantity, contract};
}

extended_asset& extended_asset::operator+=(const extended_asset& value) {
   require_same_contract(*this, value);
   quantity += value.quantity;
   return *this;
}

extended_asset& extended_asset::operator-=(const extended_asset& value) {
   require_same_contract(*this, value);
   quantity -= value.quantity;
   return *this;
}

extended_asset& extended_asset::operator*=(std::int64_t value) {
   quantity *= value;
   return *this;
}

extended_asset& extended_asset::operator/=(std::int64_t value) {
   quantity /= value;
   return *this;
}

bool operator==(const extended_asset& left, const extended_asset& right) {
   return left.contract == right.contract && left.quantity == right.quantity;
}

std::strong_ordering operator<=>(const extended_asset& left, const extended_asset& right) {
   require_same_contract(left, right);
   return left.quantity <=> right.quantity;
}

} // namespace forge::chain::protocol
