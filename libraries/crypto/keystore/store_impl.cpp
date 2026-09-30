module;

#include <forge/exceptions/macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

module forge.crypto.keystore.store;

import forge.codec.hex;
import forge.crypto.core.random;
import forge.raw.raw;

#include "details/store_impl.hxx"

namespace forge::crypto::keystore {
namespace {

struct serialized_entry {
   std::string id;
   core::bytes encoded;

   serialized_entry() = default;
   serialized_entry(std::string id_value, core::bytes encoded_value)
       : id(std::move(id_value)), encoded(std::move(encoded_value)) {}
   ~serialized_entry() {
      core::secure_erase(encoded);
   }

   serialized_entry(const serialized_entry&) = delete;
   serialized_entry& operator=(const serialized_entry&) = delete;

   serialized_entry(serialized_entry&& other) noexcept : id(std::move(other.id)), encoded(std::move(other.encoded)) {
      other.encoded.clear();
   }

   serialized_entry& operator=(serialized_entry&& other) noexcept {
      if (this != &other) {
         core::secure_erase(encoded);
         id = std::move(other.id);
         encoded = std::move(other.encoded);
         other.encoded.clear();
      }
      return *this;
   }
};

using serialized_entries = std::vector<serialized_entry>;

template <typename Stream> void raw_pack(Stream& stream, const serialized_entry& value) {
   forge::raw::pack(stream, value.id);
   forge::raw::pack(stream, value.encoded);
}

template <typename Stream> void raw_unpack(Stream& stream, serialized_entry& value) {
   forge::raw::unpack(stream, value.id);
   forge::raw::unpack(stream, value.encoded);
}

class unique_descriptor {
 public:
   explicit unique_descriptor(int value = -1) noexcept : value_(value) {}
   ~unique_descriptor() {
      reset();
   }

   unique_descriptor(const unique_descriptor&) = delete;
   unique_descriptor& operator=(const unique_descriptor&) = delete;

   unique_descriptor(unique_descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}

   unique_descriptor& operator=(unique_descriptor&& other) noexcept {
      if (this != &other) {
         reset();
         value_ = std::exchange(other.value_, -1);
      }
      return *this;
   }

   [[nodiscard]] int get() const noexcept {
      return value_;
   }

   int close() noexcept {
      const auto value = std::exchange(value_, -1);
      return value < 0 ? 0 : ::close(value);
   }

   void reset() noexcept {
      static_cast<void>(close());
   }

 private:
   int value_;
};

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

store::impl::impl(std::shared_ptr<ownership> owner, core::secret_string password, store_options options)
    : owner_(std::move(owner)), password_(std::move(password)), options_(options) {
   validate_options(options_);
   if (!owner_ || password_.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "keystore path and password must not be empty");
   }
   owner_->verify();
   path_ = owner_->path();
   access_ = owner_->claim();
}

std::unique_ptr<store::impl> store::impl::create(std::filesystem::path path, core::secret_string password,
                                                 store_options options) {
   if (path.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "keystore path must not be empty");
   }
   ensure_private_directory(path.parent_path().empty() ? "." : path.parent_path());
   return create(ownership::acquire(std::move(path)), std::move(password), options);
}

std::unique_ptr<store::impl> store::impl::create(std::shared_ptr<ownership> owner, core::secret_string password,
                                                 store_options options) {
   auto result = std::make_unique<impl>(std::move(owner), std::move(password), options);
   const auto& path = result->path_;
   auto error = std::error_code{};
   const auto exists = std::filesystem::exists(path, error);
   if (error) {
      throw_io("stat", path, error.value());
   }
   if (exists) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "keystore file already exists",
                            forge::exceptions::ctx("path", path.string()));
   }
   result->write_entries(result->entries_, false);
   return result;
}

std::unique_ptr<store::impl> store::impl::open(std::filesystem::path path, core::secret_string password,
                                               store_options options) {
   return open(ownership::acquire(std::move(path)), std::move(password), options);
}

std::unique_ptr<store::impl> store::impl::open(std::shared_ptr<ownership> owner, core::secret_string password,
                                               store_options options) {
   auto result = std::make_unique<impl>(std::move(owner), std::move(password), options);
   result->load();
   return result;
}

void store::impl::validate_options(const store_options& options) {
   if (options.max_keys == 0U || options.max_key_id_bytes == 0U || options.limits.max_plaintext_bytes == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "keystore limits must be greater than zero");
   }
   if (options.encryption.scrypt_n > options.limits.max_scrypt_n ||
       options.encryption.scrypt_r > options.limits.max_scrypt_r ||
       options.encryption.scrypt_p > options.limits.max_scrypt_p ||
       options.encryption.scrypt_max_memory_bytes > options.limits.max_scrypt_memory_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "keystore encryption parameters exceed configured decrypt limits");
   }
}

void store::impl::validate_id(const signer::key_id& id) const {
   if (id.value.empty() || id.value.size() > options_.max_key_id_bytes || id.value.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid keystore key id");
   }
}

