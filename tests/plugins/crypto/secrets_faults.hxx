#pragma once

#include <chrono>
#include <filesystem>
#include <memory>

namespace forge::tests::secrets {

enum class failure_point { write, file_sync, file_close, publish, directory_sync, pause_after_publish, pause_hkdf };

// Executable-only POSIX interposition, scoped to one private fixture's target.
// No production options or runtime hooks participate in fault injection.
class syscall_fault {
 public:
   syscall_fault(const std::filesystem::path& target, failure_point point);
   ~syscall_fault();
   syscall_fault(const syscall_fault&) = delete;
   syscall_fault& operator=(const syscall_fault&) = delete;

   bool fired() const;
   bool wait_until_published(std::chrono::milliseconds timeout = std::chrono::seconds{10});
   bool wait_until_paused(std::chrono::milliseconds timeout = std::chrono::seconds{10});
   bool pause_timed_out() const;
   void release();

 private:
   struct state;
   std::shared_ptr<state> _state;
};

} // namespace forge::tests::secrets
