module;

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <forge/exceptions/macros.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

module forge.swarm.storage.file_store;

#include "details/file_layout.hxx"

namespace forge::swarm::storage::detail {
namespace {

constexpr std::size_t checkpoint_limit = 1024U * 1024U;
static_assert(std::numeric_limits<off_t>::max() >= core::max_file_size);

int checked(int result, const char* operation) {
   if (result >= 0) {
      return result;
   }
   const auto error = errno;
   if (error == EXDEV) {
      FORGE_THROW_EXCEPTION(exceptions::cross_device, "filesystem operation crosses devices",
                            forge::exceptions::ctx("operation", operation));
   }
   if (error == ELOOP || error == ENOTDIR) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "filesystem path is not a safe ordinary object",
                            forge::exceptions::ctx("operation", operation));
   }
   if (error == ENOENT) {
      FORGE_THROW_EXCEPTION(exceptions::missing_data, "filesystem object is missing",
                            forge::exceptions::ctx("operation", operation));
   }
   if (error == EEXIST || error == ENOTEMPTY || error == EWOULDBLOCK) {
      FORGE_THROW_EXCEPTION(exceptions::conflict, "filesystem object or ownership conflicts",
                            forge::exceptions::ctx("operation", operation));
   }
   FORGE_THROW_EXCEPTION(exceptions::io_error, "filesystem operation failed",
                         forge::exceptions::ctx("operation", operation), forge::exceptions::ctx("errno", error));
}

struct stat fd_stat(int fd) {
   struct stat state{};
   checked(::fstat(fd, &state), "fstat");
   return state;
}

void close_file_descriptor(int& fd) {
   checked(::close(std::exchange(fd, -1)), "close filesystem descriptor");
}

std::optional<struct stat> named_stat(int parent, const std::string& name) {
   struct stat state{};
   if (::fstatat(parent, name.c_str(), &state, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) {
         return std::nullopt;
      }
      checked(-1, "fstatat");
   }
   if (S_ISLNK(state.st_mode)) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "symlink is not a storage object");
   }
   return state;
}

bool same_inode(const struct stat& left, const struct stat& right) {
   return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

void same_named_inode(int parent, const std::string& name, const struct stat& held) {
   const auto named = named_stat(parent, name);
   if (!named) {
      FORGE_THROW_EXCEPTION(exceptions::missing_data, "opened filesystem object was removed");
   }
   if (!same_inode(*named, held)) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "filesystem object identity changed");
   }
}

void sync_fd(int fd) {
   auto result = int{};
   do {
      result = ::fsync(fd);
   } while (result < 0 && errno == EINTR);
   checked(result, "fsync");
}

int pin_parent(const std::filesystem::path& path) {
   auto current = checked(::open(path.is_absolute() ? "/" : ".",
                                 O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC), "open parent anchor");
   const auto close_current = boost::scope::scope_exit{[&] { if (current >= 0) ::close(current); }};
   for (const auto& component : path.relative_path()) {
      const auto name = component.native();
      if (name.empty() || name == ".") {
         continue;
      }
      if (name == ".." || name.find('\0') != std::string::npos) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "parent path contains traversal or NUL");
      }
      auto next = checked(::openat(current, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC),
                          "openat parent component");
      const auto close_next = boost::scope::scope_exit{[&] { if (next >= 0) ::close(next); }};
      const auto state = fd_stat(next);
      if (!S_ISDIR(state.st_mode)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "parent component is not a directory");
      }
      same_named_inode(current, name, state);
      close_file_descriptor(current);
      current = std::exchange(next, -1);
   }
   return std::exchange(current, -1);
}

void rename_exclusive(int parent, const std::string& from, const std::string& to) {
#if defined(__APPLE__)
   const auto result = ::renameatx_np(parent, from.c_str(), parent, to.c_str(), RENAME_EXCL);
#elif defined(__linux__) && defined(SYS_renameat2)
   const auto result = ::syscall(SYS_renameat2, parent, from.c_str(), parent, to.c_str(), RENAME_NOREPLACE);
#else
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "atomic no-overwrite rename is unsupported");
#endif
#if defined(__APPLE__) || (defined(__linux__) && defined(SYS_renameat2))
   if (result < 0 && (errno == ENOSYS || errno == ENOTSUP || errno == EINVAL)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "filesystem lacks atomic no-overwrite rename");
   }
   checked(static_cast<int>(result), "atomic publish rename");
