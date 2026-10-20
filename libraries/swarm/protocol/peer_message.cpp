module;

#include <algorithm>
#include <cstdint>
#include <span>
#include <type_traits>
#include <variant>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.protocol.peer_message;

import forge.raw.raw;
import forge.raw.exceptions;

namespace forge::swarm::protocol {
namespace {

void validate_id(std::uint64_t id) {
   if (id == 0U)
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "request identity must be nonzero");
}

void validate_piece(core::piece_id piece) {
   if (piece.file >= core::max_files || piece.index >= core::max_piece_count) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "piece exceeds descriptor geometry limits");
   }
}

void validate_range(std::uint32_t offset, std::uint32_t length) {
   if (offset >= core::max_piece_size || offset % core::block_size != 0U || length == 0U || length > core::block_size ||
       length > core::max_piece_size - offset) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "block offset or length exceeds transfer geometry");
   }
}

void validate_range(const core::descriptor& manifest, core::piece_id piece, std::uint32_t offset,
                    std::uint32_t length) {
   const auto size = core::piece_length(manifest, piece);
   if (offset >= size || length != std::min(core::block_size, size - offset)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "block must be a complete aligned block or exact tail");
   }
}

} // namespace

bool hash_response::operator==(const hash_response& other) const {
   return id == other.id && piece == other.piece && hash == other.hash && proof.siblings == other.proof.siblings;
}

void validate(const peer_message& value) {
   if (value.body.valueless_by_exception()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "peer message has no active alternative");
   }
   std::visit(
       [](const auto& item) {
          using type = std::remove_cvref_t<decltype(item)>;
          if constexpr (std::is_same_v<type, handshake>) {
             if (item.version != 1U) {
                FORGE_THROW_EXCEPTION(exceptions::unsupported_version, "unsupported Swarm handshake version");
             }
             validate_realm(item.realm);
          } else if constexpr (std::is_same_v<type, inventory>) {
             if (item.pieces.size() > max_bitmap_size) {
                FORGE_THROW_EXCEPTION(exceptions::invalid_message, "inventory exceeds its bitmap limit");
             }
          } else if constexpr (std::is_same_v<type, have>) {
             validate_piece(item.piece);
          } else if constexpr (std::is_same_v<type, interest> || std::is_same_v<type, choke>) {
             // bool's canonical byte is enforced by Raw decoding.
          } else {
             validate_id(item.id);
             if constexpr (std::is_same_v<type, request>) {
                validate_piece(item.piece);
                validate_range(item.offset, item.length);
             } else if constexpr (std::is_same_v<type, block>) {
                validate_piece(item.piece);
                if (item.data.empty() || item.data.size() > core::block_size) {
                   FORGE_THROW_EXCEPTION(exceptions::invalid_message, "block payload exceeds its byte limit");
                }
                validate_range(item.offset, static_cast<std::uint32_t>(item.data.size()));
             } else if constexpr (std::is_same_v<type, reject>) {
                switch (item.reason) {
                case rejection::unavailable:
                case rejection::choked:
                case rejection::invalid_request:
                case rejection::canceled:
                   break;
                default:
                   FORGE_THROW_EXCEPTION(exceptions::invalid_message, "unknown request rejection code");
                }
             } else if constexpr (std::is_same_v<type, hash_request> || std::is_same_v<type, hash_response>) {
                validate_piece(item.piece);
                if constexpr (std::is_same_v<type, hash_response>) {
                   if (item.proof.siblings.size() > max_proof_siblings) {
                      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "piece proof exceeds its sibling limit");
                   }
                }
             }
          }
       },
       value.body);
}

void validate(const peer_message& value, const core::descriptor& manifest) {
   validate(value);
   try {
      core::validate_piece_size(manifest.piece_size);
      if (manifest.files.empty() || manifest.files.size() > core::max_files) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_message, "manifest file geometry is invalid");
      }
      std::visit(
          [&](const auto& item) {
             using type = std::remove_cvref_t<decltype(item)>;
             if constexpr (std::is_same_v<type, handshake>) {
                if (item.swarm != core::calculate_swarm_id(manifest)) {
                   FORGE_THROW_EXCEPTION(exceptions::invalid_message, "handshake is bound to another descriptor");
                }
             } else if constexpr (std::is_same_v<type, inventory>) {
                auto count = 0U;
                for (const auto& file : manifest.files) {
                   const auto pieces = core::piece_count(file.size, manifest.piece_size);
                   if (pieces > core::max_piece_count - count) {
                      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "manifest piece count exceeds bitmap limit");
                   }
                   count += pieces;
                }
                if (item.pieces.size() != (count + 7U) / 8U ||
                    (count % 8U != 0U && (item.pieces.back() >> (count % 8U)) != 0U)) {
                   FORGE_THROW_EXCEPTION(exceptions::invalid_message,
                                         "inventory must match exact little-bit-first geometry");
                }
             } else if constexpr (std::is_same_v<type, request>) {
                validate_range(manifest, item.piece, item.offset, item.length);
             } else if constexpr (std::is_same_v<type, block>) {
                validate_range(manifest, item.piece, item.offset, static_cast<std::uint32_t>(item.data.size()));
             } else if constexpr (std::is_same_v<type, have> || std::is_same_v<type, hash_request> ||
                                  std::is_same_v<type, hash_response>) {
                static_cast<void>(core::piece_length(manifest, item.piece));
                if constexpr (std::is_same_v<type, hash_response>) {
                   const auto& file = manifest.files[item.piece.file];
                   if (!core::verify_piece_hash(item.hash, file.size, manifest.piece_size, item.piece.index, item.proof,
                                                file.root)) {
                      FORGE_THROW_EXCEPTION(exceptions::invalid_message,
                                            "piece hash proof is not bound to the expected root");
                   }
                }
             }
          },
          value.body);
   } catch (const forge::exceptions::context_error& error) {
      if (error.code().category() !=
          core::exceptions::make_error_code(core::exceptions::code::invalid_descriptor).category())
         throw;
      FORGE_THROW_EXCEPTION(exceptions::invalid_message, "message is outside expected descriptor geometry");
   }
}

std::vector<std::uint8_t> encode_message(const peer_message& value) {
   validate(value);
   return forge::raw::pack(value);
}

peer_message decode_message(std::span<const std::uint8_t> bytes, forge::raw::unpack_limits limits) {
   if (bytes.size() > max_wire_message_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "peer message frame exceeds its byte limit");
   }
   limits.max_bytes = std::min({limits.max_bytes, max_bitmap_size, static_cast<std::uint32_t>(bytes.size())});
   limits.max_container_elements = std::min(limits.max_container_elements, max_proof_siblings);
   limits.max_total_container_elements = std::min(limits.max_total_container_elements, max_proof_siblings);
   limits.first_container_elements = std::min(limits.first_container_elements, max_proof_siblings);
   try {
      auto value = forge::raw::unpack_exact<peer_message>(bytes, limits);
      validate(value);
      const auto canonical = forge::raw::pack(value);
      if (!std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "peer message encoding is not canonical");
      }
      return value;
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "peer message frame is truncated");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "peer message frame is malformed");
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "peer message exceeds decode allocation bounds");
   }
}

} // namespace forge::swarm::protocol
