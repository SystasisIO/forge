module;

#include <cstddef>
#include <cstdint>
#include <span>

export module forge.crypto.digest.shake128;

export import forge.crypto.digest;
import forge.crypto.core.types;

export namespace forge::crypto::digest {

// Output is measured in bytes; zero requests an empty digest.
[[nodiscard]] core::bytes shake128(std::span<const std::uint8_t> input, std::size_t output_size);

} // namespace forge::crypto::digest