#endif
}

void check_regular(const struct stat& state, std::uint64_t device) {
   if (static_cast<std::uint64_t>(state.st_dev) != device) {
      FORGE_THROW_EXCEPTION(exceptions::cross_device, "storage object is on a different filesystem");
   }
   if (!S_ISREG(state.st_mode) || state.st_nlink != 1 || state.st_size < 0) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "storage requires an unaliased ordinary file");
   }
}

void check_kind(const struct stat& state, bool directory, std::uint64_t device) {
   if (!directory) {
      check_regular(state, device);
      return;
   }
   if (static_cast<std::uint64_t>(state.st_dev) != device) {
      FORGE_THROW_EXCEPTION(exceptions::cross_device, "directory is on a different filesystem");
   }
   if (!S_ISDIR(state.st_mode) || state.st_nlink == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "storage requires a linked ordinary directory");
   }
}

} // namespace

file_layout::file_layout(const core::descriptor& descriptor, std::filesystem::path destination,
                         std::string storage_key, open_mode mode, bool external_authority, bool has_external_checkpoint)
    : _descriptor(descriptor), _seeded(mode == open_mode::seed),
      _has_external_checkpoint(external_authority) {
   if (mode != open_mode::create && mode != open_mode::resume && mode != open_mode::seed) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown file store open mode");
   }
   if (storage_key.empty() || storage_key.size() > 129U ||
       !std::all_of(storage_key.begin(), storage_key.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_';
       })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "storage key is not a bounded safe component");
   }
   _destination_name = destination.filename().native();
   if (_destination_name.empty() || _destination_name == "." || _destination_name == ".." ||
       _destination_name.find('\0') != std::string::npos) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "destination must have one ordinary basename");
   }
   _stage_name = ".forge-swarm-" + storage_key + ".part";
   _checkpoint_name = ".forge-swarm-" + storage_key + ".resume";
   _lock_name = ".forge-swarm-" + storage_key + ".lock";
   if (_destination_name == _stage_name || _destination_name == _checkpoint_name || _destination_name == _lock_name) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "destination aliases a storage control name");
   }
   try {
      _parent = pin_parent(destination.parent_path());
      const auto parent = fd_stat(_parent);
      _parent_identity = {static_cast<std::uint64_t>(parent.st_dev), static_cast<std::uint64_t>(parent.st_ino)};
      bool created_lock = true;
      auto previous_lock = std::optional<struct stat>{};
      _lock = ::openat(_parent, _lock_name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
      if (_lock < 0 && errno == EEXIST) {
         created_lock = false;
         previous_lock = named_stat(_parent, _lock_name);
         if (!previous_lock) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "ownership lock disappeared before open");
         }
         check_regular(*previous_lock, _parent_identity.device);
         _lock = ::openat(_parent, _lock_name.c_str(), O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
      }
      checked(_lock, "open ownership lock");
      const auto lock = fd_stat(_lock);
      check_regular(lock, _parent_identity.device);
      if (previous_lock && !same_inode(*previous_lock, lock)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "ownership lock changed during open");
      }
      if (lock.st_size != 0 || lock.st_uid != ::geteuid() || (!created_lock && (lock.st_mode & 0077) != 0)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "ownership lock is not a private empty owned file");
      }
      if (created_lock) {
         checked(::fchmod(_lock, 0600), "chmod ownership lock");
      }
      checked(::flock(_lock, LOCK_EX | LOCK_NB), "flock ownership lock");
      same_named_inode(_parent, _lock_name, lock);
      _lock_identity = {static_cast<std::uint64_t>(lock.st_dev), static_cast<std::uint64_t>(lock.st_ino)};

      const auto target = named_stat(_parent, _destination_name);
      const auto stage = named_stat(_parent, _stage_name);
      const auto checkpoint = named_stat(_parent, _checkpoint_name);
      if (_has_external_checkpoint && checkpoint) {
         FORGE_THROW_EXCEPTION(exceptions::conflict, "external checkpoint and sidecar cannot both be authorities");
      }
      if (mode == open_mode::create && (target || stage || checkpoint || has_external_checkpoint)) {
         FORGE_THROW_EXCEPTION(exceptions::conflict, "create refuses existing payload or checkpoint");
      }
      if (checkpoint) {
         check_regular(*checkpoint, _parent_identity.device);
         if (static_cast<std::uint64_t>(checkpoint->st_size) > checkpoint_limit) {
            FORGE_THROW_EXCEPTION(exceptions::corrupt_checkpoint, "checkpoint exceeds 1 MiB");
         }
         _checkpoint_identity = identity{static_cast<std::uint64_t>(checkpoint->st_dev),
                                         static_cast<std::uint64_t>(checkpoint->st_ino)};
      }
      bool create = false;
      if (_seeded) {
         if (stage) {
            FORGE_THROW_EXCEPTION(exceptions::conflict, "seed refuses a conflicting staging payload");
         }
         if (!target) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "seed destination is missing");
         }
         _published = true;
      } else if (mode == open_mode::create) {
         create = true;
      } else if (stage) {
         if (target) {
            FORGE_THROW_EXCEPTION(exceptions::conflict, "staging and destination both exist");
         }
      } else if (target) {
         if (!checkpoint && !has_external_checkpoint) {
            FORGE_THROW_EXCEPTION(exceptions::conflict, "published destination has no resume checkpoint");
         }
         _published = true;
      } else if (checkpoint || has_external_checkpoint) {
         FORGE_THROW_EXCEPTION(exceptions::missing_data, "checkpoint exists without its payload");
      } else {
         create = true;
      }

      const bool directory = _descriptor.files.size() != 1U;
      const auto& root_name = _published ? _destination_name : _stage_name;
      auto previous_root = std::optional<struct stat>{};
      if (!create) {
         previous_root = _published ? target : stage;
         if (!previous_root) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "payload root disappeared before open");
         }
         check_kind(*previous_root, directory, _parent_identity.device);
      }
      if (create && directory) {
         checked(::mkdirat(_parent, root_name.c_str(), 0700), "mkdir staging root");
         previous_root = named_stat(_parent, root_name);
         if (!previous_root) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "created staging root disappeared");
         }
         check_kind(*previous_root, true, _parent_identity.device);
      }
      const auto flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
          (directory ? O_RDONLY | O_DIRECTORY : _seeded ? O_RDONLY : O_RDWR) |
          (create && !directory ? O_CREAT | O_EXCL : 0);
      _root = checked(::openat(_parent, root_name.c_str(), flags, 0600), "open payload root");
      auto root = fd_stat(_root);
      if (previous_root && !same_inode(*previous_root, root)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload root changed during open");
      }
      if (static_cast<std::uint64_t>(root.st_dev) != _parent_identity.device) {
         FORGE_THROW_EXCEPTION(exceptions::cross_device, "payload root is not on the destination filesystem");
      }
      if (directory ? !S_ISDIR(root.st_mode) : !S_ISREG(root.st_mode)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload root has an unsafe object type");
      }
      same_named_inode(_parent, root_name, root);
      if (create) {
         checked(::fchmod(_root, directory ? 0700 : 0600), "chmod staging root");
         if (!directory) {
            checked(::ftruncate(_root, static_cast<off_t>(_descriptor.files.front().size)), "size staging payload");
         }
      }
      root = fd_stat(_root);
      if (same_inode(root, lock)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload root aliases the ownership lock");
      }
      _nodes.push_back({.name = {}, .parent = 0U,
                        .size = directory ? 0U : _descriptor.files.front().size,
                        .expected = {static_cast<std::uint64_t>(root.st_dev), static_cast<std::uint64_t>(root.st_ino)},
                        .directory = directory, .recorded = true});
      check_node(0U, _root);
      initialize_nodes(create);
      assert_current();
   } catch (...) {
      close();
      throw;
   }
}

