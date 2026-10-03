module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

module forge.otlp.crash;

import forge.exceptions;
import forge.log.record;

namespace forge::otlp {
namespace {

constexpr auto record_magic = std::uint64_t{0x46434c4f544c5043ULL}; // "FORGEOTLPC"
constexpr auto record_version = std::uint16_t{1};
constexpr auto max_exception_category = std::size_t{64};
constexpr auto max_stack_addresses = std::size_t{32};
constexpr auto spool_mutation_lock_name = std::string_view{".crash-resend.lock"};

enum class record_kind : std::uint32_t {
   signal = 1,
   terminate = 2,
};

struct disk_record {
   std::uint64_t magic = record_magic;
   std::uint16_t version = record_version;
   std::uint16_t header_size = 0;
   std::uint32_t record_size = 0;
   std::uint32_t checksum = 0;
   std::uint32_t kind = 0;
   std::uint32_t pid = 0;
   std::uint64_t sequence = 0;
   std::uint64_t unix_nanos = 0;
   std::int32_t signal_number = 0;
   std::uint64_t fault_address = 0;
   std::int32_t exception_code = 0;
   std::uint32_t stack_count = 0;
   std::array<char, max_exception_category> exception_category{};
   std::array<std::uint64_t, max_stack_addresses> stack_addresses{};
};

static_assert(sizeof(disk_record) <= 4096);

std::uint32_t checksum_record(const disk_record& record) noexcept {
   const auto* bytes = reinterpret_cast<const unsigned char*>(&record);
   auto hash = std::uint32_t{2166136261u};
   for (auto index = std::size_t{0}; index < sizeof(disk_record); ++index) {
      auto value = bytes[index];
      if (index >= offsetof(disk_record, checksum) &&
          index < offsetof(disk_record, checksum) + sizeof(disk_record::checksum)) {
         value = 0;
      }
      hash ^= value;
      hash *= 16777619u;
   }
   return hash;
}

void copy_fixed(std::array<char, max_exception_category>& out, std::string_view value) noexcept {
   const auto count = std::min(out.size() - 1, value.size());
   for (auto index = std::size_t{0}; index < count; ++index) {
      out[index] = value[index];
   }
   out[count] = '\0';
}

std::string fixed_string(const std::array<char, max_exception_category>& value) {
   auto size = std::size_t{0};
   while (size < value.size() && value[size] != '\0') {
      ++size;
   }
   return std::string{value.data(), size};
}

std::uint64_t now_nanos() {
   const auto now = std::chrono::system_clock::now().time_since_epoch();
   const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now);
   if (nanos.count() <= 0) {
      return 0;
   }
   return static_cast<std::uint64_t>(nanos.count());
}

void write_all_noexcept(int fd, const disk_record& record) noexcept {
#if defined(__unix__) || defined(__APPLE__)
   const auto* data = reinterpret_cast<const char*>(&record);
   auto remaining = sizeof(disk_record);
   while (remaining > 0) {
      const auto written = ::write(fd, data, remaining);
      if (written <= 0) {
         return;
      }
      data += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
   }
#else
   static_cast<void>(fd);
   static_cast<void>(record);
#endif
}

} // namespace

struct crash_state;

#if defined(__unix__) || defined(__APPLE__)
struct spool_identity {
   pid_t pid = 0;
   dev_t device = 0;
   ino_t inode = 0;
   std::string name;
};

struct spool_snapshot {
   dev_t device = 0;
   ino_t inode = 0;
   std::uint64_t size = 0;
};
#endif

std::atomic<crash_state*> active_capture{nullptr};
std::mutex capture_lifecycle_mutex;

struct crash_state {
#if defined(__unix__) || defined(__APPLE__)
   int fd = -1;
   pid_t pid = 0;
   spool_identity identity;
   volatile std::sig_atomic_t records_written = 0;
   std::sig_atomic_t max_records = 0;
   std::vector<int> installed_signals;
   std::vector<struct sigaction> previous_actions;
#endif
   std::terminate_handler previous_terminate = nullptr;
   bool terminate_installed = false;
   bool restored = false;
   bool chain_after_capture = true;

   ~crash_state() {
      restore();
   }

   void restore() noexcept {
      if (restored) {
         return;
      }
      restored = true;

      {
         const auto lock = std::scoped_lock{capture_lifecycle_mutex};
         auto* expected = this;
         active_capture.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
      }

      if (terminate_installed) {
         std::set_terminate(previous_terminate);
      }

#if defined(__unix__) || defined(__APPLE__)
      for (auto index = std::size_t{0}; index < installed_signals.size(); ++index) {
         ::sigaction(installed_signals[index], &previous_actions[index], nullptr);
      }
      if (fd >= 0) {
         ::close(fd);
         fd = -1;
      }
#endif
   }
};

