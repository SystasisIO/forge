module;

#include <boost/describe.hpp>
#include <cstdint>
#include <span>

export module forge.swarm.core.identity;

export import forge.swarm.core.descriptor;

export namespace forge::swarm::core {

struct swarm_id {
   sha256 value;

   bool operator==(const swarm_id&) const = default;
};

// SHA256(Raw(canonical descriptor)), not a torrent infohash or bare file root.
[[nodiscard]] swarm_id calculate_swarm_id(const descriptor& value);
[[nodiscard]] swarm_id calculate_swarm_id(std::span<const std::uint8_t> bytes);

BOOST_DESCRIBE_STRUCT(swarm_id, (), (value))

} // namespace forge::swarm::core