file_layout::~file_layout() {
   close();
}

void file_layout::initialize_nodes(bool create) {
   if (!_nodes.front().directory) {
      _files.push_back(0U);
      return;
   }
   // Descriptor-derived topology only: no filesystem catalog enumeration and no retained per-file FDs.
   auto chain = std::array<std::uint32_t, core::max_path_depth>{};
   const std::vector<std::string>* previous = nullptr;
   for (const auto& file : _descriptor.files) {
      auto shared = std::size_t{0};
      if (previous != nullptr) {
         while (shared < std::min(previous->size(), file.path.size()) && (*previous)[shared] == file.path[shared]) {
            ++shared;
         }
      }
      auto parent = shared == 0U ? 0U : chain[shared - 1U];
      for (auto depth = shared; depth < file.path.size(); ++depth) {
         const auto index = static_cast<std::uint32_t>(_nodes.size());
         _nodes.push_back({.name = file.path[depth], .parent = parent,
                           .size = depth + 1U == file.path.size() ? file.size : 0U,
                           .expected = {}, .directory = depth + 1U != file.path.size()});
         chain[depth] = index;
         parent = index;
      }
      _files.push_back(parent);
      previous = &file.path;
   }
   auto identities = std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint32_t>{};
   identities.emplace(std::pair{_nodes.front().expected.device, _nodes.front().expected.inode}, 0U);
   for (auto index = std::uint32_t{1}; index < _nodes.size(); ++index) {
      auto fd = open_node(index, create);
      const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
      const auto state = fd_stat(fd);
      const auto key = std::pair{static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)};
      if (!identities.emplace(key, index).second) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "distinct logical names alias one filesystem inode");
      }
      _nodes[index].expected = {key.first, key.second};
      _nodes[index].recorded = true;
      close_file_descriptor(fd);
   }
}

