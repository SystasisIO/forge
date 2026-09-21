module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

module forge.crypto.symmetric.salsa20;

namespace {

extern "C" int forge_salsa20_vendor_keystream(unsigned char* output, unsigned long long size,
                                             const unsigned char* nonce, const unsigned char* key);

} // namespace

namespace forge::crypto::symmetric::salsa20 {

key::key(std::span<const std::uint8_t> bytes) {
   if (bytes.size() != key_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "Salsa20 requires a 32-byte key");
   }
   value_ = core::secret_bytes{bytes};
}

key::~key() = default;
key::key(key&&) noexcept = default;
key& key::operator=(key&&) noexcept = default;

std::span<const std::uint8_t> key::span() const & noexcept {
   return value_.span();
}

nonce make_nonce(std::span<const std::uint8_t> bytes) {
   if (bytes.size() != nonce_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_nonce, "Salsa20 requires an 8-byte nonce");
   }
   auto result = nonce{};
   std::copy(bytes.begin(), bytes.end(), result.bytes.begin());
   return result;
}

core::bytes keystream(const salsa20::key& key, const salsa20::nonce& nonce, std::size_t output_size) {
   static_assert(std::numeric_limits<std::size_t>::max() <= std::numeric_limits<unsigned long long>::max());
   if (key.span().size() != key_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_key, "Salsa20 key has no usable secret material");
   }
   auto output = core::bytes{};
   if (output_size > output.max_size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_size, "Salsa20 output exceeds container capacity");
   }
   if (output_size == 0) {
      return output;
   }
   output.resize(output_size);
   if (forge_salsa20_vendor_keystream(output.data(), static_cast<unsigned long long>(output_size),
                                      nonce.bytes.data(), key.span().data()) != 0) {
      core::secure_erase(output);
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "Salsa20 backend rejected keystream generation");
   }
   return output;
}

} // namespace forge::crypto::symmetric::salsa20
