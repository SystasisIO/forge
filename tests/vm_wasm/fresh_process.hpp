#pragma once

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <spawn.h>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

extern char** environ;

namespace fresh_process {

inline constexpr const char* mode_variable = "FORGE_VM_WASM_INTERPRET_FRESH_CHILD";

inline const char* mode() {
   return std::getenv(mode_variable);
}

inline void disable_core_dumps() {
   const auto limit = rlimit{0, 0};
   if (::setrlimit(RLIMIT_CORE, &limit) != 0)
      std::_Exit(92);
}

struct result {
   int status = 0;
   int error = 0;
   bool timed_out = false;
};

// Test-only fresh exec, not a product process framework. Preconditions: no
// competing reaper/SIGCHLD auto-reap, finite EINTR, and normal OS wait/kill contracts.
// Confirmed owned children are bounded/reaped; unconfirmed ownership returns an
// error without signaling. No parent mask/disposition/limit/Mach route is changed.
inline result run(std::string test, std::string child_mode, std::chrono::seconds timeout = std::chrono::seconds{4}) {
   if (timeout <= std::chrono::seconds::zero() || timeout > std::chrono::seconds{10})
      return {0, EINVAL, false};
   std::array<char, 4096> executable{};
#if defined(__APPLE__)
   auto size = static_cast<std::uint32_t>(executable.size());
   if (_NSGetExecutablePath(executable.data(), &size) != 0)
      return {0, ENAMETOOLONG, false};
#else
   const auto size = ::readlink("/proc/self/exe", executable.data(), executable.size() - 1);
   if (size < 0)
      return {0, errno, false};
   if (static_cast<std::size_t>(size) == executable.size() - 1)
      return {0, ENAMETOOLONG, false};
   executable[static_cast<std::size_t>(size)] = 0;
#endif
   auto filter = std::string{"--run_test="} + test;
   std::array<char*, 4> arguments{executable.data(), filter.data(), const_cast<char*>("--catch_system_errors=no"),
                                  nullptr};
   auto assignment = std::string{mode_variable} + "=" + child_mode;
   std::vector<char*> environment;
   const auto prefix = std::string{mode_variable} + "=";
   for (auto** item = environ; *item; ++item) {
      if (std::string_view{*item}.starts_with(prefix))
         continue;
      environment.push_back(*item);
   }
   environment.push_back(assignment.data());
   environment.push_back(nullptr);
   pid_t child = -1;
   const auto spawned =
       ::posix_spawn(&child, executable.data(), nullptr, nullptr, arguments.data(), environment.data());
   if (spawned != 0)
      return {0, spawned, false};
   auto value = result{};
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (true) {
      const auto waited = ::waitpid(child, &value.status, WNOHANG);
      if (waited == child)
         return value;
      if (waited < 0 && errno != EINTR) {
         value.error = errno;
         // An external reaper/disposition may already have consumed this child.
         // Ownership is no longer confirmed; never signal a possibly reused PID.
         return value;
      }
      if (waited < 0) {
         continue; // Retry EINTR; timeout signaling requires a confirmed waitpid()==0 below.
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         value.timed_out = true;
         break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   if (::kill(child, SIGKILL) != 0 && errno != ESRCH)
      value.error = errno;
   while (::waitpid(child, &value.status, 0) < 0) {
      if (errno == EINTR)
         continue;
      if (value.error == 0)
         value.error = errno;
      break;
   }
   return value;
}

} // namespace fresh_process
