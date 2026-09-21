module;

#include <forge/exceptions/macros.hpp>

#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

module forge.net.quic.endpoint;

namespace forge::net::quic {
namespace {

constexpr auto scheme = std::string_view{"quic://"};

[[nodiscard]] bool starts_with(std::string_view value, std::string_view prefix) noexcept {
   return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

} // namespace

std::string endpoint::authority() const {
   if (host.find(':') != std::string::npos) {
      return "[" + host + (zone.empty() ? "" : "%" + zone) + "]:" + std::to_string(port);
   }
   return host + ":" + std::to_string(port);
}

endpoint parse_endpoint(std::string_view value) {
   if (!starts_with(value, scheme)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "QUIC endpoint must use quic:// scheme");
   }

   value.remove_prefix(scheme.size());
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "QUIC endpoint host is empty");
   }

   auto host_value = std::string_view{};
   auto port_value = std::string_view{};

   if (value.front() == '[') {
      const auto end = value.find(']');
      if (end == std::string_view::npos || end + 2 > value.size() || value[end + 1] != ':') {
         FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "invalid bracketed QUIC endpoint");
      }
      host_value = value.substr(1, end - 1);
      port_value = value.substr(end + 2);
   } else {
      const auto separator = value.rfind(':');
      if (separator == std::string_view::npos) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "QUIC endpoint port is missing");
      }
      host_value = value.substr(0, separator);
      port_value = value.substr(separator + 1);
   }

   if (host_value.empty() || port_value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "QUIC endpoint host or port is empty");
   }

   auto parsed_port = unsigned{};
   const auto* first = port_value.data();
   const auto* last = port_value.data() + port_value.size();
   const auto result = std::from_chars(first, last, parsed_port);
   if (result.ec != std::errc{} || result.ptr != last || parsed_port > std::numeric_limits<std::uint16_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "QUIC endpoint port is invalid");
   }

   auto zone = std::string{};
   if (const auto separator = host_value.find('%'); separator != std::string_view::npos) {
      if (host_value.find(':') == std::string_view::npos || separator + 1 == host_value.size() ||
          host_value.find('%', separator + 1) != std::string_view::npos) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_endpoint, "invalid QUIC endpoint zone");
      }
      zone = host_value.substr(separator + 1);
      host_value = host_value.substr(0, separator);
   }
   return endpoint{
       .host = std::string{host_value}, .port = static_cast<std::uint16_t>(parsed_port), .zone = std::move(zone)};
}

} // namespace forge::net::quic