void file_layout::check_node(std::uint32_t index, int fd) const {
   const auto& node = _nodes.at(index);
   const auto state = fd_stat(fd);
   if (static_cast<std::uint64_t>(state.st_dev) != _parent_identity.device) {
      FORGE_THROW_EXCEPTION(exceptions::cross_device, "payload object crosses the destination filesystem");
   }
   if (node.directory) {
      if (!S_ISDIR(state.st_mode)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload directory was replaced with another object type");
      }
   } else {
      check_regular(state, _parent_identity.device);
      if (static_cast<std::uint64_t>(state.st_size) != node.size) {
         FORGE_THROW_EXCEPTION(exceptions::missing_data, "payload file has the wrong exact size");
      }
   }
   if (node.recorded &&
       node.expected != identity{static_cast<std::uint64_t>(state.st_dev), static_cast<std::uint64_t>(state.st_ino)}) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload inode changed");
   }
}

int file_layout::open_node(std::uint32_t index, bool create) const {
   verify_root();
   if (index == 0U) {
      const auto& name = _published ? _destination_name : _stage_name;
      auto fd = checked(::openat(_parent, name.c_str(), O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
                                 (_nodes.front().directory ? O_RDONLY | O_DIRECTORY : _seeded ? O_RDONLY : O_RDWR)),
                        "reopen payload root");
      const auto close_file = boost::scope::scope_exit{[&] { if (fd >= 0) ::close(fd); }};
      check_node(0U, fd);
      same_named_inode(_parent, name, fd_stat(fd));
      return std::exchange(fd, -1);
   }
   auto ancestors = std::array<std::uint32_t, core::max_path_depth>{};
   auto depth = std::size_t{0};
   for (auto cursor = index; cursor != 0U; cursor = _nodes.at(cursor).parent) {
      if (depth == ancestors.size()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "payload topology exceeds descriptor depth");
      }
      ancestors[depth++] = cursor;
   }
   auto current = -1;
   const auto close_current = boost::scope::scope_exit{[&] { if (current >= 0) ::close(current); }};
   while (depth != 0U) {
      const auto cursor = ancestors[--depth];
      const auto& node = _nodes[cursor];
      const auto parent = current >= 0 ? current : _root;
      const bool creating = create && cursor == index;
      if (creating && node.directory && ::mkdirat(parent, node.name.c_str(), 0700) != 0) {
         if (errno == EEXIST) {
            FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "logical directory aliases an existing name");
         }
         checked(-1, "mkdir payload directory");
      }
      auto previous = std::optional<struct stat>{};
      if (!creating || node.directory) {
         previous = named_stat(parent, node.name);
         if (!previous) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "payload component is missing");
         }
         check_kind(*previous, node.directory, _parent_identity.device);
      }
      const auto flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
          (node.directory ? O_RDONLY | O_DIRECTORY : _seeded ? O_RDONLY : O_RDWR) |
          (creating && !node.directory ? O_CREAT | O_EXCL : 0);
      auto next = ::openat(parent, node.name.c_str(), flags, 0600);
      if (next < 0 && creating && errno == EEXIST) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "logical file aliases an existing name");
      }
      checked(next, "openat payload component");
      const auto close_next = boost::scope::scope_exit{[&] { if (next >= 0) ::close(next); }};
      const auto state = fd_stat(next);
      if (previous && !same_inode(*previous, state)) {
         FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "payload component changed during open");
      }
      if (creating) {
         check_kind(state, node.directory, _parent_identity.device);
         same_named_inode(parent, node.name, state);
         checked(::fchmod(next, node.directory ? 0700 : 0600), "chmod payload object");
         if (!node.directory) {
            checked(::ftruncate(next, static_cast<off_t>(node.size)), "size payload file");
         }
      }
      check_node(cursor, next);
      same_named_inode(parent, node.name, fd_stat(next));
      if (current >= 0) {
         close_file_descriptor(current);
      }
      current = std::exchange(next, -1);
   }
   return std::exchange(current, -1);
}

