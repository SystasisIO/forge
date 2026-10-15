#include "secrets_faults.hxx"
#include <forge/exceptions/macros.hpp>

#include <dlfcn.h>
#include <fcntl.h>
#include <openssl/hmac.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <string_view>

import forge.exceptions;

namespace {

using forge::tests::secrets::failure_point;

struct fault_state {
   std::string target;
   std::string canonical_target;
   std::string temporary_prefix;
   dev_t device{};
   ino_t inode{};
   failure_point point{};
   std::atomic<bool> triggered = false;
   std::atomic<bool> published = false;
   std::atomic<bool> timed_out = false;
   std::mutex mutex;
   std::condition_variable changed;
   bool released = false;
};

std::mutex active_mutex;
std::shared_ptr<fault_state> active;
std::atomic<bool> armed = false;

std::shared_ptr<fault_state> current_fault() {
   if (!armed.load()) {
      return {};
   }
   const auto lock = std::lock_guard{active_mutex};
   return active;
}

bool temporary_descriptor(int descriptor, const fault_state& state) {
   auto path = std::array<char, 4096>{};
#ifdef __APPLE__
   if (::fcntl(descriptor, F_GETPATH, path.data()) != 0) {
      return false;
   }
#else
   auto source = std::array<char, 64>{};
   const auto count = ::snprintf(source.data(), source.size(), "/proc/self/fd/%d", descriptor);
   if (count <= 0 || static_cast<std::size_t>(count) >= source.size()) {
      return false;
   }
   const auto length = ::readlink(source.data(), path.data(), path.size() - 1);
   if (length < 0) {
      return false;
   }
   path[static_cast<std::size_t>(length)] = '\0';
#endif
   return std::string_view{path.data()}.starts_with(state.temporary_prefix);
}

bool directory_descriptor(int descriptor, const fault_state& state) {
   struct stat status{};
   return ::fstat(descriptor, &status) == 0 && S_ISDIR(status.st_mode) && status.st_dev == state.device &&
          status.st_ino == state.inode;
}

bool trigger(const std::shared_ptr<fault_state>& state, failure_point point) {
   if (!state || state->point != point) {
      return false;
   }
   auto expected = false;
   return state->triggered.compare_exchange_strong(expected, true);
}

void pause(const std::shared_ptr<fault_state>& state) {
   auto lock = std::unique_lock{state->mutex};
   state->changed.notify_all();
   if (!state->changed.wait_for(lock, std::chrono::seconds{20}, [&] { return state->released; })) {
      state->timed_out.store(true);
   }
}

} // namespace

extern "C" ssize_t write(int descriptor, const void* bytes, std::size_t size) {
   static const auto original =
       reinterpret_cast<ssize_t (*)(int, const void*, std::size_t)>(::dlsym(RTLD_NEXT, "write"));
   const auto state = current_fault();
   if (state && temporary_descriptor(descriptor, *state) && trigger(state, failure_point::write)) {
      errno = EIO;
      return -1;
   }
   return original(descriptor, bytes, size);
}

extern "C" int fsync(int descriptor) {
   static const auto original = reinterpret_cast<int (*)(int)>(::dlsym(RTLD_NEXT, "fsync"));
   const auto state = current_fault();
   if (state && ((temporary_descriptor(descriptor, *state) && trigger(state, failure_point::file_sync)) ||
                 (state->published.load() && directory_descriptor(descriptor, *state) &&
                  trigger(state, failure_point::directory_sync)))) {
      errno = EIO;
      return -1;
   }
   return original(descriptor);
}

extern "C" int close(int descriptor) {
   static const auto original = reinterpret_cast<int (*)(int)>(::dlsym(RTLD_NEXT, "close"));
   const auto state = current_fault();
   const auto inject = state && temporary_descriptor(descriptor, *state) && trigger(state, failure_point::file_close);
   const auto result = original(descriptor);
   if (inject) {
      errno = EIO;
      return -1;
   }
   return result;
}

extern "C" int rename(const char* source, const char* destination) {
   static const auto original = reinterpret_cast<int (*)(const char*, const char*)>(::dlsym(RTLD_NEXT, "rename"));
   const auto state = current_fault();
   const auto selected = state && (state->target == destination || state->canonical_target == destination);
   if (selected && trigger(state, failure_point::publish)) {
      errno = EIO;
      return -1;
   }
   const auto result = original(source, destination);
   if (selected && result == 0) {
      state->published.store(true);
      if (trigger(state, failure_point::pause_after_publish)) {
         pause(state);
      }
   }
   return result;
}

extern "C" unsigned char* HMAC(const EVP_MD* digest, const void* key, int key_size, const unsigned char* input,
                               std::size_t input_size, unsigned char* output, unsigned int* output_size) {
   static const auto original =
       reinterpret_cast<unsigned char* (*)(const EVP_MD*, const void*, int, const unsigned char*, std::size_t,
                                           unsigned char*, unsigned int*)>(::dlsym(RTLD_NEXT, "HMAC"));
   const auto state = current_fault();
   // A dedicated public fixture salt distinguishes HKDF extract from receipt
   // HMACs and OpenSSL's internal crypto. Secret material is never inspected.
   if (state && state->point == failure_point::pause_hkdf && key_size == 32 &&
       std::all_of(static_cast<const unsigned char*>(key), static_cast<const unsigned char*>(key) + key_size,
                   [](auto value) { return value == 0x77; }) &&
       trigger(state, failure_point::pause_hkdf)) {
      pause(state);
   }
   return original(digest, key, key_size, input, input_size, output, output_size);
}

namespace forge::tests::secrets {

struct syscall_fault::state : fault_state {};

syscall_fault::syscall_fault(const std::filesystem::path& target, failure_point point)
    : _state{std::make_shared<state>()} {
   struct stat directory{};
   if (::stat(target.parent_path().c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode)) {
      FORGE_THROW_EXCEPTION(forge::exceptions::context_error, "Fault test target directory is unavailable");
   }
   _state->target = target.string();
   _state->canonical_target = (std::filesystem::canonical(target.parent_path()) / target.filename()).string();
   _state->temporary_prefix = _state->canonical_target + ".tmp-";
   _state->device = directory.st_dev;
   _state->inode = directory.st_ino;
   _state->point = point;
   const auto lock = std::lock_guard{active_mutex};
   if (active) {
      FORGE_THROW_EXCEPTION(forge::exceptions::context_error, "Fault tests cannot overlap");
   }
   active = _state;
   armed.store(true);
}

syscall_fault::~syscall_fault() {
   release();
   const auto lock = std::lock_guard{active_mutex};
   if (active == _state) {
      armed.store(false);
      active.reset();
   }
}

bool syscall_fault::fired() const {
   return _state->triggered.load();
}

bool syscall_fault::wait_until_published(std::chrono::milliseconds timeout) {
   auto lock = std::unique_lock{_state->mutex};
   return _state->changed.wait_for(lock, timeout, [&] { return _state->triggered.load() && _state->published.load(); });
}

bool syscall_fault::wait_until_paused(std::chrono::milliseconds timeout) {
   auto lock = std::unique_lock{_state->mutex};
   return _state->changed.wait_for(lock, timeout, [&] { return _state->triggered.load(); });
}

bool syscall_fault::pause_timed_out() const {
   return _state->timed_out.load();
}

void syscall_fault::release() {
   auto lock = std::lock_guard{_state->mutex};
   _state->released = true;
   _state->changed.notify_all();
}

} // namespace forge::tests::secrets