void store::impl::load() {
   owner_->verify();
   auto container = read_file(path_, options_.limits.max_plaintext_bytes + 4U * 1024U);
   auto plaintext = decrypt_file(container, password_, options_.limits);
   serialized_entries serialized;
   try {
      serialized = forge::raw::unpack_exact<serialized_entries>(plaintext.span());
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file, "keystore payload is malformed");
   }
   if (serialized.size() > options_.max_keys) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "keystore contains too many keys");
   }

   auto loaded = entries{};
   try {
      for (auto& serialized_value : serialized) {
         validate_id(signer::key_id{.value = serialized_value.id});
         auto encoded = core::secret_string{std::string{reinterpret_cast<const char*>(serialized_value.encoded.data()),
                                                        serialized_value.encoded.size()}};
         auto key = asymmetric::encoding::forge().parse_private(encoded.view());
         auto value = entry{
             .encoded = std::move(encoded),
             .public_key = key.get_public_key(),
         };
         if (!loaded.emplace(std::move(serialized_value.id), std::move(value)).second) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_file, "keystore contains a duplicate key id");
         }
      }
   } catch (const exceptions::invalid_file&) {
      throw;
   } catch (const forge::exceptions::base&) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_file, "keystore payload contains an invalid key entry");
   }
   auto lock = std::scoped_lock{mutex_};
   entries_ = std::move(loaded);
}

void store::impl::write_entries(const entries& value, bool replace) const {
   owner_->verify();
   auto serialized = serialized_entries{};
   serialized.reserve(value.size());
   for (const auto& [id, key] : value) {
      const auto encoded = key.encoded.view();
      serialized.emplace_back(id, core::bytes{encoded.begin(), encoded.end()});
   }
   auto packed = forge::raw::pack(serialized);
   if (packed.size() > options_.limits.max_plaintext_bytes) {
      core::secure_erase(packed);
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "keystore payload exceeds its plaintext limit");
   }
   auto plaintext = core::secret_bytes{std::move(packed)};
   auto container = encrypt_file(encrypted_file_request{
       .plaintext = std::move(plaintext),
       .password = password_,
       .encryption = options_.encryption,
   });
   write_atomic(path_, container, replace);
}

void store::impl::save_locked() const {
   write_entries(entries_);
}

void store::impl::save() {
   auto lock = std::scoped_lock{mutex_};
   save_locked();
}

void store::impl::put(signer::key_id id, asymmetric::private_key key, bool replace) {
   validate_id(id);
   auto lock = std::scoped_lock{mutex_};
   if (entries_.size() >= options_.max_keys && !entries_.contains(id.value)) {
      FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "keystore key count limit reached");
   }
   const auto iterator = entries_.find(id.value);
   if (iterator != entries_.end() && !replace) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_key, "keystore key id already exists");
   }
   auto value = entry{
       .encoded = core::secret_string{asymmetric::encoding::forge().format(key)},
       .public_key = key.get_public_key(),
   };
   auto updated = entries_;
   const auto updated_iterator = updated.find(id.value);
   if (updated_iterator == updated.end()) {
      updated.emplace(std::move(id.value), std::move(value));
   } else {
      updated_iterator->second = std::move(value);
   }
   try {
      write_entries(updated);
   } catch (const exceptions::durability_unknown&) {
      entries_ = std::move(updated);
      throw;
   }
   entries_ = std::move(updated);
}

void store::impl::erase(const signer::key_id& id) {
   auto lock = std::scoped_lock{mutex_};
   if (!entries_.contains(id.value)) {
      FORGE_THROW_EXCEPTION(exceptions::unknown_key, "keystore key id was not found");
   }
   auto updated = entries_;
   updated.erase(id.value);
   try {
      write_entries(updated);
   } catch (const exceptions::durability_unknown&) {
      entries_ = std::move(updated);
      throw;
   }
   entries_ = std::move(updated);
}

std::vector<signer::key_info> store::impl::keys() const {
   owner_->verify();
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<signer::key_info>{};
   result.reserve(entries_.size());
   for (const auto& [id, key] : entries_) {
      result.push_back(signer::key_info{.id = {.value = id}, .public_key = key.public_key});
   }
   return result;
}

signer::key_info store::impl::describe(const signer::key_id& id) const {
   owner_->verify();
   auto lock = std::scoped_lock{mutex_};
   const auto iterator = entries_.find(id.value);
   if (iterator == entries_.end()) {
      FORGE_THROW_EXCEPTION(signer::exceptions::unknown_key, "signer key id was not found");
   }
   return signer::key_info{.id = {.value = iterator->first}, .public_key = iterator->second.public_key};
}

signer::sign_digest_response store::impl::sign_digest(const signer::sign_digest_request& request) const {
   owner_->verify();
   auto lock = std::scoped_lock{mutex_};
   const auto iterator = entries_.find(request.id.value);
   if (iterator == entries_.end()) {
      FORGE_THROW_EXCEPTION(signer::exceptions::unknown_key, "signer key id was not found");
   }
   const auto key = asymmetric::encoding::forge().parse_private(iterator->second.encoded.view());
   return signer::sign_digest_response{
       .public_key = iterator->second.public_key,
       .signature = key.sign_digest(request.digest),
   };
}

const std::filesystem::path& store::impl::path() const noexcept {
   return path_;
}

std::shared_ptr<ownership> store::impl::owner() const noexcept {
   return owner_;
}

} // namespace forge::crypto::keystore
