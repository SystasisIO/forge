#include "io_faults.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <dlfcn.h>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace swarm_test::faults {

struct scope::state {
   explicit state(plan value) : config(value) {}
   plan config;
   std::atomic<std::uint64_t> syncs{0}, writes{0}, reads{0}, errors{0};
   std::atomic<std::uint64_t> closes{0};
   std::atomic<std::uint64_t> renames{0};
   std::atomic<bool> rename_failed{false}, rename_paused{false};
   std::atomic<std::uint64_t> paused{0};
   std::atomic<bool> timed_out{false};
   std::atomic<bool> parent_selected{false};
   std::mutex mutex;
   std::condition_variable changed;
   bool released = false;
};

} // namespace swarm_test::faults

namespace {

using swarm_test::faults::scope;
using sync_function = int (*)(int);
using write_function = ssize_t (*)(int, const void*, std::size_t, off_t);
using read_function = ssize_t (*)(int, void*, std::size_t, off_t);
using close_function = int (*)(int);

sync_function actual_sync() {
   static const auto value = reinterpret_cast<sync_function>(::dlsym(RTLD_NEXT, "fsync"));
   return value;
}
write_function actual_write() {
   static const auto value = reinterpret_cast<write_function>(::dlsym(RTLD_NEXT, "pwrite"));
   return value;
}
read_function actual_read() {
   static const auto value = reinterpret_cast<read_function>(::dlsym(RTLD_NEXT, "pread"));
   return value;
}
close_function actual_close() {
   static const auto value = reinterpret_cast<close_function>(::dlsym(RTLD_NEXT, "close"));
   return value;
}

#if defined(__APPLE__)
using rename_function = int (*)(int, const char*, int, const char*, unsigned int);
rename_function actual_rename() {
   static const auto value = reinterpret_cast<rename_function>(::dlsym(RTLD_NEXT, "renameatx_np"));
   return value;
}
#endif

std::atomic<bool> armed{false};
std::mutex active_mutex;
std::shared_ptr<scope::state> active;

std::shared_ptr<scope::state> current() {
   if (!armed.load(std::memory_order_acquire)) return {};
   const auto lock = std::scoped_lock{active_mutex};
   return active;
}

bool pause(const std::shared_ptr<scope::state>& state) {
   auto lock = std::unique_lock{state->mutex};
   state->paused.fetch_add(1U);
   state->changed.notify_all();
   if (!state->changed.wait_for(lock, std::chrono::seconds{5}, [&] { return state->released; })) {
      state->timed_out.store(true);
      state->changed.notify_all();
      errno = ETIMEDOUT;
      return false;
   }
   return true;
}

bool published_parent(int fd) {
   struct stat held{}, target{};
   return ::fstat(fd, &held) == 0 && S_ISDIR(held.st_mode) &&
          ::fstatat(fd, "result", &target, AT_SYMLINK_NOFOLLOW) == 0;
}

bool selected_close(int fd, swarm_test::faults::close_target target) {
   using swarm_test::faults::close_target;
   if (target == close_target::any) return true;
   auto path = std::array<char, 4096>{};
#if defined(__APPLE__)
   if (::fcntl(fd, F_GETPATH, path.data()) != 0) return false;
#else
   auto descriptor = std::array<char, 64>{};
   const auto size = std::snprintf(descriptor.data(), descriptor.size(), "/proc/self/fd/%d", fd);
   if (size <= 0 || static_cast<std::size_t>(size) >= descriptor.size()) return false;
   const auto length = ::readlink(descriptor.data(), path.data(), path.size() - 1U);
   if (length < 0) return false;
   path[static_cast<std::size_t>(length)] = '\0';
#endif
   auto name = std::string_view{path.data()};
   const auto slash = name.find_last_of('/');
   if (slash != std::string_view::npos) name.remove_prefix(slash + 1U);
   if (!name.starts_with(".forge-swarm-")) return false;
   return target == close_target::ownership_lock ? name.ends_with(".lock")
       : name.find(".resume.tmp-") != std::string_view::npos;
}

} // namespace

// Linked only into this test executable; no production code calls or knows about these wrappers.
extern "C" int fsync(int fd) {
   const auto saved_errno = errno;
   const auto original = actual_sync();
   if (!original) { errno = ENOSYS; return -1; }
   const auto state = current();
   auto ordinal = std::uint64_t{0};
   if (state) {
      ordinal = state->syncs.fetch_add(1U) + 1U;
      if (ordinal == state->config.crash_fsync && state->config.edge == swarm_test::faults::crash_edge::before) {
         ::_exit(77);
      }
      if (ordinal == state->config.fsync_eio) {
         state->errors.fetch_add(1U); errno = EIO; return -1;
      }
      if (state->config.pause_published_parent && published_parent(fd) && !state->parent_selected.exchange(true)) {
         if (!pause(state)) return -1;
      }
   }
   errno = saved_errno;
   const auto result = original(fd);
   if (state && result == 0 && ordinal == state->config.crash_fsync &&
       state->config.edge == swarm_test::faults::crash_edge::after) {
      ::_exit(77);
   }
   return result;
}

