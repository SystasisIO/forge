module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.reachability;

import forge.multiformats.multiaddr;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.exceptions;
import forge.net.p2p.exceptions;

#include "details/protobuf.hxx"

namespace forge::net::p2p {
namespace {

[[nodiscard]] std::vector<std::uint8_t> endpoint_bytes(const endpoint& value) {
   return forge::multiformats::multiaddr::parse(value.to_string()).to_bytes();
}

[[nodiscard]] endpoint endpoint_from_bytes(std::span<const std::uint8_t> value) {
   return parse_endpoint(forge::multiformats::multiaddr::from_bytes(value).to_string());
}

[[nodiscard]] std::vector<std::uint8_t> encode_peer_info(const reachability::peer_info& value,
                                                          reachability::options options) {
   if (value.endpoints.size() > options.max_endpoints) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT peer has too many addresses");
   }
   auto out = std::vector<std::uint8_t>{};
   detail::append_bytes(out, 1, value.peer.to_bytes());
   for (const auto& item : value.endpoints) {
      const auto encoded = endpoint_bytes(item);
      detail::append_bytes(out, 2, encoded);
   }
   return out;
}

[[nodiscard]] reachability::peer_info decode_peer_info(std::span<const std::uint8_t> bytes,
                                                        reachability::options options) {
   auto out = reachability::peer_info{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (type != detail::wire_type::length_delimited) {
         in.skip(type);
         continue;
      }
      switch (field) {
      case 1:
         out.peer = peer_id::from_bytes(in.bytes());
         break;
      case 2:
         out.endpoints.push_back(endpoint_from_bytes(in.bytes()));
         if (out.endpoints.size() > options.max_endpoints) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT peer has too many addresses");
         }
         break;
      default:
         in.skip(type);
         break;
      }
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_v1_dial(const reachability::peer_info& value,
                                                        reachability::options options) {
   auto out = std::vector<std::uint8_t>{};
   detail::append_bytes(out, 1, encode_peer_info(value, options));
   return out;
}

[[nodiscard]] std::optional<reachability::peer_info> decode_v1_dial(std::span<const std::uint8_t> bytes,
                                                                      reachability::options options) {
   auto out = std::optional<reachability::peer_info>{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (field == 1) {
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT dial peer must be bytes");
         }
         out = decode_peer_info(in.bytes(), options);
      } else {
         in.skip(type);
      }
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_dial_response(const reachability::dial_response& value) {
   auto out = std::vector<std::uint8_t>{};
   // Rust's proto2 decoder distinguishes an explicit OK from an absent status.
   detail::append_uint64(out, 1, static_cast<std::uint16_t>(value.status));
   if (!value.status_text.empty()) {
      detail::append_string(out, 2, value.status_text);
   }
   if (value.endpoint) {
      const auto encoded = endpoint_bytes(*value.endpoint);
      detail::append_bytes(out, 3, encoded);
   }
   return out;
}

[[nodiscard]] reachability::dial_response decode_dial_response(std::span<const std::uint8_t> bytes) {
   auto out = reachability::dial_response{.status = reachability::dial_status::ok};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT status must be varint");
         }
         out.status = static_cast<reachability::dial_status>(in.read_varint());
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT status text must be bytes");
         }
         out.status_text = in.string();
         break;
      case 3:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT observed address must be bytes");
         }
         out.endpoint = endpoint_from_bytes(in.bytes());
         break;
      default:
         in.skip(type);
         break;
      }
   }
   return out;
}

[[nodiscard]] reachability::message::message_kind checked_kind(std::uint64_t value) {
   if (value == static_cast<std::uint16_t>(reachability::message::message_kind::dial)) {
      return reachability::message::message_kind::dial;
   }
   if (value == static_cast<std::uint16_t>(reachability::message::message_kind::dial_response)) {
      return reachability::message::message_kind::dial_response;
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "unknown AutoNAT v1 message type");
}

[[nodiscard]] reachability::v2::dial_status checked_v2_dial_status(std::uint64_t value) {
   switch (static_cast<reachability::v2::dial_status>(value)) {
   case reachability::v2::dial_status::unused:
   case reachability::v2::dial_status::dial_error:
   case reachability::v2::dial_status::dial_back_error:
   case reachability::v2::dial_status::ok:
      return static_cast<reachability::v2::dial_status>(value);
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "unknown AutoNAT v2 dial status");
}

