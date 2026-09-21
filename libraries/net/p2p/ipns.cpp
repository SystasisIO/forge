module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.ipns;

import forge.chrono.iso8601;
import forge.chrono.timestamp;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;

#include "details/identity_signature.hxx"
#include "details/ipns_cbor.hxx"
#include "details/ipns_protobuf.hxx"

namespace forge::net::p2p::ipns {
namespace {

constexpr auto signature_v2_prefix = std::string_view{"ipns-signature:"};
[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_invalid_record(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::protocol_error, std::move(message));
}

[[noreturn]] void throw_invalid_key(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_identity, std::move(message));
}

[[nodiscard]] std::string format_eol(forge::chrono::timestamp value) {
   try {
      return forge::chrono::iso8601::format_rfc3339(value);
   } catch (const std::out_of_range&) {
      throw_invalid_options("IPNS EOL is outside RFC3339Nano range");
   }
}

[[nodiscard]] forge::chrono::timestamp parse_eol(std::span<const std::uint8_t> bytes) {
   try {
      return forge::chrono::iso8601::parse_rfc3339_timestamp(
          std::string_view{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
   } catch (const std::invalid_argument& error) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, error.what());
   }
}

[[nodiscard]] std::vector<std::uint8_t> signature_v2_data(std::span<const std::uint8_t> data) {
   auto out = std::vector<std::uint8_t>{signature_v2_prefix.begin(), signature_v2_prefix.end()};
   out.insert(out.end(), data.begin(), data.end());
   return out;
}

[[nodiscard]] std::vector<std::uint8_t> signature_v1_data(std::span<const std::uint8_t> value,
                                                          std::span<const std::uint8_t> validity) {
   auto out = std::vector<std::uint8_t>{value.begin(), value.end()};
   out.insert(out.end(), validity.begin(), validity.end());
   out.push_back('0');
   return out;
}

[[nodiscard]] std::optional<public_key> extract_inline_key(const peer_id& peer) {
   try {
      const auto hash = forge::multiformats::multihash::decode(peer.to_bytes());
      if (hash.code != forge::multiformats::code_value(forge::multiformats::multicodec_code::identity)) {
         return std::nullopt;
      }
      return decode_public_key(hash.digest);
   } catch (const forge::exceptions::base& error) {
      if (exceptions::is(error, exceptions::code::invalid_identity)) {
         throw;
      }
      throw_invalid_key(error.what());
   }
}

[[nodiscard]] public_key validation_key(const std::optional<std::vector<std::uint8_t>>& embedded_key,
                                        const peer_id& expected_peer, const std::optional<public_key>& external_key,
                                        const public_key_resolver* resolver) {
   if (!valid_peer_id(expected_peer)) {
      throw_invalid_key("IPNS validation Peer ID is invalid");
   }

   auto selected = std::optional<public_key>{};
   if (embedded_key && !embedded_key->empty()) {
      try {
         selected = decode_public_key(*embedded_key);
      } catch (const forge::exceptions::base& error) {
         throw_invalid_key(error.what());
      }
   } else {
      selected = extract_inline_key(expected_peer);
      if (!selected && external_key) {
         selected = *external_key;
      }
      if (!selected && resolver) {
         selected = (*resolver)(expected_peer);
      }
      if (!selected) {
         throw_invalid_key("IPNS public key is not embedded, inline, or available from the KeyBook");
      }
   }

   try {
      if (make_peer_id(*selected) != expected_peer) {
         throw_invalid_key("IPNS public key does not match the expected Peer ID");
      }
      if (external_key && !embedded_key && make_peer_id(*external_key) != expected_peer) {
         throw_invalid_key("external IPNS public key does not match the expected Peer ID");
      }
   } catch (const forge::exceptions::base& error) {
      if (exceptions::is(error, exceptions::code::invalid_identity)) {
         throw;
      }
      throw_invalid_key(error.what());
   }
   return *selected;
}

[[nodiscard]] std::span<const std::uint8_t>
optional_bytes(const std::optional<std::vector<std::uint8_t>>& value) noexcept {
   if (!value) {
      return {};
   }
   return *value;
}

} // namespace

std::span<const std::uint8_t> record::value() const noexcept {
   return value_;
}

std::uint64_t record::sequence() const noexcept {
   return sequence_;
}

forge::chrono::timestamp record::eol() const noexcept {
   return eol_;
}

std::string_view record::eol_text() const noexcept {
   return eol_text_;
}

std::chrono::nanoseconds record::ttl() const noexcept {
   return ttl_;
}

validity_type record::validity() const noexcept {
   return validity_;
}

const metadata& record::metadata_values() const noexcept {
   return metadata_;
}

