module;

#include <forge/exceptions/macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

#include <boost/asio/awaitable.hpp>

export module forge.net.pnet.protector;

export import forge.exceptions;
export import forge.net.transport.connector;
import forge.crypto.core.secret_bytes;
import forge.net.pnet.network_fingerprint;

namespace forge::net::pnet::detail {
struct pre_shared_key_access;
}

export namespace forge::net::pnet::exceptions {

enum class code : std::uint16_t {
   invalid_options = 1,
   closed = 2,
   canceled = 3,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.net.pnet")

using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using closed = forge::exceptions::coded_exception<code, code::closed>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& error) noexcept {
   const auto& value = error.code();
   if (!value || std::string_view{value.category().name()} != "forge.net.pnet") {
      return std::nullopt;
   }
   return static_cast<code>(value.value());
}

} // namespace forge::net::pnet::exceptions

export namespace forge::net::pnet {

inline constexpr auto pre_shared_key_size = std::size_t{32};

struct operational_fingerprint {
   inline static constexpr auto byte_size = std::size_t{32};

   std::array<std::uint8_t, byte_size> bytes{};

   friend constexpr bool operator==(const operational_fingerprint&, const operational_fingerprint&) = default;
};

class pre_shared_key {
 public:
   explicit pre_shared_key(std::span<const std::uint8_t> bytes);
   ~pre_shared_key();

   pre_shared_key(pre_shared_key&&) noexcept;
   pre_shared_key& operator=(pre_shared_key&&) noexcept;

   pre_shared_key(const pre_shared_key&) = delete;
   pre_shared_key& operator=(const pre_shared_key&) = delete;

   [[nodiscard]] static pre_shared_key parse_swarm_key(std::string_view text);
   [[nodiscard]] operational_fingerprint fingerprint() const;
   [[nodiscard]] forge::net::pnet::network_fingerprint network_fingerprint() const;

 private:
   friend struct detail::pre_shared_key_access;

   forge::crypto::core::secret_bytes value_;
};

class protector {
 public:
   explicit protector(pre_shared_key key);
   ~protector();

   protector(protector&&) noexcept;
   protector& operator=(protector&&) noexcept;

   protector(const protector&) = delete;
   protector& operator=(const protector&) = delete;

   [[nodiscard]] boost::asio::awaitable<forge::net::transport::stream_connection>
   async_protect(forge::net::transport::stream_connection connection, std::stop_token stop = {}) const;
   [[nodiscard]] operational_fingerprint fingerprint() const;
   [[nodiscard]] forge::net::pnet::network_fingerprint network_fingerprint() const;

 private:
   std::shared_ptr<const pre_shared_key> key_;
};

} // namespace forge::net::pnet

namespace forge::net::pnet::detail {

struct pre_shared_key_access {
   [[nodiscard]] static std::span<const std::uint8_t> bytes(const pre_shared_key& value) noexcept;
};

} // namespace forge::net::pnet::detail