void file_layout::verify_root() const {
   if (_parent < 0 || _root < 0 || _lock < 0 || _nodes.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "file layout is closed or uninitialized");
   }
   const auto parent = fd_stat(_parent);
   if (!S_ISDIR(parent.st_mode) || parent.st_nlink == 0U ||
       _parent_identity != identity{static_cast<std::uint64_t>(parent.st_dev), static_cast<std::uint64_t>(parent.st_ino)}) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "pinned destination parent changed");
   }
   const auto lock = fd_stat(_lock);
   check_regular(lock, _parent_identity.device);
   if (_lock_identity != identity{static_cast<std::uint64_t>(lock.st_dev), static_cast<std::uint64_t>(lock.st_ino)}) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "ownership lock identity changed");
   }
   same_named_inode(_parent, _lock_name, lock);
   check_node(0U, _root);
   same_named_inode(_parent, _published ? _destination_name : _stage_name, fd_stat(_root));
}

void file_layout::assert_current() const {
   verify_root();
}

bool file_layout::published() const {
   return _published;
}

void file_layout::validate_range(std::uint32_t file, std::uint64_t offset, std::size_t length) const {
   verify_root();
   if (file >= _files.size() || length > core::max_piece_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "payload range exceeds file index or I/O bound");
   }
   const auto size = _descriptor.files[file].size;
   if (offset > size || length > size - offset) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "payload range exceeds the exact file length");
   }
}

std::vector<std::uint8_t> file_layout::read(std::uint32_t file, std::uint64_t offset, std::uint32_t length) const {
   validate_range(file, offset, length);
   auto bytes = std::vector<std::uint8_t>(length);
   {
      auto fd = open_node(_files[file]);
      const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
      auto done = std::size_t{0};
      while (done < bytes.size()) {
         const auto count = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
         if (count < 0 && errno == EINTR) continue;
         if (count < 0) checked(-1, "pread payload");
         if (count == 0) {
            FORGE_THROW_EXCEPTION(exceptions::missing_data, "payload ended before the requested range");
         }
         done += static_cast<std::size_t>(count);
      }
      check_node(_files[file], fd);
      close_file_descriptor(fd);
   }
   auto reopened = open_node(_files[file]);
   close_file_descriptor(reopened);
   return bytes;
}

