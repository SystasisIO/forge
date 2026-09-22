module;

#include <forge/exceptions/macros.hpp>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

export module forge.crypto.symmetric.salsa20;

export import forge.exceptions;
import forge.crypto.core.secret_bytes;
import forge.crypto.core.types;

export namespace forge::crypto::symmetric::salsa20::exceptions {

enum class code : std::uint16_t {
   invalid_key = 1,
   invalid_nonce = 2,
   invalid_size = 3,
   backend_error = 4,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.symmetric.salsa20")

using invalid_key = forge::exceptions::coded_exception<code, code::invalid_key>;
using invalid_nonce = forge::exceptions::coded_exception<code, code::invalid_nonce>;
using invalid_size = forge::exceptions::coded_exception<code, code::invalid_size>;
using backend_error = forge::exceptions::coded_exception<code, code::backend_error>;

} // namespace forge::crypto::symmetric::salsa20::exceptions

export namespace forge::crypto::symmetric::salsa20 {

inline constexpr auto key_size = std::size_t{32};
inline constexpr auto nonce_size = std::size_t{8};
inline constexpr auto block_size = std::size_t{64};

struct nonce {
   std::array<std::uint8_t, nonce_size> bytes{};
};

class key {
 public:
   explicit key(std::span<const std::uint8_t> bytes);
   ~key();

   key(key&&) noexcept;
   key& operator=(key&&) noexcept;
   key(const key&) = delete;
   key& operator=(const key&) = delete;

   [[nodiscard]] std::span<const std::uint8_t> span() const & noexcept;
   [[nodiscard]] std::span<const std::uint8_t> span() const && = delete;

 private:
   core::secret_bytes value_;
};

[[nodiscard]] nonce make_nonce(std::span<const std::uint8_t> bytes);
// Starts at counter zero on every call. The caller owns and must erase the result.
[[nodiscard]] core::bytes keystream(const key& key, const nonce& nonce, std::size_t output_size);

} // namespace forge::crypto::symmetric::salsa20
