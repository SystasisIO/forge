#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

namespace swarm_test::faults {

enum class crash_edge { before, after };
enum class close_target { any, checkpoint_temporary, ownership_lock };

struct plan {
   std::uint64_t fsync_eio = 0;
   std::uint64_t write_enospc = 0;
   std::uint64_t write_eintr = 0;
   std::uint64_t read_eintr = 0;
   std::size_t short_write = 0;
   std::size_t short_read = 0;
   std::uint64_t pause_first_writes = 0;
   bool pause_published_parent = false;
   std::uint64_t crash_fsync = 0;
   crash_edge edge = crash_edge::before;
   std::uint64_t close_eio = 0;
   close_target close_filter = close_target::any;
   bool rename_exdev_once = false;
   bool pause_publish_rename = false;
};

struct observation {
   std::uint64_t syncs = 0;
   std::uint64_t writes = 0;
   std::uint64_t reads = 0;
   std::uint64_t injected_errors = 0;
   std::uint64_t paused = 0;
   bool timed_out = false;
   std::uint64_t closes = 0;
   std::uint64_t renames = 0;
};

// Resolving symbols is not proof that a production call was intercepted: tests must check counters too.
[[nodiscard]] bool available();
// Linux production uses syscall(SYS_renameat2), not an interposable libc rename symbol.
[[nodiscard]] bool rename_available();

class scope {
 public:
   explicit scope(plan = {});
   ~scope();
   scope(const scope&) = delete;
   scope& operator=(const scope&) = delete;
   [[nodiscard]] observation snapshot() const;
   [[nodiscard]] bool wait_paused(std::uint64_t count = 1);
   void release() noexcept;
   void disarm() noexcept;

   struct state;

 private:
   std::shared_ptr<state> _state;
};

} // namespace swarm_test::faults
