module;

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

module forge.multiformats.multiaddr;

import forge.multiformats.exceptions;

import forge.codec.base58;
import forge.multiformats.varint;

namespace forge::multiformats {
namespace {

[[nodiscard]] std::vector<std::string_view> split_path(std::string_view value) {
   if (value.empty() || value.front() != '/') {
      throw exceptions::invalid_format{"multiaddr must start with '/'"};
   }

   auto parts = std::vector<std::string_view>{};
   std::size_t begin = 1;
   while (begin <= value.size()) {
      const auto end = value.find('/', begin);
      auto part = value.substr(begin, end == std::string_view::npos ? end : end - begin);
      if (part.empty()) {
         throw exceptions::invalid_format{"multiaddr contains an empty component"};
      }
      parts.push_back(part);
      if (end == std::string_view::npos) {
         break;
      }
      begin = end + 1;
   }
   return parts;
}

[[nodiscard]] std::uint16_t parse_port(std::string_view value) {
   std::uint32_t port = 0;
   auto result = std::from_chars(value.data(), value.data() + value.size(), port);
   if (result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
       port > std::numeric_limits<std::uint16_t>::max()) {
      throw exceptions::invalid_format{"multiaddr port is invalid"};
   }
   return static_cast<std::uint16_t>(port);
}

[[nodiscard]] bytes parse_ip(std::string_view value, int family, std::size_t size) {
   auto out = bytes(size);
   const auto text = std::string{value};
   if (inet_pton(family, text.c_str(), out.data()) != 1) {
      throw exceptions::invalid_format{"multiaddr IP literal is invalid"};
   }
   return out;
}

[[nodiscard]] std::string format_ip(std::span<const std::uint8_t> data, int family) {
   auto buffer = std::array<char, INET6_ADDRSTRLEN>{};
   if (inet_ntop(family, data.data(), buffer.data(), static_cast<socklen_t>(buffer.size())) == nullptr) {
      throw exceptions::invalid_format{"multiaddr IP bytes are invalid"};
   }
   return buffer.data();
}

void append_var(bytes& out, std::uint64_t value) {
   auto encoded = varint_encode(value);
   out.insert(out.end(), encoded.begin(), encoded.end());
}

void append_prefixed(bytes& out, std::span<const std::uint8_t> payload) {
   append_var(out, payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
}

[[nodiscard]] std::string read_prefixed_string(std::span<const std::uint8_t> data, std::size_t& offset) {
   const auto length = varint_decode(data.subspan(offset));
   offset += length.size;
   if (length.value > data.size() - offset) {
      throw exceptions::invalid_format{"multiaddr variable component length exceeds available bytes"};
   }
   auto out = std::string{};
   out.reserve(static_cast<std::size_t>(length.value));
   for (std::size_t i = 0; i < length.value; ++i) {
      out.push_back(static_cast<char>(data[offset + i]));
   }
   offset += static_cast<std::size_t>(length.value);
   return out;
}

[[nodiscard]] std::uint16_t read_big_endian_port(std::span<const std::uint8_t> data, std::size_t& offset) {
   if (data.size() - offset < 2) {
      throw exceptions::invalid_format{"multiaddr port is truncated"};
   }
   const auto port = static_cast<std::uint16_t>((data[offset] << 8U) | data[offset + 1]);
   offset += 2;
   return port;
}

void append_big_endian_port(bytes& out, std::uint16_t port) {
   out.push_back(static_cast<std::uint8_t>((port >> 8U) & 0xffU));
   out.push_back(static_cast<std::uint8_t>(port & 0xffU));
}

[[nodiscard]] multicodec_code to_multicodec(protocol_code code) noexcept {
   return static_cast<multicodec_code>(code_value(code));
}

[[nodiscard]] protocol_code from_multicodec(multicodec_code code) {
   switch (code) {
   case multicodec_code::ip4:
      return protocol_code::ip4;
   case multicodec_code::tcp:
      return protocol_code::tcp;
   case multicodec_code::ip6:
      return protocol_code::ip6;
   case multicodec_code::ip6zone:
      return protocol_code::ip6zone;
   case multicodec_code::dns:
      return protocol_code::dns;
   case multicodec_code::dns4:
      return protocol_code::dns4;
   case multicodec_code::dns6:
      return protocol_code::dns6;
   case multicodec_code::dnsaddr:
      return protocol_code::dnsaddr;
   case multicodec_code::udp:
      return protocol_code::udp;
   case multicodec_code::p2p_circuit:
      return protocol_code::p2p_circuit;
   case multicodec_code::p2p:
      return protocol_code::p2p;
   case multicodec_code::quic:
      return protocol_code::quic;
   case multicodec_code::quic_v1:
      return protocol_code::quic_v1;
   case multicodec_code::ws:
      return protocol_code::ws;
   case multicodec_code::wss:
      return protocol_code::wss;
   default:
      throw exceptions::invalid_format{"unsupported multiaddr protocol"};
   }
}

[[nodiscard]] protocol_code parse_multiaddr_protocol(std::string_view value) {
   return from_multicodec(parse_protocol_code(value));
}

[[nodiscard]] std::string_view multiaddr_protocol_name(protocol_code code) {
   return protocol_name(to_multicodec(code));
}

void validate_component(const multiaddr_component& component) {
   switch (component.code) {
   case protocol_code::ip4:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      (void)parse_ip(component.value, AF_INET, 4);
      break;
   case protocol_code::ip6:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      (void)parse_ip(component.value, AF_INET6, 16);
      break;
   case protocol_code::ip6zone:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      if (component.value.contains('/')) {
         throw exceptions::invalid_format{"multiaddr ip6zone value must not contain '/'"};
      }
      break;
   case protocol_code::tcp:
   case protocol_code::udp:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      (void)parse_port(component.value);
      break;
   case protocol_code::dns:
   case protocol_code::dns4:
   case protocol_code::dns6:
   case protocol_code::dnsaddr:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      if (component.value.contains('/')) {
         throw exceptions::invalid_format{"multiaddr DNS protocol value must not contain '/'"};
      }
      break;
   case protocol_code::p2p:
      if (component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
      }
      try {
         const auto bytes = forge::codec::base58::decode(component.value);
         if (bytes.empty()) {
            throw exceptions::invalid_format{"multiaddr p2p component is invalid"};
         }
      } catch (const forge::exceptions::base&) {
         throw exceptions::invalid_format{"multiaddr p2p component is invalid"};
      }
      break;
   case protocol_code::p2p_circuit:
   case protocol_code::quic:
   case protocol_code::quic_v1:
   case protocol_code::ws:
   case protocol_code::wss:
      if (!component.value.empty()) {
         throw exceptions::invalid_format{"multiaddr protocol must not have a value"};
      }
      break;
   default:
      throw exceptions::invalid_format{"unsupported multiaddr protocol"};
   }
}

} // namespace

multiaddr multiaddr::parse(std::string_view value) {
   auto parts = split_path(value);
   auto result = multiaddr{};

   for (std::size_t i = 0; i < parts.size(); ++i) {
      auto code = parse_multiaddr_protocol(parts[i]);
      switch (code) {
      case protocol_code::ip4:
      case protocol_code::ip6:
      case protocol_code::ip6zone:
      case protocol_code::dns:
      case protocol_code::dns4:
      case protocol_code::dns6:
      case protocol_code::dnsaddr:
      case protocol_code::tcp:
      case protocol_code::udp:
      case protocol_code::p2p:
         if (++i >= parts.size()) {
            throw exceptions::invalid_format{"multiaddr protocol is missing a value"};
         }
         result.push({.code = code, .value = std::string{parts[i]}});
         break;
      case protocol_code::p2p_circuit:
      case protocol_code::quic:
      case protocol_code::quic_v1:
      case protocol_code::ws:
      case protocol_code::wss:
         result.push({.code = code, .value = {}});
         break;
      default:
         throw exceptions::invalid_format{"unsupported address protocol"};
      }
   }

   return result;
}

multiaddr multiaddr::from_bytes(std::span<const std::uint8_t> data) {
   auto result = multiaddr{};
   std::size_t offset = 0;
   while (offset < data.size()) {
      std::size_t consumed = 0;
      auto code = from_multicodec(multicodec_decode(data.subspan(offset), consumed));
      offset += consumed;

      switch (code) {
      case protocol_code::ip4: {
         if (data.size() - offset < 4) {
            throw exceptions::invalid_format{"multiaddr ip4 component is truncated"};
         }
         result.push({.code = code, .value = format_ip(data.subspan(offset, 4), AF_INET)});
         offset += 4;
         break;
      }
      case protocol_code::ip6: {
         if (data.size() - offset < 16) {
            throw exceptions::invalid_format{"multiaddr ip6 component is truncated"};
         }
         result.push({.code = code, .value = format_ip(data.subspan(offset, 16), AF_INET6)});
         offset += 16;
         break;
      }
      case protocol_code::ip6zone:
         result.push({.code = code, .value = read_prefixed_string(data, offset)});
         break;
      case protocol_code::tcp:
      case protocol_code::udp:
         result.push({.code = code, .value = std::to_string(read_big_endian_port(data, offset))});
         break;
      case protocol_code::dns:
      case protocol_code::dns4:
      case protocol_code::dns6:
      case protocol_code::dnsaddr:
         result.push({.code = code, .value = read_prefixed_string(data, offset)});
         break;
      case protocol_code::p2p: {
         const auto length = varint_decode(data.subspan(offset));
         offset += length.size;
         if (length.value > data.size() - offset) {
            throw exceptions::invalid_format{"multiaddr p2p component length exceeds available bytes"};
         }
         auto peer_bytes = bytes{data.begin() + static_cast<std::ptrdiff_t>(offset),
                                 data.begin() + static_cast<std::ptrdiff_t>(offset + length.value)};
         result.push({.code = code, .value = forge::codec::base58::encode(peer_bytes)});
         offset += static_cast<std::size_t>(length.value);
         break;
      }
      case protocol_code::p2p_circuit:
      case protocol_code::quic:
      case protocol_code::quic_v1:
      case protocol_code::ws:
      case protocol_code::wss:
         result.push({.code = code, .value = {}});
         break;
      default:
         throw exceptions::invalid_format{"unsupported address protocol"};
      }
   }

   return result;
}

std::string multiaddr::to_string() const {
   auto out = std::string{};
   for (const auto& component : components_) {
      out += "/";
      out += multiaddr_protocol_name(component.code);
      if (!component.value.empty()) {
         out += "/";
         out += component.value;
      }
   }
   return out;
}

bytes multiaddr::to_bytes() const {
   auto out = bytes{};
   for (const auto& component : components_) {
      append_var(out, code_value(component.code));
      switch (component.code) {
      case protocol_code::ip4: {
         auto parsed = parse_ip(component.value, AF_INET, 4);
         out.insert(out.end(), parsed.begin(), parsed.end());
         break;
      }
      case protocol_code::ip6: {
         auto parsed = parse_ip(component.value, AF_INET6, 16);
         out.insert(out.end(), parsed.begin(), parsed.end());
         break;
      }
      case protocol_code::ip6zone: {
         auto payload = bytes{component.value.begin(), component.value.end()};
         append_prefixed(out, payload);
         break;
      }
      case protocol_code::tcp:
      case protocol_code::udp:
         append_big_endian_port(out, parse_port(component.value));
         break;
      case protocol_code::dns:
      case protocol_code::dns4:
      case protocol_code::dns6:
      case protocol_code::dnsaddr: {
         auto payload = bytes{component.value.begin(), component.value.end()};
         append_prefixed(out, payload);
         break;
      }
      case protocol_code::p2p: {
         auto payload = forge::codec::base58::decode(component.value);
         append_prefixed(out, payload);
         break;
      }
      case protocol_code::p2p_circuit:
      case protocol_code::quic:
      case protocol_code::quic_v1:
      case protocol_code::ws:
      case protocol_code::wss:
         break;
      default:
         throw exceptions::invalid_format{"unsupported address protocol"};
      }
   }
   return out;
}

const std::vector<multiaddr_component>& multiaddr::components() const noexcept {
   return components_;
}

multiaddr multiaddr::encapsulate(const multiaddr& value) const {
   auto out = *this;
   out.components_.insert(out.components_.end(), value.components_.begin(), value.components_.end());
   return out;
}

multiaddr multiaddr::decapsulate(const multiaddr& value) const {
   if (value.components_.empty() || value.components_.size() > components_.size()) {
      return *this;
   }

   auto found = std::optional<std::size_t>{};
   const auto needle_size = value.components_.size();
   for (std::size_t offset = 0; offset + needle_size <= components_.size(); ++offset) {
      if (std::equal(value.components_.begin(), value.components_.end(),
                     components_.begin() + static_cast<std::ptrdiff_t>(offset))) {
         found = offset;
      }
   }
   if (!found) {
      return *this;
   }

   auto out = multiaddr{};
   out.components_.assign(components_.begin(), components_.begin() + static_cast<std::ptrdiff_t>(*found));
   return out;
}

void multiaddr::push(multiaddr_component value) {
   validate_component(value);
   components_.push_back(std::move(value));
}

} // namespace forge::multiformats