namespace {

void validate_options(const crash_spool_options& options, bool installing) {
   if (options.directory.empty() || options.directory == options.directory.root_path()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash spool directory is not safe");
   }
   if (options.max_record_bytes < sizeof(disk_record) || options.max_records_per_process == 0 ||
       options.max_records_per_resend == 0 || options.max_file_bytes < sizeof(disk_record)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash spool limits must be positive");
   }
   if (options.max_file_bytes < options.max_record_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash file limit must cover one record");
   }
   if (options.max_records_per_process > static_cast<std::size_t>((std::numeric_limits<std::sig_atomic_t>::max)())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash process record limit is too large");
   }
   if (installing && !options.capture_signals && !options.capture_terminate) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash capture has no enabled capture source");
   }
   if (options.capture_signals) {
      if (options.signals.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash signal set must not be empty");
      }
      for (const auto signal_number : options.signals) {
         if (signal_number <= 0
#if defined(NSIG)
             || signal_number >= NSIG
#endif
         ) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "OTLP crash signal is not supported",
                                  forge::exceptions::ctx("signal", signal_number));
         }
      }
   }
}

void fill_common(disk_record& record, crash_state& state, record_kind kind) noexcept {
#if defined(__unix__) || defined(__APPLE__)
   record.header_size = sizeof(disk_record);
   record.record_size = sizeof(disk_record);
   record.kind = static_cast<std::uint32_t>(kind);
   record.pid = static_cast<std::uint32_t>(state.pid);
   record.sequence = static_cast<std::uint64_t>(state.records_written + 1);
#else
   static_cast<void>(record);
   static_cast<void>(state);
   static_cast<void>(kind);
#endif
}

void write_record(crash_state& state, disk_record record) noexcept {
#if defined(__unix__) || defined(__APPLE__)
   if (state.fd < 0 || state.records_written >= state.max_records) {
      return;
   }
   state.records_written = state.records_written + 1;
   record.sequence = static_cast<std::uint64_t>(state.records_written);
   record.checksum = checksum_record(record);
   write_all_noexcept(state.fd, record);
#else
   static_cast<void>(state);
   static_cast<void>(record);
#endif
}

void capture_current_exception(disk_record& record) noexcept {
   try {
      const auto current = std::current_exception();
      if (!current) {
         return;
      }
      std::rethrow_exception(current);
   } catch (const forge::exceptions::base& error) {
      record.exception_code = error.code().value();
      copy_fixed(record.exception_category, error.code().category().name());
   } catch (const std::exception&) {
      copy_fixed(record.exception_category, "std.exception");
   } catch (...) {
      copy_fixed(record.exception_category, "unknown");
   }
}

void capture_stack_addresses(disk_record& record) noexcept {
   try {
      const auto stacktrace = forge::capture_stacktrace(2, max_stack_addresses);
      const auto count = std::min(stacktrace.frames.size(), record.stack_addresses.size());
      record.stack_count = static_cast<std::uint32_t>(count);
      for (auto index = std::size_t{0}; index < count; ++index) {
         record.stack_addresses[index] = static_cast<std::uint64_t>(stacktrace.frames[index].address);
      }
   } catch (...) {
      record.stack_count = 0;
   }
}

[[noreturn]] void forward_signal_or_exit(int signal_number, bool chain_after_capture) noexcept {
#if defined(__unix__) || defined(__APPLE__)
   if (chain_after_capture) {
      ::signal(signal_number, SIG_DFL);
      ::kill(::getpid(), signal_number);
   }
#endif
   std::_Exit(chain_after_capture ? 128 + signal_number : 0);
}

void signal_handler(int signal_number, siginfo_t* info, void*) noexcept {
   auto* state = active_capture.load(std::memory_order_acquire);
   auto chain_after_capture = true;
   if (state != nullptr) {
      chain_after_capture = state->chain_after_capture;
      auto record = disk_record{};
      fill_common(record, *state, record_kind::signal);
      record.signal_number = signal_number;
      if (info != nullptr) {
         record.fault_address = reinterpret_cast<std::uintptr_t>(info->si_addr);
      }
      write_record(*state, record);
   }
   forward_signal_or_exit(signal_number, chain_after_capture);
}

[[noreturn]] void terminate_handler() noexcept {
   auto* state = active_capture.load(std::memory_order_acquire);
   auto previous = std::terminate_handler{};
   auto chain_after_capture = true;
   if (state != nullptr) {
      chain_after_capture = state->chain_after_capture;
      auto record = disk_record{};
      fill_common(record, *state, record_kind::terminate);
      record.unix_nanos = now_nanos();
      capture_current_exception(record);
      capture_stack_addresses(record);
      write_record(*state, record);
      previous = state->previous_terminate;
      active_capture.store(nullptr, std::memory_order_release);
   }

   if (chain_after_capture && previous != nullptr && previous != terminate_handler) {
      previous();
   }
   if (chain_after_capture) {
      std::abort();
   }
   std::_Exit(0);
}

void ensure_crash_directory(const std::filesystem::path& directory) {
   auto error = std::error_code{};
   std::filesystem::create_directories(directory, error);
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "failed to create OTLP crash spool directory",
                            forge::exceptions::ctx("path", directory.string()),
                            forge::exceptions::ctx("reason", error.message()));
   }
}

std::filesystem::path spool_path_for(const std::filesystem::path& directory, std::uint64_t pid) {
   return directory / ("crash-" + std::to_string(pid) + ".spool");
}

bool valid_record(const disk_record& record) {
   return record.magic == record_magic && record.version == record_version &&
          record.header_size == sizeof(disk_record) && record.record_size == sizeof(disk_record) &&
          record.checksum == checksum_record(record);
}

