module;

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.protocol.metadata_reader;

namespace forge::swarm::protocol {

metadata_reader::metadata_reader(const metadata& value) {
   try {
      _bytes = core::encode_descriptor(value.manifest);
   } catch (const forge::exceptions::context_error& error) {
      if (error.code().category() !=
          core::exceptions::make_error_code(core::exceptions::code::invalid_descriptor).category())
         throw;
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata reader descriptor is invalid");
   }
   if (core::sha256::hash(std::span<const std::uint8_t>{_bytes}) != value.swarm.value) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata reader identity does not match descriptor");
   }
}

std::optional<metadata_chunk> metadata_reader::next() {
   if (_offset >= _bytes.size())
      return std::nullopt;
   const auto total = static_cast<std::uint32_t>(_bytes.size());
   const auto length = std::min(metadata_chunk_size, total - _offset);
   auto result = metadata_chunk{total, _offset,
                                std::vector<std::uint8_t>(_bytes.begin() + _offset, _bytes.begin() + _offset + length)};
   _offset += length;
   return result;
}

} // namespace forge::swarm::protocol
