module;

#include <cstdint>
#include <span>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.swarm.protocol.metadata_assembler;

namespace forge::swarm::protocol {

metadata_assembler::metadata_assembler(core::swarm_id expected) : _expected(expected) {}

void metadata_assembler::append(const metadata_chunk& value) {
   validate(value);
   if (_finished || value.offset != _bytes.size() || (_total != 0U && value.total != _total)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata chunks must be ordered with a stable total");
   }
   if (_total == 0U)
      _bytes.reserve(value.total);
   _bytes.insert(_bytes.end(), value.data.begin(), value.data.end());
   _total = value.total;
}

bool metadata_assembler::complete() const noexcept {
   return _total != 0U && _bytes.size() == _total;
}
std::uint32_t metadata_assembler::received_size() const noexcept {
   return static_cast<std::uint32_t>(_bytes.size());
}

metadata metadata_assembler::finish() && {
   if (_finished || !complete()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "metadata assembly is incomplete or already consumed");
   }
   auto manifest = core::descriptor{};
   try {
      manifest = core::decode_descriptor(_bytes);
   } catch (const forge::exceptions::context_error& error) {
      if (error.code().category() !=
          core::exceptions::make_error_code(core::exceptions::code::invalid_descriptor).category())
         throw;
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "assembled descriptor is invalid");
   }
   if (core::sha256::hash(std::span<const std::uint8_t>{_bytes}) != _expected.value) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_metadata, "assembled descriptor does not match expected identity");
   }
   _finished = true;
   return {_expected, std::move(manifest)};
}

} // namespace forge::swarm::protocol
