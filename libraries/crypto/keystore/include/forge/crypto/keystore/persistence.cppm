module;

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>

export module forge.crypto.keystore.persistence;

export import forge.crypto.core.types;
export import forge.crypto.keystore.ownership;

export namespace forge::crypto::keystore {

void prepare_private_directory(const std::filesystem::path& directory);
[[nodiscard]] core::bytes read_container(const std::shared_ptr<ownership>& owner, std::uint64_t maximum);
void write_container(const std::shared_ptr<ownership>& owner, std::span<const std::uint8_t> container,
                     bool replace = true);

} // namespace forge::crypto::keystore
