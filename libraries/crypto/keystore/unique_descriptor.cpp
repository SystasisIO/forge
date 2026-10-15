module;

#include <unistd.h>
#include <utility>

module forge.crypto.keystore.persistence;

#include "details/unique_descriptor.hxx"

namespace forge::crypto::keystore {

unique_descriptor::unique_descriptor(int value) noexcept : value_{value} {}

unique_descriptor::~unique_descriptor() {
   reset();
}

unique_descriptor::unique_descriptor(unique_descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}

unique_descriptor& unique_descriptor::operator=(unique_descriptor&& other) noexcept {
   if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
   }
   return *this;
}

int unique_descriptor::get() const noexcept {
   return value_;
}

int unique_descriptor::close() noexcept {
   const auto value = std::exchange(value_, -1);
   return value < 0 ? 0 : ::close(value);
}

void unique_descriptor::reset() noexcept {
   static_cast<void>(close());
}

} // namespace forge::crypto::keystore