#if defined(__unix__) || defined(__APPLE__)
struct fd_guard {
   int fd = -1;

   ~fd_guard() {
      if (fd >= 0) {
         ::close(fd);
      }
   }

   fd_guard() = default;
   explicit fd_guard(int value) : fd(value) {}

   fd_guard(const fd_guard&) = delete;
   fd_guard& operator=(const fd_guard&) = delete;

   fd_guard(fd_guard&& other) noexcept : fd(std::exchange(other.fd, -1)) {}
   fd_guard& operator=(fd_guard&& other) noexcept {
      if (this != &other) {
         if (fd >= 0) {
            ::close(fd);
         }
         fd = std::exchange(other.fd, -1);
      }
      return *this;
   }

   [[nodiscard]] int get() const noexcept {
      return fd;
   }

   [[nodiscard]] int release() noexcept {
      return std::exchange(fd, -1);
   }
};

void throw_errno_spool_error(std::string_view message, const std::filesystem::path& path) {
   FORGE_THROW_EXCEPTION(exceptions::spool_error, std::string{message}, forge::exceptions::ctx("path", path.string()),
                         forge::exceptions::ctx("errno", errno));
}

void ensure_owner_private_mode(const struct stat& value, const std::filesystem::path& path, bool directory) {
   const auto mode = value.st_mode;
   if (directory) {
      if (!S_ISDIR(mode)) {
         FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool path is not a directory",
                               forge::exceptions::ctx("path", path.string()));
      }
   } else if (!S_ISREG(mode)) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool path is not a regular file",
                            forge::exceptions::ctx("path", path.string()));
   }
   if (value.st_uid != ::geteuid()) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool path is not owned by current user",
                            forge::exceptions::ctx("path", path.string()));
   }
   if ((mode & (S_IWGRP | S_IWOTH)) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool path is group/world writable",
                            forge::exceptions::ctx("path", path.string()));
   }
}

fd_guard open_spool_directory(const std::filesystem::path& directory) {
   auto flags = O_RDONLY;
#if defined(O_CLOEXEC)
   flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
   flags |= O_NOFOLLOW;
#endif
#if defined(O_DIRECTORY)
   flags |= O_DIRECTORY;
#endif
   auto fd = fd_guard{::open(directory.c_str(), flags)};
   if (fd.get() < 0) {
      throw_errno_spool_error("failed to open OTLP crash spool directory", directory);
   }
   struct stat stat_value{};
   if (::fstat(fd.get(), &stat_value) != 0) {
      throw_errno_spool_error("failed to stat OTLP crash spool directory", directory);
   }
   ensure_owner_private_mode(stat_value, directory, true);
   return fd;
}

class spool_mutation_lock {
 public:
   spool_mutation_lock(int directory_fd, const std::filesystem::path& directory)
       : path_{directory / std::string{spool_mutation_lock_name}} {
      const auto name = std::string{spool_mutation_lock_name};
      auto flags = O_RDWR;
#if defined(O_CLOEXEC)
      flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
      flags |= O_NOFOLLOW;
#endif
      fd_ = fd_guard{::openat(directory_fd, name.c_str(), flags)};
      if (fd_.get() < 0) {
         if (errno != ENOENT) {
            throw_errno_spool_error("failed to open OTLP crash spool mutation lock", path_);
         }
         auto create_flags = O_CREAT | O_EXCL | O_RDWR;
#if defined(O_CLOEXEC)
         create_flags |= O_CLOEXEC;
#endif
         fd_ = fd_guard{::openat(directory_fd, name.c_str(), create_flags, S_IRUSR | S_IWUSR)};
         if (fd_.get() < 0) {
            if (errno != EEXIST) {
               throw_errno_spool_error("failed to create OTLP crash spool mutation lock", path_);
            }
            fd_ = fd_guard{::openat(directory_fd, name.c_str(), flags)};
            if (fd_.get() < 0) {
               throw_errno_spool_error("failed to open raced OTLP crash spool mutation lock", path_);
            }
         }
      }

      struct stat stat_value{};
      if (::fstat(fd_.get(), &stat_value) != 0) {
         throw_errno_spool_error("failed to stat OTLP crash spool mutation lock", path_);
      }
      ensure_owner_private_mode(stat_value, path_, false);

      struct flock lock{};
      lock.l_type = F_WRLCK;
      lock.l_whence = SEEK_SET;
      while (::fcntl(fd_.get(), F_SETLKW, &lock) != 0) {
         if (errno == EINTR) {
            continue;
         }
         throw_errno_spool_error("failed to lock OTLP crash spool mutation", path_);
      }
      locked_ = true;
   }

   ~spool_mutation_lock() {
      if (!locked_) {
         return;
      }
      struct flock lock{};
      lock.l_type = F_UNLCK;
      lock.l_whence = SEEK_SET;
      (void)::fcntl(fd_.get(), F_SETLK, &lock);
   }

   spool_mutation_lock(const spool_mutation_lock&) = delete;
   spool_mutation_lock& operator=(const spool_mutation_lock&) = delete;

 private:
   fd_guard fd_;
   std::filesystem::path path_;
   bool locked_ = false;
};

