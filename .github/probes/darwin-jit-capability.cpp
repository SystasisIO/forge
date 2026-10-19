#include <TargetConditionals.h>
#include <cerrno>
#include <cinttypes>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
constexpr int effective_apple = 1;
#else
constexpr int effective_apple = 0;
#endif
#if defined(__arm64__) || defined(__aarch64__)
constexpr int effective_arm64 = 1;
#else
constexpr int effective_arm64 = 0;
#endif

namespace {
volatile sig_atomic_t active_child = 0;

void deadline(int) {
   const auto child = static_cast<pid_t>(active_child);
   if (child > 0) {
      int status = 0;
      pid_t found;
      do {
         found = waitpid(child, &status, WNOHANG);
      } while (found < 0 && errno == EINTR);
      // A reaped PID must never be killed: it could already have been reused.
      if (found == 0) {
         kill(child, SIGKILL);
         while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
         }
      }
   }
   constexpr char receipt[] = "{\"scope\":\"OS_CAPABILITY_ONLY_NOT_WAMR_ACCEPTANCE\","
                              "\"schema\":1,\"status\":2,\"harness_failure\":1,\"parent_timeout\":1}\n";
   static_cast<void>(write(STDOUT_FILENO, receipt, sizeof(receipt) - 1));
   _exit(2);
}

struct child_result {
   int attempted = 0;
   int fork_errno = 0;
   int wait_errno = 0;
   int raw_wait_status = -1;
   int exited = 0;
   int exit_code = -1;
   int signal = 0;
   int timeout = 0;
};

child_result run_child(void* mapping, int operation) {
   child_result result;
   sigset_t alarm_set, old_mask;
   sigemptyset(&alarm_set);
   sigaddset(&alarm_set, SIGALRM);
   if (sigprocmask(SIG_BLOCK, &alarm_set, &old_mask) != 0) {
      result.fork_errno = errno;
      return result;
   }
   result.attempted = 1;
   const auto child = fork();
   if (child == 0) {
      const rlimit core{0, 0};
      if (setrlimit(RLIMIT_CORE, &core) != 0)
         _exit(120);
      struct sigaction action{};
      action.sa_handler = SIG_DFL;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGALRM, &action, nullptr) != 0)
         _exit(121);
      if (sigprocmask(SIG_SETMASK, &old_mask, nullptr) != 0)
         _exit(122);
      alarm(3);
      if (operation == 1) {
         *static_cast<volatile std::uint8_t*>(mapping) = 0;
         _exit(112); // RX write unexpectedly survived.
      }
      const auto function = reinterpret_cast<int (*)()>(mapping);
      const auto value = function();
      _exit(operation == 0 ? (value == 42 ? 42 : 111) : 112);
   }
   if (child < 0)
      result.fork_errno = errno;
   else
      active_child = static_cast<sig_atomic_t>(child);
   if (sigprocmask(SIG_SETMASK, &old_mask, nullptr) != 0) {
      result.wait_errno = errno;
      if (child > 0)
         kill(child, SIGKILL);
   }
   if (child < 0)
      return result;
   int status = 0;
   pid_t waited;
   do {
      waited = waitpid(child, &status, 0);
   } while (waited < 0 && errno == EINTR);
   active_child = 0;
   if (waited < 0) {
      result.wait_errno = errno;
      return result;
   }
   result.raw_wait_status = status;
   result.exited = WIFEXITED(status);
   result.exit_code = result.exited ? WEXITSTATUS(status) : -1;
   result.signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
   result.timeout = result.signal == SIGALRM;
   return result;
}

bool harness_failed(const child_result& result) {
   return result.fork_errno || result.wait_errno || result.timeout ||
          (result.exited && result.exit_code >= 120 && result.exit_code <= 122);
}

bool protection_fault(const child_result& result) {
   return result.signal == SIGBUS || result.signal == SIGSEGV;
}

void print_child(const char* name, const child_result& value) {
   std::printf("\"%s\":{\"attempted\":%d,\"fork_errno\":%d,\"wait_errno\":%d,"
               "\"raw_wait_status\":%d,\"exited\":%d,\"exit_code\":%d,\"signal\":%d,\"timeout\":%d}",
               name, value.attempted, value.fork_errno, value.wait_errno, value.raw_wait_status, value.exited,
               value.exit_code, value.signal, value.timeout);
}
} // namespace

