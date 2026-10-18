module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

module forge.swarm.core.descriptor;

import forge.core.utf8;
import forge.raw.datastream;
import forge.raw.exceptions;
import forge.raw.raw;
import forge.raw.varint;

namespace forge::swarm::core {
namespace {

// Native Raw integers retain object representation. V1 explicitly freezes little-endian hosts.
static_assert(std::endian::native == std::endian::little);

void account(std::uint64_t& total, std::uint64_t count, std::uint64_t limit) {
   if (total > limit || count > limit - total) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "descriptor exceeds a Swarm v1 resource limit");
   }
   total += count;
}

int compare_component(std::string_view left, std::string_view right) {
   for (auto i = std::size_t{0}; i < std::min(left.size(), right.size()); ++i) {
      const auto a = static_cast<unsigned char>(left[i]);
      const auto b = static_cast<unsigned char>(right[i]);
      if (a != b) {
         return a < b ? -1 : 1;
      }
   }
   return left.size() < right.size() ? -1 : left.size() != right.size();
}

int compare_paths(const std::vector<std::string>& left, const std::vector<std::string>& right) {
   for (auto i = std::size_t{0}; i < std::min(left.size(), right.size()); ++i) {
      const auto order = compare_component(left[i], right[i]);
      if (order != 0) {
         return order;
      }
   }
   return left.size() < right.size() ? -1 : left.size() != right.size();
}

void validate_component(std::string_view value) {
   if (value.size() > max_component_size) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "path component exceeds 255 bytes");
   }
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_path, "path component has an invalid byte length");
   }
   if (value == "." || value == ".." || !forge::is_valid_utf8(value)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_path, "path component is dot traversal or invalid UTF-8");
   }
   for (const auto byte : value) {
      const auto c = static_cast<unsigned char>(byte);
      if (c < 0x20U || c == 0x7fU || c == '/' || c == '\\') {
         FORGE_THROW_EXCEPTION(exceptions::invalid_path, "path component contains a control byte or separator");
      }
   }
}

void validate_version(descriptor_version version) {
   if (version != descriptor_version::v1) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_version, "unsupported Swarm descriptor version");
   }
}

using input_stream = forge::datastream<const std::uint8_t*>;

std::uint32_t read_count(input_stream& stream) {
   const auto start = stream.tellp();
   auto count = forge::unsigned_int{};
   forge::raw::unpack(stream, count);
   const auto width = std::max(1U, (std::bit_width(count.value) + 6U) / 7U);
   if (stream.tellp() - start != width) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor contains a nonminimal Raw count");
   }
   return count.value;
}

// Domain-specific allocation preflight, not a second codec: Raw owns every scalar/count encoding.
// This walk mirrors the reflected field order and checks lengths against the actual frame before resize.
forge::raw::unpack_limits preflight(std::span<const std::uint8_t> bytes) {
   if (bytes.size() > max_metadata_size) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "descriptor metadata exceeds 16 MiB");
   }
   if (bytes.size() < 9U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor header is truncated");
   }
   auto stream = input_stream{bytes.data(), bytes.size()};
   auto version = descriptor_version{};
   forge::raw::unpack(stream, version);
   validate_version(version);
   auto piece_size = std::uint32_t{};
   forge::raw::unpack(stream, piece_size);
   validate_piece_size(piece_size);
   const auto files = read_count(stream);
   if (files == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "descriptor must contain at least one file");
   }
   // Even an invalid empty path needs its depth, size, root and piece-count fields.
   if (files > stream.remaining() / 42U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "file count cannot fit the descriptor frame");
   }
   if (files > max_files) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "descriptor exceeds 4096 files");
   }
   auto elements = std::uint64_t{files};
   auto largest_container = files;
   auto total_path = std::uint64_t{0};
   auto total_size = std::uint64_t{0};
   auto total_pieces = std::uint64_t{0};
   auto total_hashes = std::uint64_t{0};
   constexpr auto max_elements = max_files + max_files * max_path_depth + max_piece_count;
   for (auto file = 0U; file < files; ++file) {
      const auto depth = read_count(stream);
      if (depth > stream.remaining() / 2U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "path depth exceeds the remaining frame");
      }
      if (depth == 0U) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_path, "file path has an invalid component count");
      }
      if (depth > max_path_depth) {
         FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "file path exceeds 32 components");
      }
      account(elements, depth, max_elements);
      largest_container = std::max(largest_container, depth);
      auto path_size = std::uint64_t{depth - 1U};
      for (auto component = 0U; component < depth; ++component) {
         const auto length = read_count(stream);
         if (length > stream.remaining()) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "path component exceeds the remaining frame");
         }
         validate_component(std::string_view{reinterpret_cast<const char*>(stream.pos()), length});
         account(path_size, length, max_path_size);
         stream.skip(length);
      }
      account(total_path, path_size, max_total_path_size);
      auto size = std::uint64_t{0};
      forge::raw::unpack(stream, size);
      const auto count = piece_count(size, piece_size);
      account(total_size, size, max_total_size);
      account(total_pieces, count, max_piece_count);
      stream.skip(sha256::byte_size);
      const auto hashes = read_count(stream);
      if (hashes != (size > piece_size ? count : 0U) || hashes > stream.remaining() / sha256::byte_size) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "piece hash count does not match geometry or remaining frame");
      }
      account(total_hashes, hashes, max_piece_count);
      account(elements, hashes, max_elements);
      largest_container = std::max(largest_container, hashes);
      stream.skip(std::size_t{hashes} * sha256::byte_size);
   }
   if (stream.remaining() != 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor contains trailing bytes");
   }
   return {.max_container_elements = largest_container,
           .max_total_container_elements = static_cast<std::uint32_t>(elements),
           .max_bytes = max_component_size,
           .first_container_elements = files};
}

} // namespace

