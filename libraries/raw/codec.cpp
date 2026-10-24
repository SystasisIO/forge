module;

#include <forge/exceptions/policy.hpp>

#include <new>
#include <span>
#include <vector>

module forge.raw.codec;

#if !defined(FORGE_CONTRACT_GUEST)
import forge.raw.exceptions;
#endif

namespace forge::raw::detail {

datastream<std::vector<std::uint8_t>> make_input_stream(std::span<const std::uint8_t> input, unpack_limits limits) {
   return datastream<std::vector<std::uint8_t>>{input, make_allocation_limits(limits)};
}

datastream<std::vector<std::uint8_t>> make_input_stream(std::span<const std::uint8_t> input, bounded_unpack_limits limits) {
   return datastream<std::vector<std::uint8_t>>{input, make_allocation_limits(limits)};
}

[[noreturn]] void fail_codec(const char* message) {
   FORGE_POLICY_THROW_EXCEPTION(forge::raw::exceptions::codec_error, message);
}

[[noreturn]] void fail_allocation(const char* message) {
   FORGE_POLICY_THROW_EXCEPTION(forge::raw::exceptions::allocation_limit, message);
}

} // namespace forge::raw::detail
