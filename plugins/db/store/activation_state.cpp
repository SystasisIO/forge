module;

#include <forge/exceptions/macros.hpp>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>

module forge.plugins.db.store.plugin;

import forge.exceptions;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.api;

#include "details/activation_state.hxx"

namespace forge::plugins::db::store {
namespace {
constexpr auto lock_name = ".forge-db-activation.lock";

[[noreturn]] void io_error(const char* operation, int code) {
   FORGE_THROW_EXCEPTION(exceptions::activation_incomplete, operation, forge::exceptions::ctx("native-code", code));
}

bool private_directory(const struct stat& value) {
   return S_ISDIR(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & 0077U) == 0U;
}

bool same(const struct stat& left, const struct stat& right) {
   return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

std::filesystem::path absolute_path(std::filesystem::path value) {
   if (!value.is_absolute())
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "activation paths must be absolute");
   value = value.lexically_normal();
   if (value.filename().empty() || value.filename() == "." || value.filename() == ".." || value == value.root_path())
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "activation requires an explicit directory leaf");
   return value;
}

int rename_no_replace(int parent, const char* source, const char* target) {
#if defined(__APPLE__)
   return ::renameatx_np(parent, source, parent, target, RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
   return static_cast<int>(::syscall(SYS_renameat2, parent, source, parent, target, RENAME_NOREPLACE));
#else
   errno = ENOSYS;
   return -1;
#endif
}
} // namespace

activation_state::activation_state(std::filesystem::path root) : process_{static_cast<std::uint64_t>(::getpid())} {
   status_.root = std::move(root);
}

activation_state::~activation_state() {
   // Close only. Never publish, retry, unlink the lock, or remove data.
   if (root_fd_ >= 0)
      ::close(root_fd_);
   if (lock_fd_ >= 0)
      ::close(lock_fd_);
   if (parent_fd_ >= 0)
      ::close(parent_fd_);
}

void activation_state::prepare() {
   auto root = absolute_path(status_.root);
   std::error_code error;
   auto parent = std::filesystem::canonical(root.parent_path(), error);
   if (error)
      io_error("cannot resolve activation parent", error.value());
   parent_fd_ = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
   if (parent_fd_ < 0)
      io_error("cannot open activation parent", errno);
   struct stat parent_stat{};
   if (::fstat(parent_fd_, &parent_stat) != 0)
      io_error("cannot inspect activation parent", errno);
   if (!private_directory(parent_stat))
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "activation parent must be owner-only");
   parent_ = std::move(parent);
   leaf_ = root.filename().string();
   root_fd_ = ::openat(parent_fd_, leaf_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
   if (root_fd_ < 0)
      io_error("cannot open activation root", errno);
   struct stat root_stat{};
   if (::fstat(root_fd_, &root_stat) != 0)
      io_error("cannot inspect activation root", errno);
   if (!private_directory(root_stat) || root_stat.st_dev != parent_stat.st_dev)
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument,
                            "activation root must be private and on the parent filesystem");
   lock_fd_ = ::openat(parent_fd_, lock_name, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
   if (lock_fd_ < 0)
      io_error("cannot open activation ownership", errno);
   if (::flock(lock_fd_, LOCK_EX | LOCK_NB) != 0) {
      const auto code = errno;
      if (code == EWOULDBLOCK || code == EAGAIN)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "activation parent is already owned",
                               forge::exceptions::ctx("native-code", code));
      io_error("cannot own activation parent", code);
   }
   {
      const auto lock = std::scoped_lock{mutex_};
      status_.root = parent_ / leaf_;
      status_.device = static_cast<std::uint64_t>(root_stat.st_dev);
      status_.inode = static_cast<std::uint64_t>(root_stat.st_ino);
   }
   verify();
}

