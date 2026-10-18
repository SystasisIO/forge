module;

#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

export module forge.swarm.core.descriptor;

export import forge.swarm.core.merkle;
import forge.raw.enum_wire_type;

export namespace forge::swarm::core {

inline constexpr std::uint32_t max_metadata_size = 16U * 1024U * 1024U;
inline constexpr std::uint32_t max_files = 4096U;
inline constexpr std::uint32_t max_path_depth = 32U;
inline constexpr std::uint32_t max_component_size = 255U;
inline constexpr std::uint32_t max_path_size = 4096U;
inline constexpr std::uint32_t max_total_path_size = 8U * 1024U * 1024U;
inline constexpr std::uint64_t max_total_size = std::uint64_t{1} << 44U;

enum class descriptor_version : std::uint32_t { v1 = 1 };

struct file_entry {
   std::vector<std::string> path;
   std::uint64_t size = 0;
   sha256 root;
   std::vector<sha256> pieces;

   bool operator==(const file_entry&) const = default;
};

struct descriptor {
   descriptor_version version = descriptor_version::v1;
   std::uint32_t piece_size = block_size;
   std::vector<file_entry> files;

   bool operator==(const descriptor&) const = default;
};

struct descriptor_summary {
   std::uint64_t total_size = 0;
   std::uint32_t total_pieces = 0;
   std::uint32_t metadata_size = 0;
};

// Validation never sorts, repairs, renames or normalizes an input descriptor.
[[nodiscard]] descriptor_summary validate_descriptor(const descriptor& value);
[[nodiscard]] std::vector<std::uint8_t> encode_descriptor(const descriptor& value);
// Exact, bounded Raw decode, including canonical encoding and all semantic validation.
[[nodiscard]] descriptor decode_descriptor(std::span<const std::uint8_t> bytes);

BOOST_DESCRIBE_ENUM(descriptor_version, v1)
BOOST_DESCRIBE_STRUCT(file_entry, (), (path, size, root, pieces))
BOOST_DESCRIBE_STRUCT(descriptor, (), (version, piece_size, files))
BOOST_DESCRIBE_STRUCT(descriptor_summary, (), (total_size, total_pieces, metadata_size))

} // namespace forge::swarm::core

export namespace forge::raw {

template <> struct enum_wire_type<forge::swarm::core::descriptor_version> {
   using type = std::uint32_t;
   static constexpr bool shared = true;
};

} // namespace forge::raw
