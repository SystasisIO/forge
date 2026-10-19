module;

#include <cstdint>
#include <optional>
#include <vector>

export module forge.swarm.protocol.metadata_reader;

export import forge.swarm.protocol.metadata;

export namespace forge::swarm::protocol {

class metadata_reader {
 public:
   explicit metadata_reader(const metadata&);
   [[nodiscard]] std::optional<metadata_chunk> next();

 private:
   std::vector<std::uint8_t> _bytes;
   std::uint32_t _offset = 0;
};

} // namespace forge::swarm::protocol
