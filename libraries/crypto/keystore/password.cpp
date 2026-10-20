module;

#include <forge/exceptions/macros.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "details/password_wait.hxx"

module forge.crypto.keystore.password;

import forge.crypto.core.secret_bytes;

namespace forge::crypto::keystore {
namespace {

[[noreturn]] void throw_password_error(std::string_view operation, int error = errno) {
   FORGE_THROW_EXCEPTION(
       exceptions::password_unavailable, "password input failed", forge::exceptions::ctx("operation", operation),
       forge::exceptions::ctx("system_error", std::error_code{error, std::generic_category()}.message()));
}

void write_all(int descriptor, std::string_view value) {
   auto offset = std::size_t{};
   while (offset < value.size()) {
      const auto count = ::write(descriptor, value.data() + offset, value.size() - offset);
      if (count < 0 && errno == EINTR) {
         continue;
      }
      if (count <= 0) {
         throw_password_error("write_prompt", count < 0 ? errno : EIO);
      }
      offset += static_cast<std::size_t>(count);
   }
}

core::secret_string read_line(int descriptor, std::size_t maximum, bool require_eof, password_wait& wait) {
   if (maximum == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "password byte limit must be greater than zero");
   }
   auto result = std::string{};
   try {
      result.reserve((std::min)(maximum, std::size_t{256}));
      auto ended_with_newline = false;
      while (true) {
         auto value = char{};
         wait.ready(descriptor);
         const auto count = ::read(descriptor, &value, 1U);
         wait.require_running();
         if (count < 0 && errno == EINTR) {
            continue;
         }
         if (count < 0) {
            throw_password_error("read");
         }
         if (count == 0) {
            break;
         }
         if (value == '\n') {
            ended_with_newline = true;
            break;
         }
         if (value == '\0') {
            FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "password input contains a NUL byte");
         }
         if (result.size() == maximum) {
            FORGE_THROW_EXCEPTION(exceptions::size_limit_exceeded, "password input exceeds its byte limit",
                                  forge::exceptions::ctx("maximum", maximum));
         }
         result.push_back(value);
      }
      if (ended_with_newline && !result.empty() && result.back() == '\r') {
         result.pop_back();
      }
      if (require_eof && ended_with_newline) {
         auto extra = char{};
         while (true) {
            wait.ready(descriptor);
            const auto count = ::read(descriptor, &extra, 1U);
            wait.require_running();
            if (count < 0 && errno == EINTR) {
               continue;
            }
            if (count < 0) {
               throw_password_error("read_trailing");
            }
            if (count != 0) {
               FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "password file must contain exactly one line");
            }
            break;
         }
      }
      if (result.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::password_unavailable, "password input is empty");
      }
      return core::secret_string{std::move(result)};
   } catch (...) {
      core::secure_erase(result);
      throw;
   }
}

class echo_guard
{
 public:
   explicit echo_guard(int descriptor)
     : descriptor_{descriptor}
   {
      if (::tcgetattr(descriptor_, &original_) != 0)
      {
         throw_password_error("get_terminal_attributes");
      }

      auto hidden = original_;
      hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
      if (::tcflush(descriptor_, TCIFLUSH) != 0)
      {
         throw_password_error("flush_terminal_input");
      }

      if (::tcsetattr(descriptor_, TCSANOW, &hidden) != 0)
      {
         throw_password_error("hide_terminal_echo");
      }

      active_ = true;
   }

   ~echo_guard()
   {
      if (active_)
      {
         // Discard pending input without waiting for a caller to consume terminal output.
         static_cast<void>(::tcflush(descriptor_, TCIFLUSH));
         static_cast<void>(::tcsetattr(descriptor_, TCSANOW, &original_));
      }
   }

   echo_guard(const echo_guard&) = delete;
   echo_guard& operator=(const echo_guard&) = delete;

 private:
   int descriptor_;
   termios original_{};
   bool active_ = false;
};

core::secret_string read_terminal(password_request request, password_wait& wait) {
   if (::isatty(STDIN_FILENO) == 0) {
      FORGE_THROW_EXCEPTION(exceptions::password_unavailable,
                            "password terminal input requires an interactive TTY; use stdin or a password file");
   }
   write_all(STDERR_FILENO, request.prompt);
   auto password = core::secret_string{};
   {
      auto guard = echo_guard{STDIN_FILENO};
      try {
         password = read_line(STDIN_FILENO, request.max_bytes, false, wait);
      } catch (...) {
         write_all(STDERR_FILENO, "\n");
         throw;
      }
   }
   write_all(STDERR_FILENO, "\n");
   return password;
}

core::secret_string read_file(password_request request, password_wait& wait) {
   if (request.file.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "password file path must not be empty");
   }
   auto flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#ifdef O_NOFOLLOW
   flags |= O_NOFOLLOW;
#endif
   const auto descriptor = ::open(request.file.c_str(), flags);
   if (descriptor < 0) {
      throw_password_error("open_file");
   }
   try {
      struct stat status{};
      if (::fstat(descriptor, &status) != 0) {
         throw_password_error("stat_file");
      }
      if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || (status.st_mode & 0077U) != 0U) {
         FORGE_THROW_EXCEPTION(exceptions::password_unavailable,
                               "password file must be a regular file owned by the current user with mode 0600");
      }
      auto value = read_line(descriptor, request.max_bytes, true, wait);
      static_cast<void>(::close(descriptor));
      return value;
   } catch (...) {
      static_cast<void>(::close(descriptor));
      throw;
   }
}

} // namespace

core::secret_string read_password(password_request request) {
   auto wait = password_wait{request.stop_token};
   switch (request.source) {
   case password_source::terminal:
      return read_terminal(std::move(request), wait);
   case password_source::standard_input:
      return read_line(STDIN_FILENO, request.max_bytes, false, wait);
   case password_source::file:
      return read_file(std::move(request), wait);
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "unknown password input source");
}

} // namespace forge::crypto::keystore