[[nodiscard]] reachability::v2::response_status checked_v2_response_status(std::uint64_t value) {
   switch (static_cast<reachability::v2::response_status>(value)) {
   case reachability::v2::response_status::internal_error:
   case reachability::v2::response_status::request_rejected:
   case reachability::v2::response_status::dial_refused:
   case reachability::v2::response_status::ok:
      return static_cast<reachability::v2::response_status>(value);
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "unknown AutoNAT v2 response status");
}

[[nodiscard]] reachability::v2::dial_back_status checked_v2_dial_back_status(std::uint64_t value) {
   if (value == static_cast<std::uint16_t>(reachability::v2::dial_back_status::ok)) {
      return reachability::v2::dial_back_status::ok;
   }
   FORGE_THROW_EXCEPTION(exceptions::codec_error, "unknown AutoNAT v2 dial-back status");
}

[[nodiscard]] reachability::v2::dial_request decode_v2_dial_request(std::span<const std::uint8_t> bytes,
                                                                    reachability::options options) {
   auto out = reachability::v2::dial_request{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 address must be bytes");
         }
         out.endpoints.push_back(endpoint_from_bytes(in.bytes()));
         if (out.endpoints.size() > options.max_endpoints) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 request has too many addresses");
         }
         break;
      case 2:
         if (type != detail::wire_type::fixed64) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 nonce must be fixed64");
         }
         out.nonce = in.fixed64();
         break;
      default:
         in.skip(type);
         break;
      }
   }
   if (out.nonce == 0 || out.endpoints.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 request missing nonce or address");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_v2_dial_request(const reachability::v2::dial_request& value,
                                                               reachability::options options) {
   if (value.nonce == 0 || value.endpoints.empty() || value.endpoints.size() > options.max_endpoints) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid AutoNAT v2 dial request");
   }
   auto out = std::vector<std::uint8_t>{};
   for (const auto& endpoint : value.endpoints) {
      detail::append_bytes(out, 1, endpoint_bytes(endpoint));
   }
   detail::append_fixed64(out, 2, value.nonce);
   return out;
}

[[nodiscard]] reachability::v2::dial_response decode_v2_dial_response(std::span<const std::uint8_t> bytes) {
   auto out = reachability::v2::dial_response{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 response status must be varint");
         }
         out.status = checked_v2_response_status(in.read_varint());
         break;
      case 2:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 response address index must be varint");
         }
         out.index = static_cast<std::uint32_t>(in.read_varint());
         break;
      case 3:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 dial status must be varint");
         }
         out.dial_status = checked_v2_dial_status(in.read_varint());
         break;
      default:
         in.skip(type);
         break;
      }
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_v2_dial_response(const reachability::v2::dial_response& value) {
   auto out = std::vector<std::uint8_t>{};
   if (value.status != reachability::v2::response_status::internal_error) {
      detail::append_uint64(out, 1, static_cast<std::uint16_t>(value.status));
   }
   if (value.index != 0) {
      detail::append_uint64(out, 2, value.index);
   }
   if (value.dial_status != reachability::v2::dial_status::unused) {
      detail::append_uint64(out, 3, static_cast<std::uint16_t>(value.dial_status));
   }
   return out;
}

