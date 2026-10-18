module;

#include <coroutine>
#include <atomic>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <new>
#include <stop_token>
#include <string>
#include <sys/stat.h>
#include <sys/file.h>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.compiler_session;

import forge.codec.hex;
import forge.crypto.core.random;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"

namespace forge::vm::wasm::optimizing {
namespace {

int open_cache(const std::filesystem::path& path) {
   if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache directory is unavailable");
   const auto fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache directory cannot be opened");
   struct stat state{};
   if (::fstat(fd, &state) != 0 || !S_ISDIR(state.st_mode) || state.st_uid != ::geteuid() ||
       (state.st_mode & 0077) != 0) {
      ::close(fd);
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache must be a private owned directory");
   }
   return fd;
}

std::string random_name(const std::string& prefix) {
   try {
      return prefix + codec::hex::encode(crypto::core::random_array<12>());
   } catch (const crypto::core::random::exceptions::backend_error&) {
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache temporary randomness is unavailable");
   }
}

} // namespace

void compiler_session::impl::close_cache() noexcept {
   // Never unlink/recreate the lifetime lock file. Closing the single owning
   // open file description releases flock only after all tracked work drains.
   if (ownership_fd >= 0)
      ::close(std::exchange(ownership_fd, -1));
   if (directory_fd >= 0)
      ::close(std::exchange(directory_fd, -1));
   disk_count = 0;
   disk_bytes = 0;
   disk_faulted = false;
}

void compiler_session::impl::verify_cache_ownership() const {
   struct stat directory{}, path{}, held{}, current{};
   if (disk_faulted || directory_fd < 0 || ownership_fd < 0 || ::fstat(directory_fd, &directory) != 0 ||
       ::lstat(options.cache.c_str(), &path) != 0 || directory.st_dev != path.st_dev ||
       directory.st_ino != path.st_ino || !S_ISDIR(path.st_mode) || path.st_uid != ::geteuid() ||
       (path.st_mode & 0077) != 0 || ::fstat(ownership_fd, &held) != 0 ||
       ::fstatat(directory_fd, "owner.lock", &current, AT_SYMLINK_NOFOLLOW) != 0 || held.st_dev != current.st_dev ||
       held.st_ino != current.st_ino || !S_ISREG(held.st_mode) || held.st_uid != ::geteuid() ||
       (held.st_mode & 0077) != 0 || held.st_nlink != 1 || held.st_size != 0)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache ownership changed");
}

bool compiler_session::impl::cache_entry_matches(std::size_t slot, int fd) const noexcept {
   const auto& entry = disk[slot];
   struct stat state{};
   const auto result = fd >= 0 ? ::fstat(fd, &state) : ::fstatat(directory_fd, entry.name, &state, AT_SYMLINK_NOFOLLOW);
   return result == 0 && S_ISREG(state.st_mode) && state.st_uid == ::geteuid() && (state.st_mode & 0077) == 0 &&
          state.st_nlink == 1 && state.st_size >= 0 && static_cast<std::uint64_t>(state.st_dev) == entry.device &&
          static_cast<std::uint64_t>(state.st_ino) == entry.inode &&
          static_cast<std::uint64_t>(state.st_size) == entry.bytes;
}

std::optional<std::vector<std::uint8_t>> compiler_session::impl::read_cache_entry(std::size_t index_value,
                                                                                  asio::compute::context& context) {
   const auto& entry = disk[index_value];
   const auto fd = ::openat(directory_fd, entry.name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
   if (fd < 0) {
      disk_faulted = true; // catalog owns this named file; unconfirmed external removal is not released capacity
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache entry cannot be opened");
   }
   const auto close_file = boost::scope::scope_exit{[fd] { ::close(fd); }};
   if (!cache_entry_matches(index_value, fd)) {
      disk_faulted = true;
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache selected entry ownership changed");
   }
   if (entry.bytes == 0 || entry.bytes > options.max_response_bytes)
      return {};
   auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(entry.bytes));
   auto offset = std::size_t{};
   while (offset < bytes.size()) {
      context.throw_if_stop_requested();
      const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR)
         continue;
      if (count <= 0)
         return {};
      offset += static_cast<std::size_t>(count);
   }
   auto extra = std::uint8_t{};
   auto count = ssize_t{};
   do {
      context.throw_if_stop_requested();
      count = ::read(fd, &extra, 1);
   } while (count < 0 && errno == EINTR);
   if (count != 0)
      return {};
   if (!cache_entry_matches(index_value, fd) || !cache_entry_matches(index_value)) {
      disk_faulted = true;
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache selected entry changed during read");
   }
   return bytes; // file FD closes BEFORE any async prepare/mapping handoff
}