struct open_spool_result {
   int fd = -1;
   std::size_t existing_records = 0;
   std::size_t max_records = 0;
   spool_identity identity;
};

[[nodiscard]] std::size_t max_file_records(const crash_spool_options& options) noexcept {
   return options.max_file_bytes / sizeof(disk_record);
}

[[nodiscard]] std::sig_atomic_t checked_signal_count(std::size_t value, const std::filesystem::path& path) {
   if (value > static_cast<std::size_t>((std::numeric_limits<std::sig_atomic_t>::max)())) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool record count is too large",
                            forge::exceptions::ctx("path", path.string()));
   }
   return static_cast<std::sig_atomic_t>(value);
}

[[nodiscard]] std::size_t existing_record_count(const struct stat& stat_value, const std::filesystem::path& path,
                                                const crash_spool_options& options) {
   if (stat_value.st_size < 0 || static_cast<std::uint64_t>(stat_value.st_size) > options.max_file_bytes ||
       static_cast<std::uint64_t>(stat_value.st_size) % sizeof(disk_record) != 0) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "existing OTLP crash spool is malformed",
                            forge::exceptions::ctx("path", path.string()));
   }
   return static_cast<std::size_t>(static_cast<std::uint64_t>(stat_value.st_size) / sizeof(disk_record));
}

void read_record_or_throw(int fd, disk_record& record, const std::filesystem::path& path) {
   auto* cursor = reinterpret_cast<char*>(&record);
   auto remaining = sizeof(record);
   while (remaining > 0) {
      const auto count = ::read(fd, cursor, remaining);
      if (count == 0) {
         FORGE_THROW_EXCEPTION(exceptions::spool_error, "existing OTLP crash spool is truncated",
                               forge::exceptions::ctx("path", path.string()));
      }
      if (count < 0) {
         if (errno == EINTR) {
            continue;
         }
         throw_errno_spool_error("failed to read existing OTLP crash spool", path);
      }
      cursor += static_cast<std::size_t>(count);
      remaining -= static_cast<std::size_t>(count);
   }
}

void validate_existing_records(int fd, std::size_t count, const std::filesystem::path& path) {
   if (::lseek(fd, 0, SEEK_SET) < 0) {
      throw_errno_spool_error("failed to seek existing OTLP crash spool", path);
   }
   for (auto index = std::size_t{}; index < count; ++index) {
      auto record = disk_record{};
      read_record_or_throw(fd, record, path);
      if (!valid_record(record)) {
         FORGE_THROW_EXCEPTION(exceptions::spool_error, "existing OTLP crash spool contains invalid record",
                               forge::exceptions::ctx("path", path.string()));
      }
   }
   if (::lseek(fd, 0, SEEK_END) < 0) {
      throw_errno_spool_error("failed to seek existing OTLP crash spool end", path);
   }
}

[[nodiscard]] open_spool_result finalize_open_spool_file(fd_guard fd, const std::filesystem::path& path,
                                                         const crash_spool_options& options) {
   struct stat stat_value{};
   if (::fstat(fd.get(), &stat_value) != 0) {
      throw_errno_spool_error("failed to stat OTLP crash spool", path);
   }
   ensure_owner_private_mode(stat_value, path, false);
   const auto existing = existing_record_count(stat_value, path, options);
   validate_existing_records(fd.get(), existing, path);
   const auto capacity = max_file_records(options);
   if (existing >= capacity) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool is full",
                            forge::exceptions::ctx("path", path.string()));
   }
   const auto writable = std::min(options.max_records_per_process, capacity - existing);
   if (writable == 0) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "OTLP crash spool cannot accept records",
                            forge::exceptions::ctx("path", path.string()));
   }
   (void)checked_signal_count(existing, path);
   (void)checked_signal_count(existing + writable, path);
   return open_spool_result{
       .fd = fd.release(),
       .existing_records = existing,
       .max_records = existing + writable,
       .identity =
           spool_identity{
               .pid = ::getpid(),
               .device = stat_value.st_dev,
               .inode = stat_value.st_ino,
               .name = path.filename().string(),
           },
   };
}

open_spool_result open_spool_file(const std::filesystem::path& path, const crash_spool_options& options) {
   const auto directory_fd = open_spool_directory(path.parent_path());
   const auto mutation_lock = spool_mutation_lock{directory_fd.get(), path.parent_path()};
   auto flags = O_CREAT | O_EXCL | O_WRONLY;
#if defined(O_CLOEXEC)
   flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
   flags |= O_NOFOLLOW;
#endif
   const auto filename = path.filename().string();
   auto fd = fd_guard{::openat(directory_fd.get(), filename.c_str(), flags, S_IRUSR | S_IWUSR)};
   if (fd.get() < 0) {
      if (errno != EEXIST) {
         throw_errno_spool_error("failed to create OTLP crash spool", path);
      }
      auto existing_flags = O_RDWR | O_APPEND;
#if defined(O_CLOEXEC)
      existing_flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
      existing_flags |= O_NOFOLLOW;
#endif
      fd = fd_guard{::openat(directory_fd.get(), filename.c_str(), existing_flags)};
      if (fd.get() < 0) {
         throw_errno_spool_error("failed to open existing OTLP crash spool", path);
      }
   }
   return finalize_open_spool_file(std::move(fd), path, options);
}
#endif

