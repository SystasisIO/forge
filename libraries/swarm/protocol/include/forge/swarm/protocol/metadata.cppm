module;

#include <boost/describe.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.swarm.protocol.metadata;

export import forge.swarm.core.identity;
export import forge.swarm.protocol.exceptions;
export import forge.raw.codec;

export namespace forge::swarm::protocol {

inline constexpr std::uint32_t max_realm_size = 128U;
inline constexpr std::uint32_t metadata_chunk_size = core::block_size;
inline constexpr std::uint32_t max_metadata_chunk_wire_size = metadata_chunk_size + 11U;

struct metadata_request {
   core::swarm_id swarm;
   std::string realm;
   bool operator==(const metadata_request&) const = default;
};

struct exchange_request {
   core::swarm_id swarm;
   std::string realm;
   bool operator==(const exchange_request&) const = default;
};

// Public immutable metadata only; identity is not local payload availability.
struct metadata {
   core::swarm_id swarm;
   core::descriptor manifest;
   bool operator==(const metadata&) const = default;
};

struct metadata_chunk {
   std::uint32_t total = 0;
   std::uint32_t offset = 0;
   std::vector<std::uint8_t> data;
   bool operator==(const metadata_chunk&) const = default;
};

void validate_realm(std::string_view);
void validate(const metadata_request&);
void validate(const exchange_request&);
void validate(const metadata&);
void validate(const metadata_chunk&);
[[nodiscard]] std::vector<std::uint8_t> encode_metadata_chunk(const metadata_chunk&);
[[nodiscard]] metadata_chunk decode_metadata_chunk(std::span<const std::uint8_t>,
                                                   forge::raw::unpack_limits limits = {});

BOOST_DESCRIBE_STRUCT(metadata_request, (), (swarm, realm))
BOOST_DESCRIBE_STRUCT(exchange_request, (), (swarm, realm))
BOOST_DESCRIBE_STRUCT(metadata, (), (swarm, manifest))
BOOST_DESCRIBE_STRUCT(metadata_chunk, (), (total, offset, data))

} // namespace forge::swarm::protocol
