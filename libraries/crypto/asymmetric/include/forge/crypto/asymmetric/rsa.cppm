module;
#include <forge/exceptions/macros.hpp>
#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

export module forge.crypto.asymmetric.rsa;

export import forge.exceptions;
import forge.crypto.core.types;
import forge.raw.raw;

export namespace forge::crypto::asymmetric::rsa {

namespace exceptions {

enum class code : std::uint16_t {
   invalid_key = 1,
   invalid_signature = 2,
   backend_error = 3,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.asymmetric.rsa")

using invalid_key = forge::exceptions::coded_exception<code, code::invalid_key>;
using invalid_signature = forge::exceptions::coded_exception<code, code::invalid_signature>;
using backend_error = forge::exceptions::coded_exception<code, code::backend_error>;

} // namespace exceptions

using public_key_data = core::bytes;
using private_key_secret = core::bytes;
using signature_data = core::bytes;

class public_key {
 public:
   public_key() = default;
   explicit public_key(public_key_data value);

   [[nodiscard]] const public_key_data& serialize() const noexcept;
   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] bool verify(std::span<const std::uint8_t> message, const signature_data& signature) const;

   friend bool operator==(const public_key&, const public_key&) = default;

 private:
   public_key_data data_;
};

class private_key {
 public:
   using data_type = private_key_secret;

   private_key() = default;
   explicit private_key(private_key_secret value);
   private_key(const private_key&) = default;
   private_key(private_key&& other) noexcept;
   private_key& operator=(const private_key& other);
   private_key& operator=(private_key&& other) noexcept;
   ~private_key();

   [[nodiscard]] static private_key generate(std::uint32_t bits = 2048);
   [[nodiscard]] static private_key regenerate(private_key_secret value);

   [[nodiscard]] const private_key_secret& get_secret() const noexcept;
   [[nodiscard]] public_key get_public_key() const;
   [[nodiscard]] signature_data sign(std::span<const std::uint8_t> message) const;

   friend bool operator==(const private_key&, const private_key&) = default;

 private:
   private_key_secret data_;
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const private_key&) noexcept {
   return true;
}

} // namespace forge::crypto::asymmetric::rsa
