#include <eosio/asset.hpp>
#include <string_view>

import forge.chain.protocol.values;

static_assert(CONSUMER_DECLARATION_VALUE == 42);
static_assert(std::string_view{CONSUMER_DECLARATION_TEXT} == "quoted \"value\" with space");

namespace {

constexpr auto modern_asset = forge::chain::protocol::asset{42};
constexpr auto legacy_asset = eosio::asset{42};

static_assert(modern_asset.amount == 42);
static_assert(modern_asset.sym.raw() == 0U);
static_assert(legacy_asset.amount == 42);
static_assert(legacy_asset.symbol.raw() == 0U);

} // namespace
