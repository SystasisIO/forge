module;

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__APPLE__) || defined(__linux__)
#include <array>
#include <net/if.h>
#endif

#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

module forge.net.transport.endpoint;

namespace forge::net::transport {
namespace {

[[noreturn]] void throw_invalid_literal() {
   throw boost::system::system_error{boost::asio::error::invalid_argument, "invalid transport endpoint literal"};
}

#if defined(__APPLE__) || defined(__linux__)
[[nodiscard]] bool decimal(std::string_view value) {
   if (value.empty()) {
      return false;
   }
   for (const auto character : value) {
      if (character < '0' || character > '9') {
         return false;
      }
   }
   return true;
}
#endif

[[nodiscard]] boost::asio::ip::scope_id_type scope_id(std::string_view zone) {
   if (zone.empty() || zone.find('\0') != std::string_view::npos) {
      throw_invalid_literal();
   }

#if defined(__APPLE__) || defined(__linux__)
   if (zone.size() >= IF_NAMESIZE) {
      throw_invalid_literal();
   }

   if (decimal(zone)) {
      auto parsed = std::uint64_t{};
      const auto [position, error] = std::from_chars(zone.data(), zone.data() + zone.size(), parsed);
      if (error != std::errc{} || position != zone.data() + zone.size() || parsed == 0 ||
          parsed > std::numeric_limits<unsigned int>::max() ||
          parsed > std::numeric_limits<boost::asio::ip::scope_id_type>::max()) {
         throw_invalid_literal();
      }

      const auto value = static_cast<unsigned int>(parsed);
      auto name = std::array<char, IF_NAMESIZE>{};
      if (if_indextoname(value, name.data()) == nullptr) {
         throw_invalid_literal();
      }
      return static_cast<boost::asio::ip::scope_id_type>(value);
   }

   const auto value = if_nametoindex(std::string{zone}.c_str());
   if (value == 0) {
      throw_invalid_literal();
   }
   return static_cast<boost::asio::ip::scope_id_type>(value);
#else
   throw boost::system::system_error{boost::asio::error::operation_not_supported,
                                     "transport endpoint scope is unsupported"};
#endif
}

} // namespace

std::string endpoint::authority() const {
   if (host_type == host_kind::ip6) {
      auto scoped_host = host;
      if (!zone.empty()) {
         scoped_host += "%" + zone;
      }
      return "[" + scoped_host + "]:" + std::to_string(port);
   }
   return host + ":" + std::to_string(port);
}

boost::asio::ip::address endpoint::literal_address() const {
   if (host.empty() || host.find('\0') != std::string::npos) {
      throw_invalid_literal();
   }

   auto error = boost::system::error_code{};
   if (host_type == host_kind::ip4) {
      if (!zone.empty()) {
         throw_invalid_literal();
      }
      const auto address = boost::asio::ip::make_address_v4(host, error);
      if (error) {
         throw_invalid_literal();
      }
      return address;
   }
   if (host_type != host_kind::ip6) {
      throw_invalid_literal();
   }

   auto literal = std::string_view{host};
   auto effective_zone = std::string_view{zone};
   const auto separator = literal.find('%');
   if (separator != std::string_view::npos) {
      if (!zone.empty() || literal.find('%', separator + 1) != std::string_view::npos) {
         throw_invalid_literal();
      }
      effective_zone = literal.substr(separator + 1);
      literal = literal.substr(0, separator);
      if (effective_zone.empty()) {
         throw_invalid_literal();
      }
   }

   const auto address = boost::asio::ip::make_address_v6(std::string{literal}, error);
   if (error || (address.is_link_local() && effective_zone.empty())) {
      throw_invalid_literal();
   }
   if (effective_zone.empty()) {
      return address;
   }
   return boost::asio::ip::address_v6{address.to_bytes(), scope_id(effective_zone)};
}

endpoint endpoint::from_address(boost::asio::ip::address address, std::uint16_t port, protocol_kind protocol) {
   auto host = std::string{};
   auto zone = std::string{};
   auto host_type = host_kind::ip4;
   if (address.is_v6()) {
      const auto scoped = address.to_v6();
      host_type = host_kind::ip6;
      host = boost::asio::ip::address_v6{scoped.to_bytes()}.to_string();
      if (scoped.scope_id() != 0) {
         zone = std::to_string(scoped.scope_id());
      }
   } else {
      host = address.to_v4().to_string();
   }
   return endpoint{.host_type = host_type,
                   .protocol = protocol,
                   .host = std::move(host),
                   .port = port,
                   .zone = std::move(zone)};
}

} // namespace forge::net::transport