struct read_file_result {
   std::vector<disk_record> records;
   std::uint64_t total_records = 0;
   bool malformed = false;
#if defined(__unix__) || defined(__APPLE__)
   std::optional<spool_snapshot> snapshot;
#endif
};

#if defined(__unix__) || defined(__APPLE__)
struct dir_guard {
   DIR* value = nullptr;

   ~dir_guard() {
      if (value != nullptr) {
         ::closedir(value);
      }
   }

   dir_guard() = default;
   explicit dir_guard(DIR* directory) : value(directory) {}

   dir_guard(const dir_guard&) = delete;
   dir_guard& operator=(const dir_guard&) = delete;
};

struct spool_entry {
   std::string name;
   std::filesystem::path path;
};

[[nodiscard]] bool is_spool_name(std::string_view value) {
   return value.starts_with("crash-") && value.ends_with(".spool");
}

[[nodiscard]] std::optional<pid_t> pid_from_spool_name(std::string_view value) {
   if (!is_spool_name(value)) {
      return std::nullopt;
   }
   constexpr auto prefix_size = std::string_view{"crash-"}.size();
   constexpr auto suffix_size = std::string_view{".spool"}.size();
   const auto digits = value.substr(prefix_size, value.size() - prefix_size - suffix_size);
   if (digits.empty()) {
      return std::nullopt;
   }

   auto parsed = std::uint64_t{0};
   for (const auto digit : digits) {
      if (digit < '0' || digit > '9') {
         return std::nullopt;
      }
      const auto value_digit = static_cast<std::uint64_t>(digit - '0');
      if (parsed > (std::numeric_limits<std::uint64_t>::max() - value_digit) / 10) {
         return std::nullopt;
      }
      parsed = parsed * 10 + value_digit;
      if (parsed > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
         return std::nullopt;
      }
   }
   if (parsed == 0) {
      return std::nullopt;
   }
   return static_cast<pid_t>(parsed);
}

[[nodiscard]] bool process_may_be_alive(pid_t pid) noexcept {
   if (pid <= 0) {
      return false;
   }
   errno = 0;
   if (::kill(pid, 0) == 0) {
      return true;
   }
   return errno == EPERM;
}

[[nodiscard]] bool spool_may_belong_to_live_process(const spool_entry& entry) noexcept {
   const auto pid = pid_from_spool_name(entry.name);
   if (!pid.has_value() || *pid == ::getpid()) {
      return false;
   }
   return process_may_be_alive(*pid);
}

std::vector<spool_entry> list_spool_files(const std::filesystem::path& directory, int directory_fd) {
   auto duplicate = fd_guard{::dup(directory_fd)};
   if (duplicate.get() < 0) {
      throw_errno_spool_error("failed to duplicate OTLP crash spool directory fd", directory);
   }

   auto handle = dir_guard{::fdopendir(duplicate.get())};
   if (handle.value == nullptr) {
      throw_errno_spool_error("failed to enumerate OTLP crash spool directory", directory);
   }
   (void)duplicate.release();

   auto files = std::vector<spool_entry>{};
   while (auto* entry = ::readdir(handle.value)) {
      const auto name = std::string{entry->d_name};
      if (!is_spool_name(name)) {
         continue;
      }
      files.push_back(spool_entry{.name = name, .path = directory / name});
   }
   std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) { return left.name < right.name; });
   return files;
}

[[nodiscard]] bool safe_spool_file(const struct stat& value) noexcept {
   return S_ISREG(value.st_mode) && value.st_uid == ::geteuid() && (value.st_mode & (S_IWGRP | S_IWOTH)) == 0;
}

struct open_existing_result {
   fd_guard fd;
   bool unsafe = false;
   bool missing = false;
   struct stat stat_value{};
};

open_existing_result open_existing_spool_file(int directory_fd, const spool_entry& entry) {
   auto flags = O_RDONLY;
#if defined(O_CLOEXEC)
   flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
   flags |= O_NOFOLLOW;
#endif
   auto fd = fd_guard{::openat(directory_fd, entry.name.c_str(), flags)};
   if (fd.get() < 0) {
      if (errno == ENOENT) {
         return open_existing_result{.missing = true};
      }
#if defined(ELOOP)
      if (errno == ELOOP) {
         return open_existing_result{.unsafe = true};
      }
#endif
      throw_errno_spool_error("failed to open OTLP crash spool", entry.path);
   }

   struct stat stat_value{};
   if (::fstat(fd.get(), &stat_value) != 0) {
      throw_errno_spool_error("failed to stat OTLP crash spool", entry.path);
   }
   if (!safe_spool_file(stat_value)) {
      return open_existing_result{.unsafe = true};
   }
   return open_existing_result{.fd = std::move(fd), .stat_value = stat_value};
}

[[nodiscard]] bool active_spool_matches_locked(int directory_fd, const spool_entry& entry) {
   auto* state = active_capture.load(std::memory_order_acquire);
   if (state == nullptr) {
      return false;
   }
   if (entry.name != state->identity.name || state->identity.pid != ::getpid()) {
      return false;
   }

   auto opened = open_existing_spool_file(directory_fd, entry);
   if (opened.missing || opened.unsafe) {
      return false;
   }
   return opened.stat_value.st_dev == state->identity.device && opened.stat_value.st_ino == state->identity.inode;
}

