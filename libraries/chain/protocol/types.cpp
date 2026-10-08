module;

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

module forge.chain.protocol.types;

import forge.variant.conversion;
import forge.variant.value;

namespace forge::chain::protocol {
namespace {

bool is_digit(char value) {
   return value >= '0' && value <= '9';
}

std::uint64_t parse_decimal(std::string_view text, std::uint64_t limit, const char* invalid_message) {
   if (text.empty()) {
      fail_invalid_argument(invalid_message);
   }

   auto result = std::uint64_t{0};
   for (const auto value : text) {
      if (!is_digit(value)) {
         fail_invalid_argument(invalid_message);
      }
      const auto digit = static_cast<std::uint64_t>(value - '0');
      if (result > limit / 10U || (result == limit / 10U && digit > limit % 10U)) {
         fail_invalid_argument("chain numeric value is out of range");
      }
      result = result * 10U + digit;
   }
   return result;
}

symbol parse_symbol(std::string_view text) {
   const auto comma = text.find(',');
   if (comma == std::string_view::npos || comma == 0U || comma + 1U == text.size() ||
       text.find(',', comma + 1U) != std::string_view::npos) {
      fail_invalid_argument("chain symbol text must be '<precision>,<code>'");
   }

   const auto precision = parse_decimal(text.substr(0U, comma), std::numeric_limits<std::uint8_t>::max(),
                                        "chain symbol precision must use decimal digits");
   return make_symbol(text.substr(comma + 1U), static_cast<std::uint8_t>(precision));
}

asset parse_asset(std::string_view text) {
   const auto space = text.find(' ');
   if (space == std::string_view::npos || space == 0U || space + 1U == text.size() ||
       text.find(' ', space + 1U) != std::string_view::npos) {
      fail_invalid_argument("chain asset text must be '<amount> <symbol>'");
   }

   auto amount_text = text.substr(0U, space);
   const auto symbol_text = text.substr(space + 1U);
   auto negative = false;
   if (amount_text.front() == '+') {
      fail_invalid_argument("chain asset amount must not use explicit plus sign");
   }
   if (amount_text.front() == '-') {
      negative = true;
      amount_text.remove_prefix(1U);
      if (amount_text.empty()) {
         fail_invalid_argument("chain asset amount is empty");
      }
   }

   const auto dot = amount_text.find('.');
   auto whole = amount_text;
   auto fraction = std::string_view{};
   if (dot != std::string_view::npos) {
      if (amount_text.find('.', dot + 1U) != std::string_view::npos) {
         fail_invalid_argument("chain asset amount has multiple decimal points");
      }
      whole = amount_text.substr(0U, dot);
      fraction = amount_text.substr(dot + 1U);
      if (whole.empty() || fraction.empty()) {
         fail_invalid_argument("chain asset amount must have whole and fractional digits");
      }
      if (fraction.size() > std::numeric_limits<std::uint8_t>::max()) {
         fail_invalid_argument("chain asset precision is out of range");
      }
   }

   auto digits = std::string{};
   digits.reserve(whole.size() + fraction.size());
   digits += whole;
   digits += fraction;

   const auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
   const auto negative_limit = positive_limit + 1U;
   const auto magnitude =
       parse_decimal(digits, negative ? negative_limit : positive_limit, "chain asset amount must use decimal digits");
   const auto amount = negative ? (magnitude == negative_limit ? std::numeric_limits<std::int64_t>::min()
                                                               : -static_cast<std::int64_t>(magnitude))
                                : static_cast<std::int64_t>(magnitude);
   return asset{amount, make_symbol(symbol_text, static_cast<std::uint8_t>(fraction.size()))};
}

} // namespace

void to_variant(const name& value, forge::variant& variant) {
   variant = to_string(value);
}

void from_variant(const forge::variant& variant, name& value) {
   value = make_name(variant.as_string());
}

void to_variant(const symbol_code& value, forge::variant& variant) {
   variant = to_string(value);
}

void from_variant(const forge::variant& variant, symbol_code& value) {
   value = make_symbol_code(variant.as_string());
}

void to_variant(const symbol& value, forge::variant& variant) {
   variant = to_string(value);
}

void from_variant(const forge::variant& variant, symbol& value) {
   value = parse_symbol(variant.as_string());
}

void to_variant(const asset& value, forge::variant& variant) {
   variant = to_string(value);
}

void from_variant(const forge::variant& variant, asset& value) {
   value = parse_asset(variant.as_string());
}

} // namespace forge::chain::protocol
