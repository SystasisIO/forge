module;

#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <variant>

module forge.net.p2p.identity;

import forge.codec.base58;
import forge.net.p2p.exceptions;
import forge.crypto.asymmetric;
import forge.crypto.pki.x509;
import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;

#include "details/identity_signature.hxx"

namespace forge::net::p2p {

void to_variant(const peer_id& value, forge::variant& result) {
   result = std::get<std::string>(forge::schema::value_adapter<peer_id>::encode(value).storage);
}

void from_variant(const forge::variant& value, peer_id& result) {
   if (!value.is_string()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Peer ID requires a string");
   }
   result = forge::schema::value_adapter<peer_id>::decode(forge::schema::input_value{value.as_string()});
}

namespace {

void append_varint(forge::multiformats::bytes& out, std::uint64_t value) {
   auto encoded = forge::multiformats::varint_encode(value);
   out.insert(out.end(), encoded.begin(), encoded.end());
}

[[nodiscard]] peer_id peer_id_from_multihash(const forge::multiformats::multihash& hash) {
   return peer_id::from_bytes(hash.encode());
}

[[nodiscard]] bool has_cid_v1_prefix(std::string_view value) noexcept {
   return !value.empty() && (value.front() == 'b' || value.front() == 'B');
}

[[nodiscard]] std::size_t read_der_length(std::span<const std::uint8_t> bytes, std::size_t& offset) {
   if (offset >= bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension is truncated");
   }
   const auto first = bytes[offset++];
   if ((first & 0x80U) == 0) {
      return first;
   }
   const auto count = static_cast<std::size_t>(first & 0x7fU);
   if (count == 0 || count > sizeof(std::size_t) || count > bytes.size() - offset) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "invalid libp2p certificate extension length");
   }
   auto out = std::size_t{};
   for (auto i = std::size_t{}; i < count; ++i) {
      out = (out << 8U) | bytes[offset++];
   }
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> read_der_octet(std::span<const std::uint8_t> bytes, std::size_t& offset) {
   if (offset >= bytes.size() || bytes[offset++] != 0x04) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension expected octet string");
   }
   const auto length = read_der_length(bytes, offset);
   if (length > bytes.size() - offset) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension octet string is truncated");
   }
   const auto begin = bytes.begin() + static_cast<std::ptrdiff_t>(offset);
   const auto end = begin + static_cast<std::ptrdiff_t>(length);
   auto out = std::vector<std::uint8_t>{begin, end};
   offset += length;
   return out;
}

struct signed_key_extension {
   std::vector<std::uint8_t> public_key;
   std::vector<std::uint8_t> signature;
};

[[nodiscard]] signed_key_extension decode_signed_key_extension(std::span<const std::uint8_t> bytes) {
   auto offset = std::size_t{};
   if (bytes.empty() || bytes[offset++] != 0x30) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension expected sequence");
   }
   const auto length = read_der_length(bytes, offset);
   if (length != bytes.size() - offset) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension sequence length mismatch");
   }
   auto out = signed_key_extension{};
   out.public_key = read_der_octet(bytes, offset);
   out.signature = read_der_octet(bytes, offset);
   if (offset != bytes.size() || out.public_key.empty() || out.signature.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension is incomplete");
   }
   return out;
}

void verify_libp2p_certificate_extension(const forge::crypto::pki::x509::certificate& certificate,
                                         const signed_key_extension& extension) {
   const auto public_key = decode_public_key(extension.public_key);
   const auto spki = certificate.public_key_der();
   auto message = std::vector<std::uint8_t>{};
   constexpr auto prefix = std::string_view{"libp2p-tls-handshake:"};
   message.insert(message.end(), prefix.begin(), prefix.end());
   message.insert(message.end(), spki.begin(), spki.end());

   if (!verify_identity_signature(public_key, message, extension.signature)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p certificate extension signature is invalid");
   }
}

[[nodiscard]] peer_id
peer_id_from_libp2p_certificate_extension(const forge::crypto::pki::x509::certificate& certificate) {
   const auto value = certificate.extension("1.3.6.1.4.1.53594.1.1");
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity,
                            "libp2p peer certificate is missing signed identity extension");
   }
   const auto extension = decode_signed_key_extension(value);
   verify_libp2p_certificate_extension(certificate, extension);
   return make_peer_id(decode_public_key(extension.public_key));
}

} // namespace

std::string peer_id::to_string() const {
   return value;
}

std::string peer_id::to_cid_string() const {
   auto cid = forge::multiformats::varint_encode(1);
   append_varint(cid, forge::multiformats::code_value(forge::multiformats::multicodec_code::libp2p_key));
   auto multihash = to_bytes();
   cid.insert(cid.end(), multihash.begin(), multihash.end());
   return forge::multiformats::multibase_encode(forge::multiformats::multibase_code::base32, cid);
}

forge::multiformats::bytes peer_id::to_bytes() const {
   return forge::codec::base58::decode(value);
}

