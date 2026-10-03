module;
#include <forge/exceptions/macros.hpp>
#include <array>
#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <utility>

export module forge.crypto.asymmetric.ed25519;

export import forge.exceptions;
import forge.crypto.core.types;
import forge.raw.raw;

export namespace forge::crypto::asymmetric::ed25519 {

namespace exceptions {

enum class code : std::uint16_t {
   invalid_key = 1,
   backend_error = 2,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.asymmetric.ed25519")

using invalid_key = forge::exceptions::coded_exception<code, code::invalid_key>;
using backend_error = forge::exceptions::coded_exception<code, code::backend_error>;

} // namespace exceptions

using public_key_data = std::array<std::uint8_t, 32>;
using private_key_secret = std::array<std::uint8_t, 32>;
using signature_data = std::array<std::uint8_t, 64>;

class public_key {
 public:
   public_key() = default;
   explicit public_key(const public_key_data& value);

   [[nodiscard]] const public_key_data& serialize() const noexcept;
   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] bool verify(std::span<const std::uint8_t> message, const signature_data& signature) const;

   friend bool operator==(const public_key&, const public_key&) = default;

 private:
   public_key_data data_{};
};

class private_key {
 public:
   using data_type = private_key_secret;

   private_key() = default;
   explicit private_key(const private_key_secret& value);
   private_key(const private_key&) = default;
   private_key(private_key&& other) noexcept;
   private_key& operator=(const private_key& other);
   private_key& operator=(private_key&& other) noexcept;
   ~private_key();

   [[nodiscard]] static private_key generate();
   [[nodiscard]] static private_key regenerate(const private_key_secret& value);

   [[nodiscard]] const private_key_secret& get_secret() const noexcept;
   [[nodiscard]] public_key get_public_key() const;
   [[nodiscard]] signature_data sign(std::span<const std::uint8_t> message) const;

   friend bool operator==(const private_key&, const private_key&) = default;

 private:
   private_key_secret data_{};
};

[[nodiscard]] constexpr bool diagnostic_is_secret(const private_key&) noexcept {
   return true;
}

} // namespace forge::crypto::asymmetric::ed25519