void file_layout::write(std::uint32_t file, std::uint64_t offset, std::span<const std::uint8_t> bytes) {
   if (_seeded) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "seed layout is read-only");
   }
   validate_range(file, offset, bytes.size());
   {
      auto fd = open_node(_files[file]);
      const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
      auto done = std::size_t{0};
      while (done < bytes.size()) {
         const auto count = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(offset + done));
         if (count < 0 && errno == EINTR) continue;
         if (count < 0) checked(-1, "pwrite payload");
         if (count == 0) {
            FORGE_THROW_EXCEPTION(exceptions::io_error, "payload write made no progress");
         }
         done += static_cast<std::size_t>(count);
      }
      check_node(_files[file], fd);
      close_file_descriptor(fd);
   }
   auto reopened = open_node(_files[file]);
   close_file_descriptor(reopened);
}

void file_layout::sync_payload() {
   assert_current();
   for (const auto index : _files) {
      auto fd = open_node(index);
      const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
      sync_fd(fd);
      check_node(index, fd);
      close_file_descriptor(fd);
   }
   // Reverse topological order makes every nested directory durable before its parent/root.
   for (auto index = _nodes.size(); index-- > 1U;) {
      if (_nodes[index].directory) {
         auto fd = open_node(static_cast<std::uint32_t>(index));
         const auto close_directory = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
         sync_fd(fd);
         check_node(static_cast<std::uint32_t>(index), fd);
         close_file_descriptor(fd);
      }
   }
   if (_nodes.front().directory) {
      sync_fd(_root);
   }
   assert_current();
}

void file_layout::publish() {
   assert_current();
   if (_seeded || _published) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "payload is already published or seeded");
   }
   for (auto index = std::uint32_t{1}; index < _nodes.size(); ++index) {
      auto fd = open_node(index);
      close_file_descriptor(fd);
   }
   verify_root();
   rename_exclusive(_parent, _stage_name, _destination_name);
   _published = true;
   verify_root();
}

void file_layout::sync_parent() {
   assert_current();
   sync_fd(_parent);
   assert_current();
}

void file_layout::verify_checkpoint() const {
   const auto state = named_stat(_parent, _checkpoint_name);
   if (!state) {
      if (_checkpoint_identity) {
         FORGE_THROW_EXCEPTION(exceptions::missing_data, "checkpoint was removed");
      }
      return;
   }
   check_regular(*state, _parent_identity.device);
   if (!_checkpoint_identity || *_checkpoint_identity !=
       identity{static_cast<std::uint64_t>(state->st_dev), static_cast<std::uint64_t>(state->st_ino)}) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "checkpoint identity changed");
   }
   if (static_cast<std::uint64_t>(state->st_size) > checkpoint_limit) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_checkpoint, "checkpoint exceeds 1 MiB");
   }
}

std::optional<std::vector<std::uint8_t>> file_layout::read_checkpoint() const {
   assert_current();
   if (_has_external_checkpoint) {
      return std::nullopt;
   }
   verify_checkpoint();
   if (!_checkpoint_identity) {
      return std::nullopt;
   }
   auto fd = checked(::openat(_parent, _checkpoint_name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC),
                           "open checkpoint");
   const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
   const auto before = fd_stat(fd);
   check_regular(before, _parent_identity.device);
   if (*_checkpoint_identity != identity{static_cast<std::uint64_t>(before.st_dev), static_cast<std::uint64_t>(before.st_ino)}) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "opened checkpoint identity changed");
   }
   if (static_cast<std::uint64_t>(before.st_size) > checkpoint_limit) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_checkpoint, "checkpoint exceeds 1 MiB");
   }
   auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(before.st_size));
   auto done = std::size_t{0};
   while (done < bytes.size()) {
      const auto count = ::pread(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) checked(-1, "pread checkpoint");
      if (count == 0) {
         FORGE_THROW_EXCEPTION(exceptions::corrupt_checkpoint, "checkpoint was truncated during read");
      }
      done += static_cast<std::size_t>(count);
   }
   const auto after = fd_stat(fd);
   check_regular(after, _parent_identity.device);
   if (!same_inode(before, after) || before.st_size != after.st_size) {
      FORGE_THROW_EXCEPTION(exceptions::corrupt_checkpoint, "checkpoint changed during read");
   }
   same_named_inode(_parent, _checkpoint_name, after);
   assert_current();
   close_file_descriptor(fd);
   return bytes;
}

