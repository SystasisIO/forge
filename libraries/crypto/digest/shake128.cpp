module;

#include <forge/exceptions/macros.hpp>
#include <cstddef>
#include <cstdint>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <span>

module forge.crypto.digest.shake128;

import forge.crypto.core.secret_bytes;

#include "details/evp_digest_context.hxx"

namespace forge::crypto::digest {

core::bytes shake128(std::span<const std::uint8_t> input, std::size_t output_size) {
   auto output = core::bytes{};
   if (output_size > output.max_size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_size, "SHAKE128 output exceeds container capacity");
   }
   if (output_size == 0) {
      return output;
   }
   output.resize(output_size);
   auto context = forge::detail::evp_digest_context{};
   forge::detail::evp_digest_init(context.get(), EVP_shake128());
   if (!input.empty() && EVP_DigestUpdate(context.get(), input.data(), input.size()) != 1) {
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "error updating SHAKE128 digest",
                            forge::exceptions::ctx("code", static_cast<std::uint32_t>(ERR_get_error())));
   }
   if (EVP_DigestFinalXOF(context.get(), output.data(), output.size()) != 1) {
      core::secure_erase(output);
      FORGE_THROW_EXCEPTION(exceptions::backend_error, "error finalizing SHAKE128 digest",
                            forge::exceptions::ctx("code", static_cast<std::uint32_t>(ERR_get_error())));
   }
   return output;
}

} // namespace forge::crypto::digest
