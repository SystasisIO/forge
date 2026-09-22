#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

// Ordinary private C++ component, not attached to a public module.
namespace forge::net::p2p::detail::mdns_codec {

using bytes = std::vector<std::uint8_t>;
// Raw labels, without case folding, dot splitting or escape interpretation.
// Empty vector means the root. Service names are supplied by the caller.
using name = std::vector<std::string>;

inline constexpr std::uint16_t class_mask = 0x7fff;
inline constexpr std::uint16_t class_high_bit = 0x8000; // QU in questions; cache-flush in records.
inline constexpr std::uint16_t type_a = 1;
inline constexpr std::uint16_t type_ptr = 12;
inline constexpr std::uint16_t type_txt = 16;
inline constexpr std::uint16_t type_aaaa = 28;
inline constexpr std::uint16_t type_srv = 33;

struct limits {
   std::size_t max_packet_size = 8932;
   std::size_t max_questions = 64;
   std::size_t max_records = 256; // Sum of answers, authorities and additionals.
   std::size_t max_records_per_section = 128;
   std::size_t max_pointer_depth = 32; // Hard maximum 128.
   std::size_t max_rdata_size = 8192;
   std::size_t max_txt_attributes = 256; // Packet-wide, including empty attributes.
   std::size_t max_txt_attributes_per_record = 64;
   std::size_t max_txt_bytes = 8192; // Payload bytes, excluding length octets.
   std::size_t max_txt_bytes_per_record = 2048;
};

struct header {
   std::uint16_t id = 0;
   std::uint16_t flags = 0;
   friend bool operator==(const header&, const header&) = default;
};

struct question {
   name owner;
   std::uint16_t type = type_ptr;
   std::uint16_t class_code = 1; // Includes QU; never masked on decode/encode.
   friend bool operator==(const question&, const question&) = default;
};

struct ptr {
   name target;
   friend bool operator==(const ptr&, const ptr&) = default;
};
struct txt {
   std::vector<bytes> attributes;
   friend bool operator==(const txt&, const txt&) = default;
};
struct srv {
   std::uint16_t priority = 0;
   std::uint16_t weight = 0;
   std::uint16_t port = 0;
   name target;
   friend bool operator==(const srv&, const srv&) = default;
};
struct a {
   std::array<std::uint8_t, 4> address{};
   friend bool operator==(const a&, const a&) = default;
};
struct aaaa {
   std::array<std::uint8_t, 16> address{};
   friend bool operator==(const aaaa&, const aaaa&) = default;
};
// Unknown RDATA is bounded and skipped, never copied or interpreted as names.
// It cannot be re-encoded (opaque data may contain packet-relative pointers).
struct unknown {
   std::uint16_t wire_size = 0;
   friend bool operator==(const unknown&, const unknown&) = default;
};
using rdata = std::variant<unknown, ptr, txt, srv, a, aaaa>;

struct record {
   name owner;
   std::uint16_t type = type_ptr;
   std::uint16_t class_code = 1; // Includes cache-flush.
   std::uint32_t ttl = 0;
   rdata data;
   friend bool operator==(const record&, const record&) = default;
};

struct message {
   header head;
   std::vector<question> questions;
   std::vector<record> answers;
   std::vector<record> authorities;
   std::vector<record> additionals;
   friend bool operator==(const message&, const message&) = default;
};

// Malformed wire, invalid models and exceeded limits throw P2P codec_error.
// Decode validates the entire packet before allocating result containers.
// Compression pointers must reference prior packet data outside the DNS header.
// Encode validates/sizes before allocation; emits uncompressed names and derives counts.
[[nodiscard]] message decode(std::span<const std::uint8_t> packet, const limits& bounds = {});
[[nodiscard]] bytes encode(const message& value, const limits& bounds = {});

} // namespace forge::net::p2p::detail::mdns_codec