[[nodiscard]] reachability::v2::dial_data_request decode_v2_dial_data_request(std::span<const std::uint8_t> bytes) {
   auto out = reachability::v2::dial_data_request{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (type != detail::wire_type::varint) {
         in.skip(type);
         continue;
      }
      if (field == 1) {
         out.index = static_cast<std::uint32_t>(in.read_varint());
      } else if (field == 2) {
         out.bytes = in.read_varint();
      } else {
         in.skip(type);
      }
   }
   if (out.bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 data request missing byte count");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_v2_dial_data_request(const reachability::v2::dial_data_request& value) {
   if (value.bytes == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid AutoNAT v2 data request");
   }
   auto out = std::vector<std::uint8_t>{};
   detail::append_uint64(out, 1, value.index);
   detail::append_uint64(out, 2, value.bytes);
   return out;
}

[[nodiscard]] reachability::v2::dial_data_response decode_v2_dial_data_response(std::span<const std::uint8_t> bytes,
                                                                                reachability::options options) {
   auto out = reachability::v2::dial_data_response{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (field == 1) {
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 data response must be bytes");
         }
         out.data = in.bytes();
      } else {
         in.skip(type);
      }
   }
   if (out.data.empty() || out.data.size() > options.max_data_response_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 data response has invalid data size");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> encode_v2_dial_data_response(const reachability::v2::dial_data_response& value,
                                                                     reachability::options options) {
   if (value.data.empty() || value.data.size() > options.max_data_response_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid AutoNAT v2 data response");
   }
   auto out = std::vector<std::uint8_t>{};
   detail::append_bytes(out, 1, value.data);
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> make_v1_payload(const reachability::message& value,
                                                         reachability::options options) {
   auto out = std::vector<std::uint8_t>{};
   detail::append_uint64(out, 1, static_cast<std::uint16_t>(value.kind));
   if (value.peer) {
      const auto encoded = encode_v1_dial(*value.peer, options);
      detail::append_bytes(out, 2, encoded);
   }
   if (value.response) {
      const auto encoded = encode_dial_response(*value.response);
      detail::append_bytes(out, 3, encoded);
   }
   return out;
}

[[nodiscard]] reachability::message read_v1_payload(std::span<const std::uint8_t> bytes,
                                                     reachability::options options) {
   auto out = reachability::message{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      switch (field) {
      case 1:
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT message type must be varint");
         }
         out.kind = checked_kind(in.read_varint());
         break;
      case 2:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT dial peer must be bytes");
         }
         out.peer = decode_v1_dial(in.bytes(), options);
         break;
      case 3:
         if (type != detail::wire_type::length_delimited) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT dial response must be bytes");
         }
         out.response = decode_dial_response(in.bytes());
         break;
      default:
         in.skip(type);
         break;
      }
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> make_v2_payload(const reachability::v2::message& value,
                                                        reachability::options options) {
   auto out = std::vector<std::uint8_t>{};
   switch (value.type) {
   case reachability::v2::message::kind::dial_request:
      if (!value.dial_request) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT v2 message missing dial request");
      }
      detail::append_bytes(out, 1, encode_v2_dial_request(*value.dial_request, options));
      break;
   case reachability::v2::message::kind::dial_response:
      if (!value.dial_response) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT v2 message missing dial response");
      }
      detail::append_bytes(out, 2, encode_v2_dial_response(*value.dial_response));
      break;
   case reachability::v2::message::kind::dial_data_request:
      if (!value.dial_data_request) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT v2 message missing dial data request");
      }
      detail::append_bytes(out, 3, encode_v2_dial_data_request(*value.dial_data_request));
      break;
   case reachability::v2::message::kind::dial_data_response:
      if (!value.dial_data_response) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT v2 message missing dial data response");
      }
      detail::append_bytes(out, 4, encode_v2_dial_data_response(*value.dial_data_response, options));
      break;
   }
   return out;
}

[[nodiscard]] reachability::v2::message read_v2_payload(std::span<const std::uint8_t> bytes,
                                                        reachability::options options) {
   auto out = reachability::v2::message{};
   auto seen = 0U;
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (type != detail::wire_type::length_delimited) {
         in.skip(type);
         continue;
      }
      ++seen;
      switch (field) {
      case 1:
         out.type = reachability::v2::message::kind::dial_request;
         out.dial_request = decode_v2_dial_request(in.bytes(), options);
         break;
      case 2:
         out.type = reachability::v2::message::kind::dial_response;
         out.dial_response = decode_v2_dial_response(in.bytes());
         break;
      case 3:
         out.type = reachability::v2::message::kind::dial_data_request;
         out.dial_data_request = decode_v2_dial_data_request(in.bytes());
         break;
      case 4:
         out.type = reachability::v2::message::kind::dial_data_response;
         out.dial_data_response = decode_v2_dial_data_response(in.bytes(), options);
         break;
      default:
         in.skip(type);
         --seen;
         break;
      }
   }
   if (seen != 1U) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 message must contain exactly one payload");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> make_v2_dial_back_payload(const reachability::v2::dial_back& value) {
   if (value.nonce == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT v2 dial-back nonce is required");
   }
   auto out = std::vector<std::uint8_t>{};
   detail::append_fixed64(out, 1, value.nonce);
   return out;
}