[[nodiscard]] bool spool_snapshot_matches_locked(int directory_fd, const spool_entry& entry,
                                                 const spool_snapshot& snapshot) {
   auto opened = open_existing_spool_file(directory_fd, entry);
   if (opened.missing || opened.unsafe || opened.stat_value.st_size < 0) {
      return false;
   }
   return opened.stat_value.st_dev == snapshot.device && opened.stat_value.st_ino == snapshot.inode &&
          static_cast<std::uint64_t>(opened.stat_value.st_size) == snapshot.size;
}

[[nodiscard]] bool is_active_current_process_spool(int directory_fd, const spool_entry& entry) {
   const auto lock = std::scoped_lock{capture_lifecycle_mutex};
   return active_spool_matches_locked(directory_fd, entry);
}

template <typename Mutation>
[[nodiscard]] bool mutate_if_malformed_spool_still_quarantinable(int directory_fd, const spool_entry& entry,
                                                                 const std::optional<spool_snapshot>& snapshot,
                                                                 Mutation&& mutation) {
   const auto mutation_lock = spool_mutation_lock{directory_fd, entry.path.parent_path()};
   const auto lock = std::scoped_lock{capture_lifecycle_mutex};
   if (active_spool_matches_locked(directory_fd, entry) || spool_may_belong_to_live_process(entry)) {
      return false;
   }
   if (snapshot.has_value() && !spool_snapshot_matches_locked(directory_fd, entry, *snapshot)) {
      return false;
   }
   std::forward<Mutation>(mutation)();
   return true;
}

template <typename Mutation>
[[nodiscard]] bool mutate_if_exported_spool_still_removable(int directory_fd, const spool_entry& entry,
                                                            const spool_snapshot& snapshot, Mutation&& mutation) {
   const auto mutation_lock = spool_mutation_lock{directory_fd, entry.path.parent_path()};
   const auto lock = std::scoped_lock{capture_lifecycle_mutex};
   if (active_spool_matches_locked(directory_fd, entry) || spool_may_belong_to_live_process(entry)) {
      return false;
   }
   if (!spool_snapshot_matches_locked(directory_fd, entry, snapshot)) {
      return false;
   }
   std::forward<Mutation>(mutation)();
   return true;
}

[[nodiscard]] bool read_exact_or_malformed(int fd, void* out, std::size_t size, const std::filesystem::path& path) {
   auto* cursor = static_cast<char*>(out);
   auto remaining = size;
   while (remaining > 0) {
      const auto read = ::read(fd, cursor, remaining);
      if (read == 0) {
         return false;
      }
      if (read < 0) {
         throw_errno_spool_error("failed to read OTLP crash spool", path);
      }
      cursor += static_cast<std::size_t>(read);
      remaining -= static_cast<std::size_t>(read);
   }
   return true;
}

void write_all(int fd, const char* data, std::size_t size, const std::filesystem::path& path) {
   auto remaining = size;
   while (remaining > 0) {
      const auto written = ::write(fd, data, remaining);
      if (written <= 0) {
         throw_errno_spool_error("failed to write OTLP crash spool", path);
      }
      data += static_cast<std::size_t>(written);
      remaining -= static_cast<std::size_t>(written);
   }
}

read_file_result read_records(int directory_fd, const spool_entry& entry, std::size_t limit,
                              std::size_t max_file_bytes) {
   auto result = read_file_result{};
   auto opened = open_existing_spool_file(directory_fd, entry);
   if (opened.missing) {
      return result;
   }
   if (opened.unsafe || opened.stat_value.st_size < 0) {
      result.malformed = true;
      return result;
   }

   const auto file_size = static_cast<std::uint64_t>(opened.stat_value.st_size);
   result.snapshot = spool_snapshot{
       .device = opened.stat_value.st_dev,
       .inode = opened.stat_value.st_ino,
       .size = file_size,
   };
   if (file_size == 0 || file_size > max_file_bytes || file_size % sizeof(disk_record) != 0) {
      result.malformed = true;
      return result;
   }

   result.total_records = file_size / sizeof(disk_record);
   const auto count = std::min<std::uint64_t>(result.total_records, limit);

   result.records.reserve(static_cast<std::size_t>(count));
   for (auto index = std::uint64_t{0}; index < count; ++index) {
      auto record = disk_record{};
      if (!read_exact_or_malformed(opened.fd.get(), &record, sizeof(record), entry.path) || !valid_record(record)) {
         result.records.clear();
         result.malformed = true;
         return result;
      }
      result.records.push_back(record);
   }
   return result;
}

void quarantine_file(int directory_fd, const spool_entry& entry) {
   const auto target = entry.name + ".bad";
   (void)::unlinkat(directory_fd, target.c_str(), 0);
   if (::renameat(directory_fd, entry.name.c_str(), directory_fd, target.c_str()) != 0 && errno != ENOENT) {
      throw_errno_spool_error("failed to quarantine malformed OTLP crash spool", entry.path);
   }
}