descriptor_summary validate_descriptor(const descriptor& value) {
   validate_version(value.version);
   validate_piece_size(value.piece_size);
   if (value.files.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "descriptor must contain at least one file");
   }
   if (value.files.size() > max_files) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "descriptor exceeds 4096 files");
   }
   auto total_size = std::uint64_t{0};
   auto total_pieces = std::uint64_t{0};
   auto total_hashes = std::uint64_t{0};
   auto total_path = std::uint64_t{0};
   const file_entry* previous = nullptr;
   for (const auto& file : value.files) {
      if (file.path.size() > max_path_depth) {
         FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "file path exceeds 32 components");
      }
      if (file.path.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_path, "file path has an invalid component count");
      }
      auto path_size = std::uint64_t{file.path.size() - 1U};
      for (const auto& component : file.path) {
         validate_component(component);
         account(path_size, component.size(), max_path_size);
      }
      account(total_path, path_size, max_total_path_size);
      if (previous != nullptr) {
         if (compare_paths(previous->path, file.path) >= 0) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_path, "file paths must be strictly componentwise unsigned-byte ordered");
         }
         if (previous->path.size() <= file.path.size() &&
             std::equal(previous->path.begin(), previous->path.end(), file.path.begin())) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_path, "file/directory prefix collision");
         }
      }
      previous = &file;
      const auto count = piece_count(file.size, value.piece_size);
      account(total_size, file.size, max_total_size);
      account(total_pieces, count, max_piece_count);
      account(total_hashes, file.pieces.size(), max_piece_count);
      if (file.pieces.size() != (file.size > value.piece_size ? count : 0U)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "file piece layer has the wrong hash count");
      }
      if (file.size == 0U && !file.root.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "empty files require a zero root and empty piece layer");
      }
   }
   auto size_stream = forge::datastream<std::size_t>{};
   forge::raw::pack(size_stream, value);
   if (size_stream.tellp() > max_metadata_size) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "descriptor metadata exceeds 16 MiB");
   }
   for (const auto& file : value.files) {
      if (file.size > value.piece_size && root_from_piece_layer(file.pieces, file.size, value.piece_size) != file.root) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "file piece layer does not match its root");
      }
   }
   return {.total_size = total_size,
           .total_pieces = static_cast<std::uint32_t>(total_pieces),
           .metadata_size = static_cast<std::uint32_t>(size_stream.tellp())};
}

std::vector<std::uint8_t> encode_descriptor(const descriptor& value) {
   const auto summary = validate_descriptor(value);
   auto bytes = std::vector<std::uint8_t>(summary.metadata_size);
   auto stream = forge::datastream<std::uint8_t*>{bytes.data(), bytes.size()};
   forge::raw::pack(stream, value);
   return bytes;
}

descriptor decode_descriptor(std::span<const std::uint8_t> bytes) {
   try {
      const auto limits = preflight(bytes);
      auto value = forge::raw::unpack_exact<descriptor>(bytes, limits);
      const auto summary = validate_descriptor(value);
      if (summary.metadata_size != bytes.size()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor Raw size is not canonical");
      }
      return value;
   } catch (const forge::raw::exceptions::range_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor Raw input is truncated");
   } catch (const forge::raw::exceptions::codec_error&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor Raw input is malformed");
   } catch (const forge::raw::exceptions::allocation_limit&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_encoding, "descriptor Raw input exceeds its checked allocation budget");
   }
}

} // namespace forge::swarm::core
