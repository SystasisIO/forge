#include <forge/contract/host.hpp>

#include <concepts>
#include <cstdint>
#include <type_traits>

using interface = forge::contract::host::interface;
using expression_v1 = forge::contract::host::expression_v1;

static_assert(forge::contract::host::interface_version == 1);
static_assert(!std::is_base_of_v<interface, expression_v1>);
static_assert(std::same_as<decltype(&expression_v1::expression_compile_v1),
                           std::uint32_t (expression_v1::*)(const char*, std::uint32_t, char*, std::uint32_t)>);
static_assert(std::same_as<decltype(&expression_v1::expression_import_v1),
                           std::uint32_t (expression_v1::*)(const char*, std::uint32_t, char*, std::uint32_t)>);
static_assert(std::same_as<decltype(&expression_v1::expression_evaluate_v1),
                           std::uint32_t (expression_v1::*)(const char*, std::uint32_t, char*, std::uint32_t)>);
static_assert(std::same_as<decltype(&interface::send_inline), void (interface::*)(char*, std::uint32_t)>);
static_assert(
    std::same_as<decltype(&interface::sha256), void (interface::*)(const char*, std::uint32_t, capi_checksum256*)>);
static_assert(std::same_as<decltype(&interface::call),
                           std::int64_t (interface::*)(std::uint64_t, std::uint64_t, const char*, std::uint32_t)>);
static_assert(
    std::same_as<decltype(&interface::bls_g1_add), std::int32_t (interface::*)(const char*, std::uint32_t, const char*,
                                                                               std::uint32_t, char*, std::uint32_t)>);
static_assert(
    std::same_as<decltype(&interface::set_finalizers), void (interface::*)(std::uint64_t, const char*, std::uint32_t)>);
static_assert(std::same_as<decltype(&interface::db_store_i64),
                           std::int32_t (interface::*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t,
                                                       const void*, std::uint32_t)>);
static_assert(std::same_as<decltype(&interface::db_get_i64),
                           std::int32_t (interface::*)(std::int32_t, const void*, std::uint32_t)>);
static_assert(std::same_as<decltype(&interface::db_idx128_find_primary),
                           std::int32_t (interface::*)(std::uint64_t, std::uint64_t, std::uint64_t, unsigned __int128*,
                                                       std::uint64_t)>);
static_assert(std::same_as<decltype(&interface::db_idx256_find_secondary),
                           std::int32_t (interface::*)(std::uint64_t, std::uint64_t, std::uint64_t,
                                                       const unsigned __int128*, std::uint32_t, std::uint64_t*)>);
static_assert(
    std::same_as<decltype(&interface::db_idx_double_lowerbound),
                 std::int32_t (interface::*)(std::uint64_t, std::uint64_t, std::uint64_t, double*, std::uint64_t*)>);
static_assert(std::same_as<decltype(&interface::db_idx_long_double_upperbound),
                           std::int32_t (interface::*)(std::uint64_t, std::uint64_t, std::uint64_t, long double*,
                                                       std::uint64_t*)>);