void file_layout::write_checkpoint(std::span<const std::uint8_t> bytes) {
   assert_current();
   if (_has_external_checkpoint) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "externally checkpointed layout has no writable sidecar");
   }
   if (bytes.size() > checkpoint_limit) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "checkpoint exceeds 1 MiB");
   }
   verify_checkpoint();
   static auto next_temporary = std::atomic<std::uint64_t>{0};
   auto temporary = std::string{};
   auto fd = -1;
   for (auto attempt = 0U; attempt < 16U; ++attempt) {
      temporary = _checkpoint_name + ".tmp-" + std::to_string(::getpid()) + "-" +
          std::to_string(next_temporary.fetch_add(1U, std::memory_order_relaxed));
      fd = ::openat(_parent, temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
      if (fd >= 0) break;
      if (errno != EEXIST) checked(-1, "create checkpoint temporary");
   }
   if (fd < 0) {
      FORGE_THROW_EXCEPTION(exceptions::conflict, "checkpoint temporary names are occupied");
   }
   const auto close_file = boost::scope::scope_exit{[&fd] { if (fd >= 0) ::close(fd); }};
   const auto initial = fd_stat(fd);
   bool renamed = false;
   const auto cleanup = boost::scope::scope_exit{[&] {
      if (!renamed) {
         struct stat named{};
         if (::fstatat(_parent, temporary.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 && same_inode(initial, named)) {
            ::unlinkat(_parent, temporary.c_str(), 0);
         }
      }
   }};
   check_regular(initial, _parent_identity.device);
   checked(::fchmod(fd, 0600), "chmod checkpoint temporary");
   auto done = std::size_t{0};
   while (done < bytes.size()) {
      const auto count = ::pwrite(fd, bytes.data() + done, bytes.size() - done, static_cast<off_t>(done));
      if (count < 0 && errno == EINTR) continue;
      if (count < 0) checked(-1, "pwrite checkpoint temporary");
      if (count == 0) {
         FORGE_THROW_EXCEPTION(exceptions::io_error, "checkpoint write made no progress");
      }
      done += static_cast<std::size_t>(count);
   }
   sync_fd(fd);
   const auto final = fd_stat(fd);
   check_regular(final, _parent_identity.device);
   if (!same_inode(initial, final) || static_cast<std::uint64_t>(final.st_size) != bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::unsafe_path, "checkpoint temporary identity or length changed");
   }
   same_named_inode(_parent, temporary, final);
   checked(::close(std::exchange(fd, -1)), "close checkpoint temporary");
   assert_current();
   verify_checkpoint();
   if (_checkpoint_identity) {
      checked(::renameat(_parent, temporary.c_str(), _parent, _checkpoint_name.c_str()), "replace checkpoint");
   } else {
      rename_exclusive(_parent, temporary, _checkpoint_name);
   }
   renamed = true;
   _checkpoint_identity = identity{static_cast<std::uint64_t>(final.st_dev), static_cast<std::uint64_t>(final.st_ino)};
   sync_fd(_parent);
   verify_checkpoint();
   assert_current();
}

void file_layout::close_checked() {
   auto error = 0;
   for (auto* fd : std::array{&_root, &_lock, &_parent}) {
      if (*fd >= 0 && ::close(std::exchange(*fd, -1)) != 0 && error == 0) { error = errno; }
   }
   if (error != 0) {
      errno = error;
      checked(-1, "close storage resource");
   }
}

void file_layout::close() noexcept {
   if (_root >= 0) ::close(std::exchange(_root, -1));
   if (_lock >= 0) ::close(std::exchange(_lock, -1));
   if (_parent >= 0) ::close(std::exchange(_parent, -1));
}

} // namespace forge::swarm::storage::detail