void compiler_session::impl::scan_cache(const target_profile& expected, asio::compute::context& context) {
   verify_cache_ownership();
   // New open description: dup() would share/reuse a directory seek position.
   const auto scan_fd = ::openat(directory_fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
   if (scan_fd < 0)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache scan cannot be opened");
   const auto directory = ::fdopendir(scan_fd);
   if (directory == nullptr) {
      ::close(scan_fd);
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache scan cannot be created");
   }
   const auto close_scan = boost::scope::scope_exit{[directory] { ::closedir(directory); }};
   disk_count = 0;
   disk_bytes = 0;
   auto visited = std::size_t{};
   for (;;) {
      context.throw_if_stop_requested();
      errno = 0;
      const auto item = ::readdir(directory);
      if (item == nullptr) {
         if (errno != 0)
            FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache enumeration failed");
         break;
      }
      if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0)
         continue; // exactly two known directory mechanics, not user entries
      if (visited == options.max_disk_entries + 2)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler cache cold scan exceeds finite entry work bound");
      ++visited; // ALL entries, including lock/unknown/symlink/nonregular, consume scan work
      if (std::strcmp(item->d_name, "owner.lock") == 0)
         continue; // one verified zero-byte, fixed lifetime metadata file
      if (disk_count == options.max_disk_entries + 1)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler cache has too many cold entries");
      const auto length = std::strlen(item->d_name);
      if (length >= sizeof(disk_entry::name))
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache filename exceeds fixed catalog slot");
      struct stat state{};
      if (::fstatat(directory_fd, item->d_name, &state, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(state.st_mode) ||
          state.st_uid != ::geteuid() || (state.st_mode & 0077) != 0 || state.st_nlink != 1 || state.st_size < 0)
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache contains an unknown unsafe entry");
      auto& entry = disk[disk_count];
      entry = disk_entry{};
      std::memcpy(entry.name, item->d_name, length + 1);
      entry.bytes = static_cast<std::uint64_t>(state.st_size);
      entry.device = static_cast<std::uint64_t>(state.st_dev);
      entry.inode = static_cast<std::uint64_t>(state.st_ino);
#if defined(__APPLE__)
      entry.modified_seconds = state.st_mtimespec.tv_sec;
      entry.modified_nanoseconds = state.st_mtimespec.tv_nsec;
#else
      entry.modified_seconds = state.st_mtim.tv_sec;
      entry.modified_nanoseconds = state.st_mtim.tv_nsec;
#endif
      if (entry.bytes > UINT64_MAX - disk_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler cache byte accounting overflow");
      disk_bytes += entry.bytes; // opaque/orphan/corrupt bytes are NEVER silently ignored
      const auto slot = disk_count++;
      if (length != 68 || std::strcmp(entry.name + 64, ".foc") != 0)
         continue; // charged opaque file/orphan: never delete/adopt another temporary
      auto identity = crypto::digest::sha256{};
      try {
         const auto name = std::string{entry.name, 64};
         identity = crypto::digest::sha256{name}; // sole existing hex/digest owner
         if (identity.str() != name)
            continue;
      } catch (const std::bad_alloc&) {
         throw;
      } catch (...) {
         continue; // not a canonical cache filename; charged/preserved opaque
      }
      entry.status = disk_entry::state::quarantined;
      // Logical quarantine in this SAME bounded catalog, no sidecar/format.
      // On restart every candidate is revalidated before becoming load-eligible.
      const auto bytes = read_cache_entry(slot, context);
      if (!bytes)
         continue;
      try {
         const auto code = artifact::decode(*bytes, identity, options.max_response_bytes);
         if (code.image().configuration.target == expected)
            entry.status = disk_entry::state::candidate;
      } catch (const exceptions::invalid_artifact&) {
      } catch (const exceptions::unsupported_target&) {
      } catch (const exceptions::unsupported_import&) {
      } catch (const exceptions::invalid_module&) {
      } catch (const exceptions::compile_limit&) {
      }
   }
   // Enumeration/validation completes BEFORE any eviction or other mutation.
}

void compiler_session::impl::remove_cache_entry(std::size_t slot, asio::compute::context& context) {
   context.throw_if_stop_requested();
   verify_cache_ownership();
   const auto& entry = disk[slot];
   if (entry.status == disk_entry::state::opaque)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "opaque compiler cache entries cannot be evicted");
   if (!cache_entry_matches(slot) || ::unlinkat(directory_fd, entry.name, 0) != 0 || ::fsync(directory_fd) != 0) {
      disk_faulted = true;
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache eviction could not be confirmed");
   }
   // Confirmed unlink+namespace sync; failed unlink NEVER releases its charge.
   disk_bytes -= entry.bytes;
   disk[slot] = disk[--disk_count];
}

void compiler_session::impl::require_cache_space(std::uint64_t bytes, std::size_t entries,
                                                 asio::compute::context& context, const char* preserved) {
   if (bytes > options.max_disk_bytes || entries > options.max_disk_entries)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler cache publication exceeds finite quota");
   while (disk_bytes > options.max_disk_bytes - bytes || disk_count > options.max_disk_entries - entries) {
      auto victim = disk_count;
      for (std::size_t i = 0; i < disk_count; ++i) {
         if (disk[i].status == disk_entry::state::opaque)
            continue;
         if (preserved != nullptr && std::strcmp(disk[i].name, preserved) == 0)
            continue; // replacement cannot evade old + temp admission by deleting its destination
         if (victim == disk_count ||
             (disk[i].status == disk_entry::state::quarantined && disk[victim].status != disk[i].status) ||
             (disk[i].status == disk[victim].status &&
              std::tie(disk[i].modified_seconds, disk[i].modified_nanoseconds) <
                  std::tie(disk[victim].modified_seconds, disk[victim].modified_nanoseconds)) ||
             (disk[i].status == disk[victim].status && disk[i].modified_seconds == disk[victim].modified_seconds &&
              disk[i].modified_nanoseconds == disk[victim].modified_nanoseconds &&
              std::strcmp(disk[i].name, disk[victim].name) < 0))
            victim = i;
      }
      if (victim == disk_count)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "opaque compiler cache bytes prevent bounded publication");
      remove_cache_entry(victim, context);
   }
}

