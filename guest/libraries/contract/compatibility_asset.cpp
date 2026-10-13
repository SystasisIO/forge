module;

#include <compare>
#include <cstdint>
#include <string>

module forge.contract.compatibility_asset;

namespace forge::contract::compatibility {

asset::asset(std::int64_t raw_amount, chain::protocol::symbol raw_symbol)
    : asset(chain::protocol::asset{raw_amount, raw_symbol}) {}

void asset::set_amount(std::int64_t value) {
   auto converted = protocol_value();
   converted.set_amount(value);
   assign(converted);
}

std::string asset::to_string() const {
   return chain::protocol::to_string(protocol_value());
}

asset asset::operator-() const {
   return {-protocol_value()};
}

asset& asset::operator+=(const asset& value) {
   auto converted = protocol_value();
   converted += value.protocol_value();
   assign(converted);
   return *this;
}

asset& asset::operator-=(const asset& value) {
   auto converted = protocol_value();
   converted -= value.protocol_value();
   assign(converted);
   return *this;
}

asset& asset::operator*=(std::int64_t value) {
   auto converted = protocol_value();
   converted *= value;
   assign(converted);
   return *this;
}

asset& asset::operator/=(std::int64_t value) {
   auto converted = protocol_value();
   converted /= value;
   assign(converted);
   return *this;
}

asset operator+(asset left, const asset& right) {
   return left += right;
}

asset operator-(asset left, const asset& right) {
   return left -= right;
}

asset operator*(asset value, std::int64_t multiplier) {
   return value *= multiplier;
}

asset operator*(std::int64_t multiplier, asset value) {
   return value *= multiplier;
}

asset operator/(asset value, std::int64_t divisor) {
   return value /= divisor;
}

std::int64_t operator/(const asset& left, const asset& right) {
   return left.protocol_value() / right.protocol_value();
}

bool operator==(const asset& left, const asset& right) {
   return left.protocol_value() == right.protocol_value();
}

std::strong_ordering operator<=>(const asset& left, const asset& right) {
   return left.protocol_value() <=> right.protocol_value();
}

extended_asset::extended_asset(std::int64_t amount, chain::protocol::extended_symbol symbol)
    : extended_asset(chain::protocol::extended_asset{amount, symbol}) {}

extended_asset extended_asset::operator-() const {
   return {-protocol_value()};
}

extended_asset& extended_asset::operator+=(const extended_asset& value) {
   auto converted = protocol_value();
   converted += value.protocol_value();
   assign(converted);
   return *this;
}

extended_asset& extended_asset::operator-=(const extended_asset& value) {
   auto converted = protocol_value();
   converted -= value.protocol_value();
   assign(converted);
   return *this;
}

extended_asset& extended_asset::operator*=(std::int64_t value) {
   auto converted = protocol_value();
   converted *= value;
   assign(converted);
   return *this;
}

extended_asset& extended_asset::operator/=(std::int64_t value) {
   auto converted = protocol_value();
   converted /= value;
   assign(converted);
   return *this;
}

extended_asset operator+(extended_asset left, const extended_asset& right) {
   return left += right;
}

extended_asset operator-(extended_asset left, const extended_asset& right) {
   return left -= right;
}

extended_asset operator*(extended_asset value, std::int64_t multiplier) {
   return value *= multiplier;
}

extended_asset operator*(std::int64_t multiplier, extended_asset value) {
   return value *= multiplier;
}

extended_asset operator/(extended_asset value, std::int64_t divisor) {
   return value /= divisor;
}

std::int64_t operator/(const extended_asset& left, const extended_asset& right) {
   if (left.contract != right.contract) {
      chain::protocol::detail::fail_value("type mismatch");
   }
   return left.quantity / right.quantity;
}

bool operator==(const extended_asset& left, const extended_asset& right) {
   return left.protocol_value() == right.protocol_value();
}

std::strong_ordering operator<=>(const extended_asset& left, const extended_asset& right) {
   return left.protocol_value() <=> right.protocol_value();
}

} // namespace forge::contract::compatibility
