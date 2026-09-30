module;

#include <sys/types.h>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <utility>

module forge.crypto.keystore.ownership;

#include "details/ownership_impl.hxx"

namespace forge::crypto::keystore {

ownership::ownership(std::unique_ptr<impl> value) : impl_{std::move(value)} {}
ownership::~ownership() = default;

std::shared_ptr<ownership> ownership::acquire(std::filesystem::path path) {
   return std::shared_ptr<ownership>{new ownership{std::make_unique<impl>(std::move(path))}};
}

const std::filesystem::path& ownership::path() const noexcept {
   return impl_->path();
}

void ownership::verify() const {
   impl_->verify();
}

void ownership::require_file(std::uint64_t max_bytes) const {
   impl_->require_file(max_bytes);
}

ownership::access::access(std::shared_ptr<impl> value) : impl_{std::move(value)} {
   impl_->attach();
}

ownership::access::~access() {
   impl_->detach();
}

std::shared_ptr<ownership::access> ownership::claim() {
   return std::shared_ptr<access>{new access{impl_}};
}

} // namespace forge::crypto::keystore