bool activation_state::matches(const std::string& leaf) const {
   struct stat held{};
   struct stat named{};
   return ::fstat(root_fd_, &held) == 0 && ::fstatat(parent_fd_, leaf.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
          private_directory(named) && same(held, named);
}

void activation_state::verify() const {
   if (process_ != static_cast<std::uint64_t>(::getpid()))
      FORGE_THROW_EXCEPTION(exceptions::activation_incomplete, "activation capability belongs to another process");
   auto leaf = std::string{};
   {
      const auto lock = std::scoped_lock{mutex_};
      leaf = leaf_;
   }
   struct stat held{};
   struct stat named{};
   if (::fstat(parent_fd_, &held) != 0 || ::lstat(parent_.c_str(), &named) != 0 || !private_directory(held) ||
       !private_directory(named) || !same(held, named) || !matches(leaf))
      FORGE_THROW_EXCEPTION(exceptions::activation_incomplete, "activation directory ownership changed");
   if (::fstat(lock_fd_, &held) != 0 || ::fstatat(parent_fd_, lock_name, &named, AT_SYMLINK_NOFOLLOW) != 0 ||
       !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() || (held.st_mode & 0077U) != 0U || held.st_nlink != 1 ||
       !same(held, named))
      FORGE_THROW_EXCEPTION(exceptions::activation_incomplete, "activation lock ownership changed");
}

void activation_state::sync_root_and_parent() const {
   if (::fsync(root_fd_) != 0)
      io_error("cannot sync closed activation root", errno);
   if (::fsync(parent_fd_) != 0)
      io_error("cannot sync activation parent", errno);
}

void activation_state::mark_published(std::filesystem::path root, std::string leaf) {
   const auto lock = std::scoped_lock{mutex_};
   status_.root.swap(root);
   leaf_.swap(leaf);
   status_.published = true;
   status_.phase = activation_phase::published_unconfirmed;
}

void activation_state::publish(std::filesystem::path destination) {
   destination = absolute_path(std::move(destination));
   std::error_code error;
   const auto parent = std::filesystem::canonical(destination.parent_path(), error);
   struct stat named_parent{};
   struct stat held_parent{};
   if (error || ::lstat(parent.c_str(), &named_parent) != 0 || ::fstat(parent_fd_, &held_parent) != 0 ||
       !same(named_parent, held_parent))
      FORGE_THROW_EXCEPTION(exceptions::invalid_argument, "activation destination must share the held parent");
   auto target_leaf = destination.filename().string();
   auto target = parent_ / target_leaf; // All path allocation BEFORE publication.
   auto source = std::string{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (status_.published || status_.phase == activation_phase::publishing)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "closed root has already attempted publication");
      source = leaf_;
   }
   verify();
   struct stat existing{};
   if (::fstatat(parent_fd_, target_leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0)
      FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "activation destination already exists");
   const auto lookup_error = errno;
   if (lookup_error != ENOENT)
      io_error("cannot inspect activation destination", lookup_error);
   sync_root_and_parent();
   {
      const auto lock = std::scoped_lock{mutex_};
      status_.phase = activation_phase::publishing;
   }
   if (rename_no_replace(parent_fd_, source.c_str(), target_leaf.c_str()) != 0) {
      const auto code = errno;
      // An uncertain outcome never authorizes a second rename. Reconcile only
      // these two exact owned names, never search for a potentially reused inode.
      if (matches(target_leaf)) {
         mark_published(std::move(target), std::move(target_leaf));
         io_error("activation publication requires durability confirmation", code);
      }
      struct stat remaining{};
      const auto target_absent =
          ::fstatat(parent_fd_, target_leaf.c_str(), &remaining, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT;
      if (matches(source) && target_absent) {
         const auto lock = std::scoped_lock{mutex_};
         status_.phase = activation_phase::closed;
      }
      if (code == EEXIST || code == ENOTEMPTY)
         FORGE_THROW_EXCEPTION(exceptions::activation_conflict, "activation destination already exists",
                               forge::exceptions::ctx("native-code", code));
      if (code == ENOSYS || code == ENOTSUP || code == EINVAL)
         FORGE_THROW_EXCEPTION(exceptions::unsupported_activation, "native no-replace activation is unavailable",
                               forge::exceptions::ctx("native-code", code));
      io_error("cannot publish closed activation root", code);
   }
   mark_published(std::move(target), std::move(target_leaf));
   confirm();
}

void activation_state::confirm() {
   verify();
   sync_root_and_parent();
   verify();
   const auto lock = std::scoped_lock{mutex_};
   status_.phase = activation_phase::durable;
   // Initial-closed confirmation deliberately leaves published=false.
}

activation_status activation_state::snapshot() const {
   const auto lock = std::scoped_lock{mutex_};
   return status_;
}

} // namespace forge::plugins::db::store