std::optional<public_key> record::embedded_public_key() const {
   if (!public_key_ || public_key_->empty()) {
      return std::nullopt;
   }
   return decode_public_key(*public_key_);
}

bool record::has_v1_compatibility() const noexcept {
   return signature_v1_.has_value() || value_v1_.has_value();
}

bool record::has_v2_signature() const noexcept {
   return signature_v2_.has_value();
}

std::span<const std::uint8_t> record::signature_v1() const noexcept {
   return optional_bytes(signature_v1_);
}

std::span<const std::uint8_t> record::signature_v2() const noexcept {
   return optional_bytes(signature_v2_);
}

std::span<const std::uint8_t> record::data() const noexcept {
   return optional_bytes(data_);
}

std::span<const std::uint8_t> record::encoded() const noexcept {
   return encoded_;
}

record create(const public_key& key, const signing_callback& signer, std::span<const std::uint8_t> value,
              std::uint64_t sequence, forge::chrono::timestamp eol, std::chrono::nanoseconds ttl, create_options options) {
   if (!signer) {
      throw_invalid_options("IPNS signing callback is empty");
   }
   if (ttl.count() < 0) {
      throw_invalid_options("IPNS TTL must not be negative");
   }
   if (value.size() > max_record_size) {
      throw_invalid_options("IPNS value exceeds the record size limit");
   }
   try {
      static_cast<void>(crypto_public_key(key));
      static_cast<void>(make_peer_id(key));
   } catch (const forge::exceptions::base& error) {
      throw_invalid_options(error.what());
   }

   const auto validity = format_eol(eol);
   auto cbor = detail::ipns_cbor::encode(detail::ipns_cbor::document{
       .value = {value.begin(), value.end()},
       .validity = {validity.begin(), validity.end()},
       .validity_type = static_cast<std::int64_t>(validity_type::eol),
       .sequence = std::bit_cast<std::int64_t>(sequence),
       .ttl = ttl.count(),
       .metadata_values = std::move(options.metadata_values),
   });
   auto signature_v2 = signer(signature_v2_data(cbor));
   if (signature_v2.empty()) {
      throw_invalid_options("IPNS signing callback returned an empty V2 signature");
   }
   if (signature_v2.size() > max_record_size) {
      throw_invalid_options("IPNS signing callback returned an oversized V2 signature");
   }
   if (!verify_identity_signature(key, signature_v2_data(cbor), signature_v2)) {
      throw_invalid_options("IPNS signing callback returned a V2 signature for a different key");
   }

   auto wire = detail::ipns_protobuf::wire_record{
       .signature_v2 = std::move(signature_v2),
       .data = std::move(cbor),
   };
   if (options.v1_compatibility) {
      wire.value = std::vector<std::uint8_t>{value.begin(), value.end()};
      wire.validity_type = static_cast<std::uint64_t>(validity_type::eol);
      wire.validity = std::vector<std::uint8_t>{validity.begin(), validity.end()};
      wire.sequence = sequence;
      wire.ttl = static_cast<std::uint64_t>(ttl.count());
      wire.signature_v1 = signer(signature_v1_data(*wire.value, *wire.validity));
      if (wire.signature_v1->empty()) {
         throw_invalid_options("IPNS signing callback returned an empty V1 signature");
      }
      if (wire.signature_v1->size() > max_record_size) {
         throw_invalid_options("IPNS signing callback returned an oversized V1 signature");
      }
      if (!verify_identity_signature(key, signature_v1_data(*wire.value, *wire.validity), *wire.signature_v1)) {
         throw_invalid_options("IPNS signing callback returned a V1 signature for a different key");
      }
   }

   auto embed = options.embed_public_key.value_or(false);
   if (!options.embed_public_key) {
      const auto hash = forge::multiformats::multihash::decode(make_peer_id(key).to_bytes());
      embed = hash.code != forge::multiformats::code_value(forge::multiformats::multicodec_code::identity);
   }
   if (embed) {
      wire.public_key = encode_public_key(key);
   }

   auto encoded = detail::ipns_protobuf::encode(wire);
   if (encoded.size() > max_record_size) {
      throw_invalid_options("IPNS record exceeds 10 KiB");
   }
   return decode(encoded);
}

