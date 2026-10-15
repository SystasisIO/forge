module;

#include <forge/exceptions/macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

module forge.crypto.keystore.persistence;

import forge.codec.hex;
import forge.crypto.core.random;

#include "details/unique_descriptor.hxx"

namespace forge::crypto::keystore {
namespace {

[[noreturn]] void throw_io(std::string_view operation, const std::filesystem::path& path, int error = errno) {
   FORGE_THROW_EXCEPTION(
       exceptions::io_error, "keystore filesystem operation failed", forge::exceptions::ctx("operation", operation),
       forge::exceptions::ctx("path", path.string()),
       forge::exceptions::ctx("system_error", std::error_code{error, std::generic_category()}.message()));
}

[[noreturn]] void throw_durability_unknown(std::string_view operation, const std::filesystem::path& path,
                                           int error = errno) {
   FORGE_THROW_EXCEPTION(
       exceptions::durability_unknown, "keystore update was published but its crash durability is unknown",
       forge::exceptions::ctx("operation", operation), forge::exceptions::ctx("path", path.string()),
       forge::exceptions::ctx("system_error", std::error_code{error, std::generic_category()}.message()));
}

unique_descriptor open_directory(const std::filesystem::path& path) {
   auto flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
   flags |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   auto descriptor = unique_descriptor{::open(path.c_str(), flags)};
   if (descriptor.get() < 0) {
      throw_io("open_directory", path);
   }
   return descriptor;
}

void validate_private_directory(const std::filesystem::path& path) {
   struct stat status{};
   if (::lstat(path.c_str(), &status) != 0) {
      throw_io("stat_directory", path);
   }
   if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077U) != 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file,
                            "keystore directory must be owned by the current user with mode 0700");
   }
}

void sync_directory(const std::filesystem::path& path, std::string_view operation) {
   auto descriptor = open_directory(path);
   if (::fsync(descriptor.get()) != 0) {
      throw_io(operation, path);
   }
}

void sync_directory_chain(std::filesystem::path path) {
   for (;;) {
      sync_directory(path, "fsync_directory");
      auto parent = path.parent_path();
      if (parent == path) {
         return;
      }
      if (parent.empty()) {
         if (path != ".") {
            sync_directory(".", "fsync_parent_directory");
         }
         return;
      }
      path = std::move(parent);
   }
}

void ensure_private_directory(const std::filesystem::path& directory) {
   auto missing = std::vector<std::filesystem::path>{};
   auto current = directory;
   for (;;) {
      struct stat status{};
      if (::lstat(current.c_str(), &status) == 0) {
         break;
      }
      if (errno != ENOENT) {
         throw_io("stat_directory", current);
      }
      missing.push_back(current);
      auto parent = current.parent_path();
      if (parent.empty()) {
         parent = ".";
      }
      if (parent == current) {
         throw_io("create_directory", current, ENOENT);
      }
      current = std::move(parent);
   }

   for (auto iterator = missing.rbegin(); iterator != missing.rend(); ++iterator) {
      if (::mkdir(iterator->c_str(), 0700) != 0) {
         if (errno != EEXIST) {
            throw_io("create_directory", *iterator);
         }
      } else if (::chmod(iterator->c_str(), 0700) != 0) {
         throw_io("chmod_directory", *iterator);
      }
      validate_private_directory(*iterator);
   }

   validate_private_directory(directory);
   auto error = std::error_code{};
   const auto canonical_directory = std::filesystem::canonical(directory, error);
   if (error) {
      throw_io("canonicalize_directory", directory, error.value());
   }
   sync_directory_chain(canonical_directory);
}

core::bytes read_file(const std::filesystem::path& path, std::uint64_t maximum) {
   auto flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   auto descriptor = unique_descriptor{::open(path.c_str(), flags)};
   if (descriptor.get() < 0) {
      throw_io("open", path);
   }

   struct stat status{};
   if (::fstat(descriptor.get(), &status) != 0) {
      throw_io("stat", path);
   }
   if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077U) != 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file,
                            "keystore must be a regular file owned by the current user with mode 0600");
   }
   if (status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > maximum) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "keystore file exceeds its size limit");
   }

   auto result = core::bytes(static_cast<std::size_t>(status.st_size));
   auto offset = std::size_t{};
   while (offset < result.size()) {
      const auto count = ::read(descriptor.get(), result.data() + offset, result.size() - offset);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      if (count <= 0) {
         const auto error = count < 0 ? errno : EIO;
         throw_io("read", path, error);
      }
      offset += static_cast<std::size_t>(count);
   }
   if (descriptor.close() != 0) {
      throw_io("close", path);
   }
   return result;
}

void write_all(int descriptor, std::span<const std::uint8_t> value, const std::filesystem::path& path) {
   auto offset = std::size_t{};
   while (offset < value.size()) {
      const auto count = ::write(descriptor, value.data() + offset, value.size() - offset);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      if (count <= 0) {
         throw_io("write", path, count < 0 ? errno : EIO);
      }
      offset += static_cast<std::size_t>(count);
   }
}

void write_atomic(const std::filesystem::path& path, std::span<const std::uint8_t> value, bool replace) {
   auto directory = path.parent_path();
   if (directory.empty()) {
      directory = ".";
   }
   ensure_private_directory(directory);

   const auto suffix = codec::hex::encode(core::random_bytes(12U));
   const auto temporary = std::filesystem::path{path.string() + ".tmp-" + suffix};
   auto directory_descriptor = open_directory(directory);
   auto descriptor = unique_descriptor{::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600)};
   if (descriptor.get() < 0) {
      throw_io("create", temporary);
   }
   try {
      write_all(descriptor.get(), value, temporary);
      if (::fsync(descriptor.get()) != 0) {
         throw_io("fsync", temporary);
      }
      if (descriptor.close() != 0) {
         throw_io("close", temporary);
      }
      if (replace) {
         if (::rename(temporary.c_str(), path.c_str()) != 0) {
            throw_io("rename", path);
         }
      } else {
         if (::link(temporary.c_str(), path.c_str()) != 0) {
            throw_io("create", path);
         }
         if (::unlink(temporary.c_str()) != 0) {
            throw_durability_unknown("unlink_temporary", path);
         }
      }
      const auto sync_result = ::fsync(directory_descriptor.get());
      const auto sync_error = errno;
      if (sync_result != 0) {
         throw_durability_unknown("fsync_directory", path, sync_error);
      }
   } catch (...) {
      static_cast<void>(::unlink(temporary.c_str()));
      throw;
   }
}

} // namespace

void prepare_private_directory(const std::filesystem::path& directory) {
   ensure_private_directory(directory);
}

core::bytes read_container(const std::shared_ptr<ownership>& owner, std::uint64_t maximum) {
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Encrypted container ownership is required");
   }
   owner->verify();
   return read_file(owner->path(), maximum);
}

void write_container(const std::shared_ptr<ownership>& owner, std::span<const std::uint8_t> container, bool replace) {
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "Encrypted container ownership is required");
   }
   owner->verify();
   write_atomic(owner->path(), container, replace);
}

} // namespace forge::crypto::keystore
