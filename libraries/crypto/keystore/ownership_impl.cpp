module;

#include <forge/exceptions/macros.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>

module forge.crypto.keystore.ownership;

#include "details/ownership_impl.hxx"

namespace forge::crypto::keystore {

ownership::impl::impl(std::filesystem::path path) : process_{::getpid()} {
   if (path.empty() || path.filename().empty() || path.filename() == "." || path.filename() == "..") {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid keystore ownership path");
   }
   auto directory = path.parent_path();
   if (directory.empty()) {
      directory = ".";
   }
   struct stat parent{};
   if (::lstat(directory.c_str(), &parent) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot access keystore directory");
   }
   if (!S_ISDIR(parent.st_mode) || parent.st_uid != ::geteuid() || (parent.st_mode & 0077U) != 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file, "keystore ownership requires a private directory");
   }
   auto error = std::error_code{};
   directory = std::filesystem::canonical(directory, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot resolve keystore directory");
   }
   path_ = directory / path.filename();
   lock_path_ = path_.string() + ".lock";
   descriptor_ = ::open(lock_path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (descriptor_ < 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot open keystore ownership file");
   }
   try {
      verify();
      if (::flock(descriptor_, LOCK_EX | LOCK_NB) != 0) {
         if (errno == EWOULDBLOCK || errno == EAGAIN) {
            FORGE_THROW_EXCEPTION(exceptions::in_use, "keystore is owned by another client");
         }
         FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot acquire keystore ownership");
      }
      verify();
   } catch (...) {
      static_cast<void>(::close(std::exchange(descriptor_, -1)));
      throw;
   }
}

ownership::impl::~impl() {
   // Closing, not unlinking, releases ownership. An inherited descriptor must
   // not explicitly LOCK_UN: it shares the parent's open file description.
   if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
   }
}

void ownership::impl::verify() const {
   struct stat held{};
   struct stat current{};
   if (process_ != ::getpid() || ::fstat(descriptor_, &held) != 0 || ::lstat(lock_path_.c_str(), &current) != 0 ||
       !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() || (held.st_mode & 0077U) != 0U || held.st_nlink != 1 ||
       held.st_dev != current.st_dev || held.st_ino != current.st_ino) {
      FORGE_THROW_EXCEPTION(exceptions::ownership_lost, "keystore ownership is no longer valid");
   }
}

const std::filesystem::path& ownership::impl::path() const noexcept {
   return path_;
}

void ownership::impl::require_file(std::uint64_t max_bytes) const {
   verify();
   const auto descriptor = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
   if (descriptor < 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot open encrypted keystore");
   }
   struct stat file{};
   const auto result = ::fstat(descriptor, &file);
   const auto closed = ::close(descriptor);
   if (result != 0 || closed != 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "cannot inspect encrypted keystore");
   }
   if (!S_ISREG(file.st_mode) || file.st_uid != ::geteuid() || (file.st_mode & 0077U) != 0U || file.st_nlink != 1) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file, "keystore must be an owner-only regular file");
   }
   if (file.st_size < 0 || static_cast<std::uint64_t>(file.st_size) > max_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "keystore exceeds its size limit");
   }
}

void ownership::impl::attach() {
   verify();
   if (attached_.exchange(true, std::memory_order_acq_rel)) {
      FORGE_THROW_EXCEPTION(exceptions::in_use, "keystore owner already has an open store");
   }
}

void ownership::impl::detach() noexcept {
   attached_.store(false, std::memory_order_release);
}

} // namespace forge::crypto::keystore