peer_id peer_id::from_string(std::string_view value) {
   if (has_cid_v1_prefix(value)) {
      auto decoded = forge::multiformats::multibase_decode(value);
      auto span = std::span<const std::uint8_t>{decoded.bytes};
      auto version = forge::multiformats::varint_decode(span);
      if (version.value != 1) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Peer ID CID uses an unsupported CID version");
      }
      std::size_t consumed = 0;
      const auto codec = forge::multiformats::multicodec_decode(span.subspan(version.size), consumed);
      if (codec != forge::multiformats::multicodec_code::libp2p_key) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Peer ID CID is not a libp2p-key CID");
      }
      auto multihash = forge::multiformats::multihash::decode(span.subspan(version.size + consumed));
      return from_bytes(multihash.encode());
   }

   auto id = peer_id{.value = std::string{value}};
   if (!valid_peer_id(id)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "Peer ID string is not a valid libp2p multihash");
   }
   return id;
}

peer_id peer_id::from_bytes(std::span<const std::uint8_t> value) {
   (void)forge::multiformats::multihash::decode(value);
   return peer_id{.value = forge::codec::base58::encode(value)};
}

forge::multiformats::bytes encode_public_key(const public_key& key) {
   if (key.data.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_identity, "libp2p public key data is empty");
   }

   auto out = forge::multiformats::bytes{};
   append_varint(out, 0x08);
   append_varint(out, static_cast<std::uint8_t>(key.type));
   append_varint(out, 0x12);
   append_varint(out, key.data.size());
   out.insert(out.end(), key.data.begin(), key.data.end());
   return out;
}

public_key decode_public_key(std::span<const std::uint8_t> bytes) {
   auto out = public_key{};
   auto offset = std::size_t{};
   auto saw_type = false;
   auto saw_data = false;
   while (offset < bytes.size()) {
      const auto key = forge::multiformats::varint_decode(bytes.subspan(offset));
      offset += key.size;
      const auto field = static_cast<std::uint32_t>(key.value >> 3U);
      const auto wire = static_cast<std::uint8_t>(key.value & 0x07U);
      switch (field) {
      case 1: {
         if (wire != 0) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p public key type must be varint");
         }
         const auto value = forge::multiformats::varint_decode(bytes.subspan(offset));
         offset += value.size;
         out.type = static_cast<decltype(out.type)>(value.value);
         saw_type = true;
         break;
      }
      case 2: {
         if (wire != 2) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p public key data must be bytes");
         }
         const auto size = forge::multiformats::varint_decode(bytes.subspan(offset));
         offset += size.size;
         if (size.value > bytes.size() - offset) {
            FORGE_THROW_EXCEPTION(exceptions::codec_error, "truncated libp2p public key data");
         }
         out.data = forge::multiformats::bytes{
             bytes.begin() + static_cast<std::ptrdiff_t>(offset),
             bytes.begin() + static_cast<std::ptrdiff_t>(offset + static_cast<std::size_t>(size.value))};
         offset += static_cast<std::size_t>(size.value);
         saw_data = true;
         break;
      }
      default:
         FORGE_THROW_EXCEPTION(exceptions::codec_error, "unsupported libp2p public key field");
      }
   }
   if (!saw_type || !saw_data || out.data.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "libp2p public key is incomplete");
   }
   validate_public_key(out);
   return out;
}

peer_id make_peer_id(const public_key& key) {
   validate_public_key(key);
   auto encoded = encode_public_key(key);
   if (encoded.size() <= 42) {
      return peer_id_from_multihash(forge::multiformats::multihash::identity(encoded));
   }
   return peer_id_from_multihash(forge::multiformats::multihash::sha2_256(encoded));
}

peer_id make_peer_id_from_certificate_pem(std::string_view certificate_pem) {
   auto certificate = forge::crypto::pki::x509::certificate::from_pem(certificate_pem);
   return peer_id_from_libp2p_certificate_extension(certificate);
}

peer_id make_peer_id_from_certificate_der(std::span<const std::uint8_t> der) {
   auto parsed = forge::crypto::pki::x509::certificate::from_der(der);
   return peer_id_from_libp2p_certificate_extension(parsed);
}

bool valid_peer_id(const peer_id& id) noexcept {
   try {
      (void)forge::multiformats::multihash::decode(id.to_bytes());
      return true;
   } catch (...) {
      return false;
   }
}

} // namespace forge::net::p2p

forge::schema::input_value forge::schema::value_adapter<forge::net::p2p::peer_id>::encode(
    const forge::net::p2p::peer_id& input) {
   if (!input.value.empty() && !forge::net::p2p::valid_peer_id(input)) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_identity, "Peer ID is not canonical");
   }
   return forge::schema::input_value{input.to_string()};
}

forge::net::p2p::peer_id forge::schema::value_adapter<forge::net::p2p::peer_id>::decode(
    const forge::schema::input_value& input) {
   const auto* text = std::get_if<std::string>(&input.storage);
   if (!text) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_identity, "Peer ID requires a string");
   }
   return text->empty() ? forge::net::p2p::peer_id{} : forge::net::p2p::peer_id::from_string(*text);
}
