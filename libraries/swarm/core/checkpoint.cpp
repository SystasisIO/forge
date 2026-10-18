module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

module forge.swarm.core.checkpoint;

import forge.raw.raw;
import forge.raw.exceptions;

namespace forge::swarm::core {
namespace {

void validate_bitmap(std::span<const std::uint8_t> bytes, std::uint32_t count) {
   if (bytes.size() != (count + 7U) / 8U ||
       (count % 8U != 0U && (bytes.back() >> (count % 8U)) != 0U)) {
      throw exceptions::invalid_checkpoint{"checkpoint bitmap has an invalid shape"};
   }
}

} // namespace

std::uint32_t piece_ordinal(const descriptor& value, piece_id piece) {
   if (piece.file >= value.files.size() || piece.index >= piece_count(value.files[piece.file].size, value.piece_size)) {
      throw exceptions::invalid_geometry{"piece is outside the descriptor"};
   }
   auto offset = piece.index;
   for (auto i = 0U; i < piece.file; ++i) {
      offset += piece_count(value.files[i].size, value.piece_size);
   }
   return offset;
}

std::uint32_t piece_length(const descriptor& value, piece_id piece) {
   static_cast<void>(piece_ordinal(value, piece));
   return static_cast<std::uint32_t>(std::min<std::uint64_t>(value.piece_size,
      value.files[piece.file].size - std::uint64_t{piece.index} * value.piece_size));
}

bool bitmap_test(std::span<const std::uint8_t> bytes, std::uint32_t index) noexcept {
   return index / 8U < bytes.size() && (bytes[index / 8U] & (1U << (index % 8U))) != 0U;
}

void validate_checkpoint(const resume_state& state, const descriptor& value) {
   const auto summary = validate_descriptor(value);
   auto identity = sha256::encoder{};
   forge::raw::pack(identity, value);
   if (state.version != 1U || state.id.value != identity.result() || state.unfinished.size() > max_unfinished_pieces ||
       state.generation == std::numeric_limits<std::uint64_t>::max()) {
      throw exceptions::invalid_checkpoint{"checkpoint version, identity or unfinished bound is invalid"};
   }
   validate_bitmap(state.verified, summary.total_pieces);
   const unfinished_piece* previous = nullptr;
   for (const auto& item : state.unfinished) {
      if (item.piece.file >= value.files.size() ||
          item.piece.index >= piece_count(value.files[item.piece.file].size, value.piece_size)) {
         throw exceptions::invalid_checkpoint{"unfinished piece is outside the descriptor"};
      }
      if (previous != nullptr && previous->piece >= item.piece) {
         throw exceptions::invalid_checkpoint{"unfinished pieces must be unique and ordered"};
      }
      const auto offset = piece_ordinal(value, item.piece);
      const auto length = piece_length(value, item.piece);
      validate_bitmap(item.blocks, (length + block_size - 1U) / block_size);
      if (bitmap_test(state.verified, offset)) {
         throw exceptions::invalid_checkpoint{"verified piece cannot be unfinished"};
      }
      previous = &item;
   }
   if (state.published) {
      for (auto i = 0U; i < summary.total_pieces; ++i) {
         if (!bitmap_test(state.verified, i)) {
            throw exceptions::invalid_checkpoint{"published checkpoint must be complete"};
         }
      }
   }
}

std::vector<std::uint8_t> encode_checkpoint(const resume_state& state, const descriptor& value) {
   validate_checkpoint(state, value);
   auto encoder = sha256::encoder{};
   forge::raw::pack(encoder, state);
   const auto digest = encoder.result();
   auto bytes = forge::raw::pack(std::pair<const resume_state&, sha256>{state, digest});
   if (bytes.size() > max_checkpoint_size) {
      throw exceptions::invalid_checkpoint{"checkpoint exceeds its byte bound"};
   }
   return bytes;
}

resume_state decode_checkpoint(std::span<const std::uint8_t> bytes, const descriptor& value) {
   if (bytes.size() > max_checkpoint_size) {
      throw exceptions::invalid_checkpoint{"checkpoint exceeds its byte bound"};
   }
   try {
      auto envelope = forge::raw::unpack_exact<std::pair<resume_state, sha256>>(bytes,
         {.max_container_elements = max_piece_count / 8U,
          .max_total_container_elements = max_piece_count / 8U + max_unfinished_pieces * 129U,
          .max_bytes = max_checkpoint_size});
      auto& state = envelope.first;
      validate_checkpoint(state, value);
      auto encoder = sha256::encoder{};
      forge::raw::pack(encoder, state);
      if (encoder.result() != envelope.second) {
         throw exceptions::invalid_checkpoint{"checkpoint checksum does not match"};
      }
      const auto canonical = forge::raw::pack(envelope);
      if (!std::equal(canonical.begin(), canonical.end(), bytes.begin(), bytes.end())) {
         throw exceptions::invalid_checkpoint{"checkpoint encoding is not canonical"};
      }
      return std::move(state);
   } catch (const forge::raw::exceptions::range_error&) {
      throw exceptions::invalid_checkpoint{"checkpoint is truncated"};
   } catch (const forge::raw::exceptions::codec_error&) {
      throw exceptions::invalid_checkpoint{"checkpoint is malformed"};
   } catch (const forge::raw::exceptions::allocation_limit&) {
      throw exceptions::invalid_checkpoint{"checkpoint exceeds its allocation bound"};
   }
}

} // namespace forge::swarm::core
