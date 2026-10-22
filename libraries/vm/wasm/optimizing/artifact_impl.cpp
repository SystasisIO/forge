module;

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <mutex>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.artifact;

#include "details/artifact_impl.hxx"

namespace forge::vm::wasm::optimizing {

artifact::impl::impl(artifact_payload value, std::vector<std::uint8_t> bytes, std::uint64_t charge, origin trusted,
                     retention* retained)
    : lifetime(retained), image(std::move(value)), encoded(std::move(bytes)),
      identity(identify(image.wasm, image.configuration)),
      owned_bytes(charge + retained->control_bytes + sizeof(retention)), authority(trusted) {}

} // namespace forge::vm::wasm::optimizing
