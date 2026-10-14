module;

#include <forge/exceptions/policy.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

export module forge.api.wasm.codec;

export import forge.api.wasm.validation;

#if !defined(FORGE_CONTRACT_GUEST)
import forge.exceptions;
import forge.raw.exceptions;
#endif

export namespace forge::api::wasm {

template <typename T> payload encode(std::string_view type, const T& value, const limits& bounds = {}) {
   validate_type_name(type, bounds);
   // Native serializers are trusted code; Raw pack_size would serialize twice.
   auto result = payload{std::string{type}, raw::pack(value)};
   validate(result, type, bounds);
   return result;
}

template <typename T> T decode(const payload& value, std::string_view expected_type, const limits& bounds = {}) {
   if (value.data.size() > bounds.wire.max_bytes) {
      FORGE_POLICY_THROW_EXCEPTION(raw::exceptions::allocation_limit, "Payload input exceeds the wire byte limit");
   }
   validate(value, expected_type, bounds);
   return raw::unpack_exact<T>(value.data, bounds.wire);
}

template <typename T> T decode(std::span<const std::uint8_t> input, const limits& bounds = {}) {
   if (input.size() > bounds.wire.max_bytes) {
      FORGE_POLICY_THROW_EXCEPTION(raw::exceptions::allocation_limit, "Protocol input exceeds the wire byte limit");
   }
   auto result = raw::unpack_exact<T>(input, bounds.wire);
   validate(result, bounds);
   return result;
}

} // namespace forge::api::wasm