void compiler_session::impl::initialize_cache(const target_profile& expected, asio::compute::context& context) {
   const auto lock = std::scoped_lock{disk_mutex}; // compute only, never index/Asio/writer
   context.throw_if_stop_requested();
   directory_fd = open_cache(options.cache);
   ownership_fd = ::openat(directory_fd, "owner.lock", O_CREAT | O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (ownership_fd < 0)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache lifetime lock cannot be opened");
   verify_cache_ownership();
   if (::flock(ownership_fd, LOCK_EX | LOCK_NB) != 0)
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache is owned by another session");
   verify_cache_ownership(); // SAME existing keystore exclusive-path ownership pattern
   scan_cache(expected, context);
   const auto name = random_name("preflight.tmp-");
   require_cache_space(0, 1, context);
   auto& reservation = disk[disk_count++];
   reservation = disk_entry{};
   std::memcpy(reservation.name, name.c_str(), name.size() + 1);
   const auto slot = disk_count - 1; // reserve the live zero-byte temporary slot BEFORE creation
   const auto fd = ::openat(directory_fd, name.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_NOFOLLOW | O_CLOEXEC, 0600);
   if (fd < 0) {
      --disk_count; // no owned file was created; existing colliding entries are untouched
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache writable preflight failed");
   }
   const auto closed = ::close(fd) == 0;
   if (!closed || ::unlinkat(directory_fd, name.c_str(), 0) != 0 || ::fsync(directory_fd) != 0) {
      disk_faulted = true;
      FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache writable preflight cleanup failed");
   }
   disk[slot] = disk[--disk_count]; // only confirmed creation/close/unlink/sync releases reservation
   require_cache_space(0, 0, context);
}

void compiler_session::impl::touch_cache_entry(const crypto::digest::sha256& identity,
                                               asio::compute::context& context) {
   const auto lock = std::scoped_lock{disk_mutex};
   context.throw_if_stop_requested();
   verify_cache_ownership();
   const auto name = identity.str() + ".foc";
   for (std::size_t i = 0; i < disk_count; ++i) {
      auto& entry = disk[i];
      if (std::strcmp(entry.name, name.c_str()) != 0 || entry.status != disk_entry::state::candidate)
         continue;
      const auto fd = ::openat(directory_fd, entry.name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (fd < 0) {
         disk_faulted = true;
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache recency entry cannot be opened");
      }
      const auto close_file = boost::scope::scope_exit{[fd] { ::close(fd); }};
      if (!cache_entry_matches(i, fd)) {
         disk_faulted = true;
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache recency ownership changed");
      }
      if (::futimens(fd, nullptr) != 0)
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache recency update failed");
      struct stat state{};
      if (::fstat(fd, &state) != 0)
         FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache recency readback failed");
#if defined(__APPLE__)
      entry.modified_seconds = state.st_mtimespec.tv_sec;
      entry.modified_nanoseconds = state.st_mtimespec.tv_nsec;
#else
      entry.modified_seconds = state.st_mtim.tv_sec;
      entry.modified_nanoseconds = state.st_mtim.tv_nsec;
#endif
      return;
   }
}

boost::asio::awaitable<std::optional<artifact>> compiler_session::impl::load(std::shared_ptr<job> record,
                                                                             std::stop_token stop) {
   auto bytes = co_await compute.execute(
       asio::compute::task_options{.name = "wasm-cache-read", .parent_stop_token = stop},
       [this, record](asio::compute::context& context) -> std::optional<std::vector<std::uint8_t>> {
          const auto lock = std::scoped_lock{disk_mutex};
          context.throw_if_stop_requested();
          verify_cache_ownership();
          // initialize performed the single bounded reconciliation. Under the
          // whole-directory lock the SAME catalog owns subsequent accounting;
          // no unrelated artifact is read/decoded for this requested identity.
          require_cache_space(0, 0, context);
          const auto name = record->verified->str() + ".foc";
          for (std::size_t i = 0; i < disk_count; ++i) {
             if (std::strcmp(disk[i].name, name.c_str()) == 0 && disk[i].status == disk_entry::state::candidate)
                return read_cache_entry(i, context);
          }
          return {}; // absent/quarantined NEVER establishes ready or poisons the key
       });
   if (!bytes)
      co_return std::nullopt;
   auto code = std::optional<artifact>{};
   auto rejected = false;
   try {
      code.emplace(co_await prepare(record, std::move(*bytes), stop, artifact::origin::owned_cache));
   } catch (const exceptions::invalid_artifact&) {
      rejected = true;
   } catch (const exceptions::unsupported_target&) {
      rejected = true;
   }
   if (rejected) {
      co_await compute.execute(
          asio::compute::task_options{.name = "wasm-cache-quarantine", .parent_stop_token = stop},
          [this, record](asio::compute::context& context) {
             const auto lock = std::scoped_lock{disk_mutex};
             context.throw_if_stop_requested();
             verify_cache_ownership();
             const auto name = record->verified->str() + ".foc";
             for (std::size_t i = 0; i < disk_count; ++i) {
                if (std::strcmp(disk[i].name, name.c_str()) != 0)
                   continue;
                if (!cache_entry_matches(i)) {
                   disk_faulted = true;
                   FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache rejected entry ownership changed");
                }
                disk[i].status = disk_entry::state::quarantined;
                return; // keep SAME charged bytes/slot; rebuild still reserves old + full new temporary
             }
             // Another owned job may already have confirmed eviction of this
             // candidate; do not invent a new entry, charge or negative cache.
          });
      co_return std::nullopt;
   }
   co_await compute.execute(
       asio::compute::task_options{.name = "wasm-cache-touch", .parent_stop_token = stop},
       [this, record](asio::compute::context& context) { touch_cache_entry(*record->verified, context); });
   co_return std::move(code);
}

boost::asio::awaitable<void> compiler_session::impl::persist(std::shared_ptr<job> record, std::stop_token stop) {
   co_await compute.execute(
       asio::compute::task_options{.name = "wasm-cache-publish", .parent_stop_token = stop},
       [this, record](asio::compute::context& context) {
          context.throw_if_stop_requested();
          const auto& code = *record->prepared; // charged SAME job, no detached mapping pin
          if (code.identity() != *record->verified || code.image().configuration != record->request->configuration)
             FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "cache publication identity/configuration mismatch");
          auto bytes = code.encode(); // sole existing artifact codec
          if (bytes.size() > options.max_response_bytes)
             FORGE_THROW_EXCEPTION(exceptions::compile_limit, "cache artifact exceeds reserved response budget");
          const auto name = record->verified->str() + ".foc";
          const auto temporary = random_name(name + ".tmp-");
          const auto lock = std::scoped_lock{disk_mutex};
          verify_cache_ownership(); // same authoritative catalog, no whole-cache reread per publication
          require_cache_space(bytes.size(), 1, context, name.c_str());
          auto old_slot = disk_count;
          for (std::size_t i = 0; i < disk_count; ++i)
             if (std::strcmp(disk[i].name, name.c_str()) == 0) {
                old_slot = i;
                break;
             }
          const auto slot = disk_count++;
          auto& reservation = disk[slot];
          reservation = disk_entry{};
          std::memcpy(reservation.name, temporary.c_str(), temporary.size() + 1);
          reservation.bytes = bytes.size();
          disk_bytes += bytes.size(); // checked require_cache_space reserved old + FULL new temp
          auto fd =
              ::openat(directory_fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
          if (fd < 0) {
             disk_bytes -= reservation.bytes;
             --disk_count; // no new owned file was created; never unlink a colliding unknown
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache temporary cannot be created");
          }
          auto published = false;
          auto closed = false;
          const auto cleanup = boost::scope::scope_exit{[&] {
             if (fd >= 0) {
                closed = ::close(fd) == 0;
                fd = -1; // never retry uncertain close against a potentially reused FD
                if (!closed)
                   disk_faulted = true;
             }
             if (!published) {
                if (closed && ::unlinkat(directory_fd, temporary.c_str(), 0) == 0 && ::fsync(directory_fd) == 0) {
                   disk_bytes -= disk[slot].bytes;
                   disk[slot] = disk[--disk_count];
                } else {
                   // Actual orphan stays charged. Block subsequent writes/scans
                   // until owner shutdown; never fabricate released capacity.
                   disk_faulted = true;
                }
             }
          }};
          auto offset = std::size_t{};
          while (offset < bytes.size()) {
             context.throw_if_stop_requested();
             const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
             if (count < 0 && errno == EINTR)
                continue;
             if (count <= 0)
                FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache temporary write failed");
             offset += static_cast<std::size_t>(count);
          }
          context.throw_if_stop_requested();
          if (::fsync(fd) != 0)
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache temporary sync failed");
          context.throw_if_stop_requested();
          struct stat destination{};
          const auto found = ::fstatat(directory_fd, name.c_str(), &destination, AT_SYMLINK_NOFOLLOW);
          if ((old_slot < slot && !cache_entry_matches(old_slot)) ||
              (old_slot == slot && (found == 0 || errno != ENOENT))) {
             disk_faulted = true;
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache destination ownership changed");
          }
          if (::renameat(directory_fd, temporary.c_str(), directory_fd, name.c_str()) != 0)
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache atomic replacement failed");
          published = true;
          // Still account BOTH old destination + new temporary until durable
          // namespace publication AND descriptor close are confirmed.
          struct stat published_state{};
          if (::fsync(directory_fd) != 0 || ::fstat(fd, &published_state) != 0) {
             disk_faulted = true;
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache durable replacement failed");
          }
          closed = ::close(fd) == 0;
          fd = -1;
          if (!closed) {
             disk_faulted = true;
             FORGE_THROW_EXCEPTION(exceptions::cache_io, "compiler cache published descriptor close failed");
          }
          auto result = disk[slot];
          std::memcpy(result.name, name.c_str(), name.size() + 1);
          result.device = static_cast<std::uint64_t>(published_state.st_dev);
          result.inode = static_cast<std::uint64_t>(published_state.st_ino);
          result.status = disk_entry::state::candidate;
#if defined(__APPLE__)
          result.modified_seconds = published_state.st_mtimespec.tv_sec;
          result.modified_nanoseconds = published_state.st_mtimespec.tv_nsec;
#else
          result.modified_seconds = published_state.st_mtim.tv_sec;
          result.modified_nanoseconds = published_state.st_mtim.tv_nsec;
#endif
          if (old_slot < slot) {
             disk_bytes -= disk[old_slot].bytes;
             disk[old_slot] = result;
             --disk_count;
          } else {
             disk[slot] = result;
          }
       });
}

} // namespace forge::vm::wasm::optimizing