extern "C" ssize_t pwrite(int fd, const void* bytes, std::size_t size, off_t offset) {
   const auto saved_errno = errno;
   const auto original = actual_write();
   if (!original) { errno = ENOSYS; return -1; }
   const auto state = current();
   if (state) {
      const auto ordinal = state->writes.fetch_add(1U) + 1U;
      if (ordinal == state->config.write_enospc || ordinal == state->config.write_eintr) {
         state->errors.fetch_add(1U);
         errno = ordinal == state->config.write_enospc ? ENOSPC : EINTR;
         return -1;
      }
      if (ordinal <= state->config.pause_first_writes && !pause(state)) return -1;
      if (state->config.short_write != 0U) size = std::min(size, state->config.short_write);
   }
   errno = saved_errno;
   return original(fd, bytes, size, offset);
}

extern "C" ssize_t pread(int fd, void* bytes, std::size_t size, off_t offset) {
   const auto saved_errno = errno;
   const auto original = actual_read();
   if (!original) { errno = ENOSYS; return -1; }
   const auto state = current();
   if (state) {
      const auto ordinal = state->reads.fetch_add(1U) + 1U;
      if (ordinal == state->config.read_eintr) {
         state->errors.fetch_add(1U); errno = EINTR; return -1;
      }
      if (state->config.short_read != 0U) size = std::min(size, state->config.short_read);
   }
   errno = saved_errno;
   return original(fd, bytes, size, offset);
}

extern "C" int close(int fd) {
   const auto saved_errno = errno;
   const auto original = actual_close();
   if (!original) { errno = ENOSYS; return -1; }
   const auto state = current();
   const auto selected = state && selected_close(fd, state->config.close_filter);
   const auto ordinal = selected ? state->closes.fetch_add(1U) + 1U : 0U;
   errno = saved_errno;
   const auto result = original(fd); // Inject only AFTER closing the real FD; never simulate a resource leak.
   if (result == 0 && selected && ordinal == state->config.close_eio) {
      state->errors.fetch_add(1U);
      errno = EIO;
      return -1;
   }
   return result;
}

#if defined(__APPLE__)
extern "C" int renameatx_np(int from_fd, const char* from, int to_fd, const char* to, unsigned int flags) {
   const auto saved_errno = errno;
   const auto original = actual_rename();
   if (!original) { errno = ENOSYS; return -1; }
   const auto state = current();
   const auto selected = state && from && to && std::string_view{from}.starts_with(".forge-swarm-") &&
       std::string_view{from}.ends_with(".part") && std::string_view{to} == "result";
   if (selected) {
      state->renames.fetch_add(1U);
      if (state->config.pause_publish_rename && !state->rename_paused.exchange(true) && !pause(state)) return -1;
      if (state->config.rename_exdev_once && !state->rename_failed.exchange(true)) {
         state->errors.fetch_add(1U); errno = EXDEV; return -1;
      }
   }
   errno = saved_errno;
   return original(from_fd, from, to_fd, to, flags);
}
#endif

namespace swarm_test::faults {

bool available() { return actual_sync() && actual_write() && actual_read() && actual_close(); }
bool rename_available() {
#if defined(__APPLE__)
   return actual_rename() != nullptr;
#else
   return false; // NOT_PROVEN: wrapping renameat2 cannot intercept production's raw Linux syscall.
#endif
}

scope::scope(plan value) : _state(std::make_shared<state>(value)) {
   if (!available()) throw std::runtime_error{"NOT_PROVEN: RTLD_NEXT POSIX symbols unavailable"};
   if ((value.rename_exdev_once || value.pause_publish_rename) && !rename_available()) {
      throw std::runtime_error{"NOT_PROVEN: production publication rename cannot be interposed on this platform"};
   }
   const auto lock = std::scoped_lock{active_mutex};
   if (active) throw std::logic_error{"overlapping syscall fault scopes"};
   active = _state;
   armed.store(true, std::memory_order_release);
}

scope::~scope() { disarm(); }

observation scope::snapshot() const {
   return {_state->syncs.load(), _state->writes.load(), _state->reads.load(),
           _state->errors.load(), _state->paused.load(), _state->timed_out.load(),
           _state->closes.load(), _state->renames.load()};
}

bool scope::wait_paused(std::uint64_t count) {
   auto lock = std::unique_lock{_state->mutex};
   const auto ready = _state->changed.wait_for(lock, std::chrono::seconds{5}, [&] {
      return _state->paused.load() >= count || _state->timed_out.load();
   });
   return ready && !_state->timed_out.load() && _state->paused.load() >= count;
}

void scope::release() noexcept {
   const auto lock = std::scoped_lock{_state->mutex};
   _state->released = true;
   _state->changed.notify_all();
}

void scope::disarm() noexcept {
   release();
   const auto lock = std::scoped_lock{active_mutex};
   if (active == _state) {
      armed.store(false, std::memory_order_release);
      active.reset();
   }
}

} // namespace swarm_test::faults