[[nodiscard]] reachability::v2::dial_back read_v2_dial_back_payload(std::span<const std::uint8_t> bytes) {
   auto out = reachability::v2::dial_back{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (field == 1) {
         if (type != detail::wire_type::fixed64) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 dial-back nonce must be fixed64");
         }
         out.nonce = in.fixed64();
      } else {
         in.skip(type);
      }
   }
   if (out.nonce == 0) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 dial-back missing nonce");
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t>
make_v2_dial_back_response_payload(const reachability::v2::dial_back_response& value) {
   auto out = std::vector<std::uint8_t>{};
   if (value.status != reachability::v2::dial_back_status::ok) {
      detail::append_uint64(out, 1, static_cast<std::uint16_t>(value.status));
   }
   return out;
}

[[nodiscard]] reachability::v2::dial_back_response read_v2_dial_back_response_payload(std::span<const std::uint8_t> bytes) {
   auto out = reachability::v2::dial_back_response{};
   auto in = detail::reader{bytes};
   while (!in.done()) {
      const auto [field, type] = in.key();
      if (field == 1) {
         if (type != detail::wire_type::varint) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "AutoNAT v2 dial-back response status must be varint");
         }
         out.status = checked_v2_dial_back_status(in.read_varint());
      } else {
         in.skip(type);
      }
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> wrap_bounded_message(std::vector<std::uint8_t> payload,
                                                              reachability::options options) {
   if (payload.size() > options.max_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT message exceeds configured size limit");
   }
   return detail::wrap_message(payload);
}

} // namespace

std::vector<std::uint8_t> reachability::codec::encode_v1(const reachability::message& value) {
   return encode_v1(value, reachability::options{});
}

std::vector<std::uint8_t> reachability::codec::encode_v1(const reachability::message& value,
                                                         reachability::options opts) {
   return wrap_bounded_message(make_v1_payload(value, opts), opts);
}

reachability::message reachability::codec::decode_v1(std::span<const std::uint8_t> bytes) {
   return decode_v1(bytes, reachability::options{});
}

reachability::message reachability::codec::decode_v1(std::span<const std::uint8_t> bytes,
                                                     reachability::options opts) {
   return read_v1_payload(detail::unwrap_message(bytes, opts.max_message_size), opts);
}

std::vector<std::uint8_t> reachability::codec::encode_v2(const reachability::v2::message& value) {
   return encode_v2(value, reachability::options{});
}

std::vector<std::uint8_t> reachability::codec::encode_v2(const reachability::v2::message& value,
                                                         reachability::options opts) {
   return wrap_bounded_message(make_v2_payload(value, opts), opts);
}

reachability::v2::message reachability::codec::decode_v2(std::span<const std::uint8_t> bytes) {
   return decode_v2(bytes, reachability::options{});
}

reachability::v2::message reachability::codec::decode_v2(std::span<const std::uint8_t> bytes,
                                                         reachability::options opts) {
   return read_v2_payload(detail::unwrap_message(bytes, opts.max_message_size), opts);
}

std::vector<std::uint8_t> reachability::codec::encode_v2_dial_back(const reachability::v2::dial_back& value) {
   return detail::wrap_message(make_v2_dial_back_payload(value));
}

reachability::v2::dial_back reachability::codec::decode_v2_dial_back(std::span<const std::uint8_t> bytes) {
   return decode_v2_dial_back(bytes, reachability::options{});
}

reachability::v2::dial_back reachability::codec::decode_v2_dial_back(std::span<const std::uint8_t> bytes,
                                                                     reachability::options opts) {
   return read_v2_dial_back_payload(detail::unwrap_message(bytes, opts.max_message_size));
}

std::vector<std::uint8_t>
reachability::codec::encode_v2_dial_back_response(const reachability::v2::dial_back_response& value) {
   return detail::wrap_message(make_v2_dial_back_response_payload(value));
}

reachability::v2::dial_back_response
reachability::codec::decode_v2_dial_back_response(std::span<const std::uint8_t> bytes) {
   return decode_v2_dial_back_response(bytes, reachability::options{});
}

reachability::v2::dial_back_response
reachability::codec::decode_v2_dial_back_response(std::span<const std::uint8_t> bytes,
                                                  reachability::options opts) {
   return read_v2_dial_back_response_payload(detail::unwrap_message(bytes, opts.max_message_size));
}

} // namespace forge::net::p2p