void remove_exported_records(int directory_fd, const spool_entry& entry, std::uint64_t removed, std::uint64_t total) {
   if (removed >= total) {
      if (::unlinkat(directory_fd, entry.name.c_str(), 0) != 0 && errno != ENOENT) {
         throw_errno_spool_error("failed to remove exported OTLP crash spool", entry.path);
      }
      return;
   }

   auto opened = open_existing_spool_file(directory_fd, entry);
   if (opened.missing || opened.unsafe) {
      FORGE_THROW_EXCEPTION(exceptions::spool_error, "cannot safely rewrite OTLP crash spool",
                            forge::exceptions::ctx("path", entry.path.string()));
   }
   const auto offset = static_cast<off_t>(removed * sizeof(disk_record));
   if (::lseek(opened.fd.get(), offset, SEEK_SET) < 0) {
      throw_errno_spool_error("failed to seek OTLP crash spool rewrite source", entry.path);
   }

   const auto temp_name = entry.name + ".tmp";
   (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
   auto flags = O_CREAT | O_EXCL | O_WRONLY;
#if defined(O_CLOEXEC)
   flags |= O_CLOEXEC;
#endif
#if defined(O_NOFOLLOW)
   flags |= O_NOFOLLOW;
#endif
   auto temp = fd_guard{::openat(directory_fd, temp_name.c_str(), flags, S_IRUSR | S_IWUSR)};
   const auto temp_path = entry.path.parent_path() / temp_name;
   if (temp.get() < 0) {
      throw_errno_spool_error("failed to create OTLP crash spool rewrite", temp_path);
   }

   auto buffer = std::array<char, 8192>{};
   while (true) {
      const auto read = ::read(opened.fd.get(), buffer.data(), buffer.size());
      if (read == 0) {
         break;
      }
      if (read < 0) {
         (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
         throw_errno_spool_error("failed to read OTLP crash spool rewrite source", entry.path);
      }
      write_all(temp.get(), buffer.data(), static_cast<std::size_t>(read), temp_path);
   }
   temp = fd_guard{};

   if (::renameat(directory_fd, temp_name.c_str(), directory_fd, entry.name.c_str()) != 0) {
      (void)::unlinkat(directory_fd, temp_name.c_str(), 0);
      throw_errno_spool_error("failed to replace OTLP crash spool", entry.path);
   }
}
#endif

std::string kind_text(const disk_record& record) {
   switch (static_cast<record_kind>(record.kind)) {
   case record_kind::signal:
      return "signal";
   case record_kind::terminate:
      return "terminate";
   }
   return "unknown";
}

std::chrono::sys_time<std::chrono::microseconds> timestamp_for(const disk_record& record) {
   if (record.unix_nanos == 0) {
      return std::chrono::time_point_cast<std::chrono::microseconds>(std::chrono::system_clock::now());
   }
   const auto nanos = std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(record.unix_nanos)};
   return std::chrono::time_point_cast<std::chrono::microseconds>(
       std::chrono::sys_time<std::chrono::nanoseconds>{nanos});
}

forge::stacktrace_snapshot stacktrace_for(const disk_record& record) {
   auto stacktrace = forge::stacktrace_snapshot{.backend = "crash-spool"};
   const auto count = std::min<std::uint32_t>(record.stack_count, max_stack_addresses);
   stacktrace.frames.reserve(count);
   for (auto index = std::uint32_t{0}; index < count; ++index) {
      const auto address = record.stack_addresses[index];
      if (address == 0) {
         continue;
      }
      stacktrace.frames.push_back(forge::stacktrace_frame{
          .index = stacktrace.frames.size(),
          .address = static_cast<std::uintptr_t>(address),
      });
   }
   return stacktrace;
}

forge::log_record to_log_record(const disk_record& record) {
   auto fields = forge::log_fields{};
   fields.push_back(forge::log_ctx("crash.severity", "fatal"));
   fields.push_back(forge::log_ctx("crash.kind", kind_text(record)));
   fields.push_back(forge::log_ctx("process.pid", record.pid));
   fields.push_back(forge::log_ctx("crash.sequence", record.sequence));
   if (record.signal_number != 0) {
      fields.push_back(forge::log_ctx("signal.number", record.signal_number));
   }
   if (record.fault_address != 0) {
      fields.push_back(forge::log_ctx("fault.address", record.fault_address));
   }
   const auto category = fixed_string(record.exception_category);
   if (!category.empty()) {
      fields.push_back(forge::log_ctx("exception.category", category));
      fields.push_back(forge::log_ctx("exception.code", record.exception_code));
   }

   auto log = forge::log_record{
       .level = forge::log_level::error,
       .logger = "forge.otlp.crash",
       .component = "otlp.crash",
       .message = "forge crash captured",
       .fields = std::move(fields),
       .timestamp = timestamp_for(record),
       .thread_id = "crash",
       .thread_name = "crash",
   };

   auto stacktrace = stacktrace_for(record);
   if (stacktrace.available()) {
      log.stacktrace = std::move(stacktrace);
   }
   return log;
}

} // namespace