record decode(std::span<const std::uint8_t> bytes) {
   if (bytes.size() > max_record_size) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "IPNS record exceeds 10 KiB");
   }
   auto wire = detail::ipns_protobuf::decode(bytes);
   if (!wire.data || wire.data->empty()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "IPNS record is missing DAG-CBOR Data");
   }
   auto document = detail::ipns_cbor::decode(*wire.data);
   auto out = record{};
   out.encoded_ = {bytes.begin(), bytes.end()};
   out.value_v1_ = std::move(wire.value);
   out.signature_v1_ = std::move(wire.signature_v1);
   out.validity_type_v1_ = wire.validity_type;
   out.validity_v1_ = std::move(wire.validity);
   out.sequence_v1_ = wire.sequence;
   out.ttl_v1_ = wire.ttl;
   out.public_key_ = std::move(wire.public_key);
   out.signature_v2_ = std::move(wire.signature_v2);
   out.data_ = std::move(wire.data);
   out.unknown_protobuf_fields_ = std::move(wire.unknown_fields);
   out.value_ = std::move(document.value);
   out.sequence_ = std::bit_cast<std::uint64_t>(document.sequence);
   out.eol_text_ = {document.validity.begin(), document.validity.end()};
   out.eol_ = parse_eol(document.validity);
   out.ttl_ = std::chrono::nanoseconds{document.ttl};
   out.validity_ = static_cast<validity_type>(document.validity_type);
   out.metadata_ = std::move(document.metadata_values);
   return out;
}

std::vector<std::uint8_t> encode(const record& value) {
   return value.encoded_;
}

void validate(const record& value, const peer_id& expected_peer, std::optional<public_key> external_key,
              forge::chrono::timestamp now) {
   const auto resolver = [external_key = std::move(external_key)](const peer_id&) { return external_key; };
   validate(value, expected_peer, public_key_resolver{resolver}, now);
}

void validate(const record& value, const peer_id& expected_peer, const public_key_resolver& resolver, forge::chrono::timestamp now) {
   if (!resolver) {
      throw_invalid_options("IPNS public key resolver is empty");
   }
   if (value.encoded_.size() > max_record_size) {
      throw_invalid_record("IPNS record exceeds 10 KiB");
   }
   if (!value.signature_v2_ || value.signature_v2_->empty() || !value.data_ || value.data_->empty()) {
      throw_invalid_record("IPNS record requires SignatureV2 and Data");
   }
   const auto key = validation_key(value.public_key_, expected_peer, std::nullopt, &resolver);
   if (!verify_identity_signature(key, signature_v2_data(*value.data_), *value.signature_v2_)) {
      throw_invalid_key("IPNS SignatureV2 verification failed");
   }

   const auto has_legacy_data =
       (value.signature_v1_ && !value.signature_v1_->empty()) || (value.value_v1_ && !value.value_v1_->empty());
   if (has_legacy_data) {
      const auto empty = std::vector<std::uint8_t>{};
      const auto& legacy_value = value.value_v1_ ? *value.value_v1_ : empty;
      const auto& legacy_validity = value.validity_v1_ ? *value.validity_v1_ : empty;
      if (legacy_value != value.value_ ||
          legacy_validity != std::vector<std::uint8_t>{value.eol_text_.begin(), value.eol_text_.end()} ||
          value.validity_type_v1_.value_or(0) != static_cast<std::uint64_t>(value.validity_) ||
          value.sequence_v1_.value_or(0) != value.sequence_ ||
          value.ttl_v1_.value_or(0) != static_cast<std::uint64_t>(value.ttl_.count())) {
         throw_invalid_record("IPNS V1 protobuf fields do not match signed DAG-CBOR Data");
      }
   }
   if (now > value.eol_) {
      throw_invalid_record("IPNS record is expired");
   }
}

std::size_t select(std::span<const record> candidates) {
   if (candidates.empty()) {
      throw_invalid_options("IPNS selector requires at least one record");
   }
   auto best = std::size_t{};
   for (auto index = std::size_t{1}; index < candidates.size(); ++index) {
      const auto& current = candidates[best];
      const auto& candidate = candidates[index];
      auto newer = false;
      if (current.signature_v2_.has_value() != candidate.signature_v2_.has_value()) {
         newer = candidate.signature_v2_.has_value();
      } else if (current.sequence_ != candidate.sequence_) {
         newer = candidate.sequence_ > current.sequence_;
      } else if (current.eol_ != candidate.eol_) {
         newer = candidate.eol_ > current.eol_;
      } else {
         newer = std::lexicographical_compare(current.encoded_.begin(), current.encoded_.end(),
                                              candidate.encoded_.begin(), candidate.encoded_.end());
      }
      if (newer) {
         best = index;
      }
   }
   return best;
}

std::vector<std::uint8_t> routing_key(const peer_id& peer) {
   if (!valid_peer_id(peer)) {
      throw_invalid_key("cannot derive an IPNS routing key from an invalid Peer ID");
   }
   auto out = std::vector<std::uint8_t>{routing_prefix.begin(), routing_prefix.end()};
   const auto bytes = peer.to_bytes();
   out.insert(out.end(), bytes.begin(), bytes.end());
   return out;
}

} // namespace forge::net::p2p::ipns