int main() {
   // Status: 0 expected controls, 1 capability failure, 2 harness, 3 wrong target.
   // The raw pthread JIT-supported value is independent, never substituted.
   static_assert(sizeof(pid_t) <= sizeof(sig_atomic_t));
   const auto page = sysconf(_SC_PAGESIZE);
   const auto thread_stack = pthread_get_stacksize_np(pthread_self());
   rlimit stack{}, address_space{};
   const auto stack_errno = getrlimit(RLIMIT_STACK, &stack) == 0 ? 0 : errno;
   const auto as_errno = getrlimit(RLIMIT_AS, &address_space) == 0 ? 0 : errno;
   const auto raw_jit_supported = pthread_jit_write_protect_supported_np();
   struct sigaction action{};
   action.sa_handler = deadline;
   sigemptyset(&action.sa_mask);
   const auto alarm_errno = sigaction(SIGALRM, &action, nullptr) == 0 ? 0 : errno;
   int status = page <= 0 || stack_errno || as_errno || alarm_errno ? 2 : 0;
   if (!effective_apple || !effective_arm64 || !TARGET_OS_OSX)
      status = 3;
   int mmap_errno = 0, rx_errno = 0, rw_errno = 0, unmap_errno = 0;
   int mmap_attempted = 0, rx_attempted = 0, rw_attempted = 0, icache_flushed = 0;
   child_result execute_rx, write_rx, execute_rw;
   if (status == 0) {
      alarm(15); // Whole parent lifecycle; every child also has its own 3s alarm.
      mmap_attempted = 1;
      auto* mapping =
          mmap(nullptr, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
      if (mapping == MAP_FAILED) {
         mmap_errno = errno;
         status = 1;
      } else {
         // AAPCS64: mov w0, #42; ret. No MAP_JIT, RWX or JIT policy override.
         constexpr std::uint32_t instructions[]{0x52800540, 0xd65f03c0};
         std::memcpy(mapping, instructions, sizeof(instructions));
         sys_icache_invalidate(mapping, sizeof(instructions));
         icache_flushed = 1;
         rx_attempted = 1;
         if (mprotect(mapping, static_cast<std::size_t>(page), PROT_READ | PROT_EXEC) != 0) {
            rx_errno = errno;
            status = 1;
         } else {
            execute_rx = run_child(mapping, 0);
            write_rx = run_child(mapping, 1);
            if (!execute_rx.exited || execute_rx.exit_code != 42 || !protection_fault(write_rx))
               status = 1;
         }
         rw_attempted = 1;
         if (mprotect(mapping, static_cast<std::size_t>(page), PROT_READ | PROT_WRITE) != 0) {
            rw_errno = errno;
            status = 1;
         } else {
            execute_rw = run_child(mapping, 2);
            if (!protection_fault(execute_rw))
               status = 1;
         }
         if (munmap(mapping, static_cast<std::size_t>(page)) != 0)
            unmap_errno = errno;
      }
      alarm(0);
   }
   const auto harness = status == 2 || unmap_errno || harness_failed(execute_rx) || harness_failed(write_rx) ||
                        harness_failed(execute_rw);
   if (harness)
      status = 2;
   std::printf("{\"scope\":\"OS_CAPABILITY_ONLY_NOT_WAMR_ACCEPTANCE\",\"schema\":1,"
               "\"status\":%d,\"harness_failure\":%d,\"parent_timeout\":0,"
               "\"effective_apple\":%d,\"effective_arm64\":%d,\"target_os_osx\":%d,"
               "\"raw_pthread_jit_write_protect_supported_np\":%d,\"page_size\":%ld,"
               "\"pthread_stack_bytes\":%zu,\"rlim_infinity\":%" PRIu64 ","
               "\"rlimit_stack_errno\":%d,\"rlimit_stack_soft\":%" PRIu64 ",\"rlimit_stack_hard\":%" PRIu64 ","
               "\"rlimit_as_errno\":%d,\"rlimit_as_soft\":%" PRIu64 ",\"rlimit_as_hard\":%" PRIu64 ","
               "\"alarm_setup_errno\":%d,\"mmap_attempted\":%d,\"mmap_errno\":%d,"
               "\"icache_flushed\":%d,\"rx_attempted\":%d,\"mprotect_rx_errno\":%d,"
               "\"rw_attempted\":%d,\"mprotect_rw_errno\":%d,\"munmap_errno\":%d,",
               status, static_cast<int>(harness), effective_apple, effective_arm64, TARGET_OS_OSX, raw_jit_supported,
               page, thread_stack, static_cast<std::uint64_t>(RLIM_INFINITY), stack_errno,
               static_cast<std::uint64_t>(stack.rlim_cur), static_cast<std::uint64_t>(stack.rlim_max), as_errno,
               static_cast<std::uint64_t>(address_space.rlim_cur), static_cast<std::uint64_t>(address_space.rlim_max),
               alarm_errno, mmap_attempted, mmap_errno, icache_flushed, rx_attempted, rx_errno, rw_attempted, rw_errno,
               unmap_errno);
   print_child("execute_rx", execute_rx);
   std::putchar(',');
   print_child("write_rx", write_rx);
   std::putchar(',');
   print_child("execute_rw", execute_rw);
   std::puts("}");
   return status;
}