std::vector<int> default_crash_signals() {
   auto signals = std::vector<int>{};
   auto add = [&](int signal_number) {
      if (std::find(signals.begin(), signals.end(), signal_number) == signals.end()) {
         signals.push_back(signal_number);
      }
   };
#if defined(SIGABRT)
   add(SIGABRT);
#endif
#if defined(SIGSEGV)
   add(SIGSEGV);
#endif
#if defined(SIGBUS)
   add(SIGBUS);
#endif
#if defined(SIGILL)
   add(SIGILL);
#endif
#if defined(SIGFPE)
   add(SIGFPE);
#endif
   return signals;
}

crash_guard::crash_guard(std::shared_ptr<void> impl) : impl_(std::move(impl)) {}

crash_guard::~crash_guard() = default;

crash_guard::crash_guard(crash_guard&& other) noexcept = default;

crash_guard& crash_guard::operator=(crash_guard&& other) noexcept = default;

crash_guard::operator bool() const noexcept {
   return impl_ != nullptr;
}

crash_guard install_crash_capture(crash_spool_options options) {
   validate_options(options, true);
   ensure_crash_directory(options.directory);

   auto state = std::make_shared<crash_state>();
   state->chain_after_capture = options.chain_after_capture;
   {
      const auto lock = std::scoped_lock{capture_lifecycle_mutex};
      if (active_capture.load(std::memory_order_acquire) != nullptr) {
         FORGE_THROW_EXCEPTION(exceptions::capture_active, "OTLP crash capture is already active");
      }
#if defined(__unix__) || defined(__APPLE__)
      state->pid = ::getpid();
      const auto path = spool_path_for(options.directory, static_cast<std::uint64_t>(state->pid));
      const auto opened = open_spool_file(path, options);
      state->records_written = checked_signal_count(opened.existing_records, path);
      state->max_records = checked_signal_count(opened.max_records, path);
      state->fd = opened.fd;
      state->identity = opened.identity;
#endif

      auto* expected = static_cast<crash_state*>(nullptr);
      if (!active_capture.compare_exchange_strong(expected, state.get(), std::memory_order_acq_rel)) {
         FORGE_THROW_EXCEPTION(exceptions::capture_active, "OTLP crash capture is already active");
      }
   }

   try {
#if defined(__unix__) || defined(__APPLE__)
      if (options.capture_signals) {
         struct sigaction action{};
         action.sa_sigaction = signal_handler;
         action.sa_flags = SA_SIGINFO;
         sigemptyset(&action.sa_mask);
         for (const auto signal_number : options.signals) {
            struct sigaction previous{};
            if (::sigaction(signal_number, &action, &previous) != 0) {
               FORGE_THROW_EXCEPTION(exceptions::invalid_options, "failed to install OTLP crash signal handler",
                                     forge::exceptions::ctx("signal", signal_number),
                                     forge::exceptions::ctx("errno", errno));
            }
            state->installed_signals.push_back(signal_number);
            state->previous_actions.push_back(previous);
         }
      }
#endif
      if (options.capture_terminate) {
         state->previous_terminate = std::set_terminate(terminate_handler);
         state->terminate_installed = true;
      }
   } catch (...) {
      state->restore();
      throw;
   }

   return crash_guard{std::move(state)};
}

boost::asio::awaitable<crash_resend_result> async_resend_crashes(log_exporter& exporter, crash_spool_options options) {
   validate_options(options, false);
   ensure_crash_directory(options.directory);

   auto result = crash_resend_result{};
#if defined(__unix__) || defined(__APPLE__)
   const auto directory_fd = open_spool_directory(options.directory);
   auto remaining = options.max_records_per_resend;
   for (const auto& entry : list_spool_files(options.directory, directory_fd.get())) {
      if (remaining == 0) {
         ++result.files_retained;
         break;
      }

      ++result.files_scanned;
      if (is_active_current_process_spool(directory_fd.get(), entry)) {
         ++result.files_retained;
         continue;
      }
      auto read = read_records(directory_fd.get(), entry, remaining, options.max_file_bytes);
      if (read.malformed) {
         if (!mutate_if_malformed_spool_still_quarantinable(directory_fd.get(), entry, read.snapshot,
                                                            [&] { quarantine_file(directory_fd.get(), entry); })) {
            ++result.files_retained;
            continue;
         }
         ++result.bad_files;
         continue;
      }
      if (read.records.empty()) {
         continue;
      }

      auto records = std::vector<forge::log_record>{};
      records.reserve(read.records.size());
      for (const auto& record : read.records) {
         records.push_back(to_log_record(record));
      }

      result.records_read += records.size();
      remaining -= records.size();
      const auto exported = co_await exporter.async_export(std::move(records));
      if (exported.failed_records == 0 && exported.exported_records == read.records.size()) {
         result.exported_records += exported.exported_records;
         ++result.files_exported;
         if (!read.snapshot.has_value() ||
             !mutate_if_exported_spool_still_removable(directory_fd.get(), entry, *read.snapshot, [&] {
                remove_exported_records(directory_fd.get(), entry, read.records.size(), read.total_records);
             })) {
            ++result.files_retained;
            continue;
         }
         if (read.records.size() < read.total_records) {
            ++result.files_retained;
         }
      } else {
         result.failed_records += read.records.size();
         ++result.files_retained;
      }
   }
#endif

   co_return result;
}

} // namespace forge::otlp
