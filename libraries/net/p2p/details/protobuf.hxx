#pragma once

#include <forge/exceptions/macros.hpp>

namespace forge::net::p2p::detail {

enum class wire_type : std::uint8_t {
   varint = 0,
   fixed64 = 1,
   length_delimited = 2,
   fixed32 = 5,
};

inline void append_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
   auto encoded = forge::multiformats::varint_encode(value);
   out.insert(out.end(), encoded.begin(), encoded.end());
}

inline void append_key(std::vector<std::uint8_t>& out, std::uint32_t field, wire_type type) {
   append_varint(out, (static_cast<std::uint64_t>(field) << 3U) | static_cast<std::uint8_t>(type));
}

inline void append_uint64(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t value) {
   append_key(out, field, wire_type::varint);
   append_varint(out, value);
}

inline void append_fixed64(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t value) {
   append_key(out, field, wire_type::fixed64);
   for (auto shift = 0; shift != 64; shift += 8) {
      out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
   }
}

inline void append_bytes(std::vector<std::uint8_t>& out, std::uint32_t field, std::span<const std::uint8_t> value) {
   append_key(out, field, wire_type::length_delimited);
   append_varint(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

inline void append_string(std::vector<std::uint8_t>& out, std::uint32_t field, std::string_view value) {
   auto bytes = std::vector<std::uint8_t>{value.begin(), value.end()};
   append_bytes(out, field, bytes);
}

inline std::vector<std::uint8_t> wrap_message(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

class reader {
 public:
   explicit reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

   [[nodiscard]] bool done() const noexcept {
      return offset_ == bytes_.size();
   }

   [[nodiscard]] std::pair<std::uint32_t, wire_type> key() {
      const auto decoded = read_varint();
      const auto field = decoded >> 3U;
      if (field == 0 || field > 0x1fffffffU) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "invalid libp2p protobuf field number");
      }
      const auto type = static_cast<wire_type>(decoded & 0x07U);
      if (type != wire_type::varint && type != wire_type::fixed64 &&
          type != wire_type::length_delimited && type != wire_type::fixed32) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "unsupported libp2p protobuf wire type");
      }
      return {static_cast<std::uint32_t>(field), type};
   }

   [[nodiscard]] std::uint64_t read_varint() {
      try {
         const auto decoded = forge::multiformats::varint_decode(bytes_.subspan(offset_));
         offset_ += decoded.size;
         return decoded.value;
      } catch (const forge::multiformats::exceptions::invalid_format& error) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, error.what());
      }
   }

   [[nodiscard]] std::uint64_t fixed64() {
      require(8);
      auto out = std::uint64_t{0};
      for (auto shift = 0; shift != 64; shift += 8) {
         out |= static_cast<std::uint64_t>(bytes_[offset_++]) << shift;
      }
      return out;
   }

   [[nodiscard]] std::vector<std::uint8_t> bytes() {
      const auto size = read_varint();
      if (size > bytes_.size() - offset_) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "truncated libp2p protobuf bytes field");
      }
      auto out = std::vector<std::uint8_t>{bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
                                           bytes_.begin() + static_cast<std::ptrdiff_t>(offset_ + size)};
      offset_ += static_cast<std::size_t>(size);
      return out;
   }

   [[nodiscard]] std::string string() {
      const auto value = bytes();
      return {value.begin(), value.end()};
   }

   void skip(wire_type type) {
      switch (type) {
      case wire_type::varint:
         (void)read_varint();
         return;
      case wire_type::fixed64:
         (void)fixed64();
         return;
      case wire_type::length_delimited: {
         const auto size = read_varint();
         if (size > bytes_.size() - offset_) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "truncated libp2p protobuf bytes field");
         }
         offset_ += static_cast<std::size_t>(size);
         return;
      }
      case wire_type::fixed32:
         require(4);
         offset_ += 4;
         return;
      }
   }

 private:
   void require(std::size_t size) const {
      if (size > bytes_.size() - offset_) {
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "truncated libp2p protobuf message");
      }
   }

   std::span<const std::uint8_t> bytes_;
   std::size_t offset_ = 0;
};

inline std::vector<std::uint8_t> unwrap_message(std::span<const std::uint8_t> bytes, std::size_t max_payload_size) {
   auto decoded = forge::multiformats::decoded_varint{};
   try {
      decoded = forge::multiformats::varint_decode(bytes);
   } catch (const forge::multiformats::exceptions::invalid_format& error) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, error.what());
   }
   if (decoded.value > max_payload_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p protobuf message exceeds max size");
   }
   const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
   if (total != bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p protobuf message length mismatch");
   }
   return {bytes.begin() + static_cast<std::ptrdiff_t>(decoded.size), bytes.end()};
}

} // namespace forge::net::p2p::detail
