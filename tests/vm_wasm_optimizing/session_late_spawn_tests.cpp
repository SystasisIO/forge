#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <typeinfo>
#include <unistd.h>
#include <vector>

import forge.asio.exceptions;
import forge.asio.blocking;
import forge.exceptions;
import forge.vm.wasm.optimizing.compiler_session;

namespace {
namespace oc = forge::vm::wasm::optimizing;
using namespace std::chrono_literals;
using clock_type = std::chrono::steady_clock;

struct spawn_publication_gate {
   std::atomic_bool armed{false};
   std::string helper;
   std::mutex mutex;
   std::condition_variable changed;
   std::string workspace;
   bool entered = false;
   bool released = false;
   bool timed_out = false;
   std::atomic<pid_t> pid{-1};
   std::atomic<unsigned> launches{0};
   std::atomic<unsigned> terminal_reaps{0};
   std::atomic<int> native_status{0};

   void release() {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }
} gate;

void require(bool value, const char* message) {
   if (!value)
      throw std::runtime_error{message};
}

// A private process group contains only this fresh CTest process and its helper.
// The watchdog needs neither a thread nor a competing child waiter.
void watchdog(int) {
   constexpr char message[] = "FAIL session late-spawn cleanup watchdog (30s)\n";
   static_cast<void>(::write(STDERR_FILENO, message, sizeof(message) - 1));
   ::kill(0, SIGKILL);
   ::_exit(124);
}

void workspace_watchdog(int) {
   constexpr char message[] = "FAIL session workspace-refusal watchdog (60s)\n";
   static_cast<void>(::write(STDERR_FILENO, message, sizeof(message) - 1));
   ::kill(0, SIGKILL);
   ::_exit(124);
}

[[noreturn]] void bounded_failure(const char* message) {
   gate.release();
   std::cerr << "FAIL " << message << std::endl;
   ::kill(0, SIGKILL);
   ::_exit(125);
}

struct sigaction signal_snapshot() {
   struct sigaction value{};
   require(::sigaction(SIGCHLD, nullptr, &value) == 0, "Cannot inspect SIGCHLD");
   return value;
}

bool same_signal(const struct sigaction& before, const struct sigaction& after) {
   if (before.sa_flags != after.sa_flags ||
       ((before.sa_flags & SA_SIGINFO) != 0 ? before.sa_sigaction != after.sa_sigaction
                                            : before.sa_handler != after.sa_handler))
      return false;
   for (auto signal = 1; signal < NSIG; ++signal)
      if (sigismember(&before.sa_mask, signal) != sigismember(&after.sa_mask, signal))
         return false;
   return true;
}

void report_error(const char* stage, std::exception_ptr error) {
   std::cout << "SESSION_CLEANUP stage=" << stage;
   try {
      if (error)
         std::rethrow_exception(error);
      std::cout << " success=1\n";
   } catch (const forge::exceptions::base& failure) {
      std::cout << " success=0 type=" << typeid(failure).name() << " category=" << failure.code().category().name()
                << " code=" << failure.code().value()
                << " asio_rejected=" << (dynamic_cast<const forge::asio::exceptions::rejected*>(&failure) != nullptr)
                << " error=" << failure.what() << '\n';
   } catch (const std::exception& failure) {
      std::cout << " success=0 type=" << typeid(failure).name() << " error=" << failure.what() << '\n';
   } catch (...) {
      std::cout << " success=0 type=unknown\n";
   }
}

std::exception_ptr finish(std::future<void>& pending) {
   if (pending.wait_for(5s) != std::future_status::ready)
      bounded_failure("Owned shutdown/compute drain did not complete within 5s");
   try {
      pending.get();
      return {};
   } catch (...) {
      return std::current_exception();
   }
}

std::uint64_t refused_admission(std::exception_ptr error) {
   require(bool(error), "Workspace permission refusal unexpectedly succeeded");
   try {
      std::rethrow_exception(error);
   } catch (const oc::exceptions::shutdown_incomplete& failure) {
      require(failure.code() == oc::exceptions::make_error_code(oc::exceptions::code::shutdown_incomplete),
              "Workspace refusal has the wrong typed error code");
      for (const auto& field : failure.context())
         if (field.key == "compute_admission" && !field.redacted) {
            auto id = std::uint64_t{};
            const auto parsed = std::from_chars(field.value.data(), field.value.data() + field.value.size(), id);
            require(parsed.ec == std::errc{} && parsed.ptr == field.value.data() + field.value.size() && id,
                    "Workspace refusal lost its actual retained admission ID");
            return id;
         }
      throw std::runtime_error{"Typed workspace refusal lacks its retained admission context"};
   }
}

void require_cache_locked(int descriptor) {
   errno = 0;
   const auto result = ::flock(descriptor, LOCK_EX | LOCK_NB);
   const auto error = errno;
   if (result == 0)
      static_cast<void>(::flock(descriptor, LOCK_UN));
   require(result == -1 && (error == EWOULDBLOCK || error == EAGAIN),
           "Incomplete workspace cleanup released the exclusive cache lock");
}
} // namespace

// Delegate unchanged to the real OS spawn. Only the dedicated mode arms this
// gate; ordinary session_bounded calls pass straight through every interposer.
extern "C" int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
                           const posix_spawnattr_t* attributes, char* const argv[], char* const environment[]) {
   static const auto actual = reinterpret_cast<decltype(&::posix_spawn)>(::dlsym(RTLD_NEXT, "posix_spawn"));
   if (!actual)
      return ENOSYS;
   const auto result = actual(pid, path, actions, attributes, argv, environment);
   if (result == 0 && gate.armed.load(std::memory_order_acquire) && std::strcmp(path, gate.helper.c_str()) == 0) {
      gate.pid.store(*pid, std::memory_order_release);
      gate.launches.fetch_add(1, std::memory_order_relaxed);
      auto lock = std::unique_lock{gate.mutex};
      gate.entered = true;
      gate.changed.notify_all();
      gate.timed_out = !gate.changed.wait_for(lock, 10s, [] { return gate.released; });
   }
   return result; // Same successful PID, held before production publication.
}

extern "C" int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t* actions, const char* path) {
   static const auto actual = reinterpret_cast<decltype(&::posix_spawn_file_actions_addchdir_np)>(
       ::dlsym(RTLD_NEXT, "posix_spawn_file_actions_addchdir_np"));
   if (!actual)
      return ENOSYS;
   const auto result = actual(actions, path);
   if (result == 0 && gate.armed.load(std::memory_order_acquire)) {
      const auto lock = std::scoped_lock{gate.mutex};
      gate.workspace = path; // Observe the actual helper cwd, never scan/delete unrelated temporary directories.
   }
   return result;
}

// Passive observation of the production Boost.Process waiter, not a second
// reaper: no extra call, altered result, injected error or synthetic PID/status.
extern "C" pid_t waitpid(pid_t pid, int* status, int options) {
   static const auto actual = reinterpret_cast<decltype(&::waitpid)>(::dlsym(RTLD_NEXT, "waitpid"));
   if (!actual) {
      errno = ENOSYS;
      return -1;
   }
   const auto result = actual(pid, status, options);
   const auto saved_errno = errno;
   if (gate.armed.load(std::memory_order_acquire) && pid > 0 && pid == gate.pid.load(std::memory_order_acquire) &&
       result == pid && status && (WIFEXITED(*status) || WIFSIGNALED(*status))) {
      gate.native_status.store(*status, std::memory_order_relaxed);
      gate.terminal_reaps.fetch_add(1, std::memory_order_release);
   }
   errno = saved_errno;
   return result;
}

int session_late_spawn_cleanup(int argc, char** argv) {
   try {
      if (argc != 6)
         return 2;
      require((::getpgrp() == ::getpid() || ::setpgid(0, 0) == 0) && ::getpgrp() == ::getpid(),
              "Cannot isolate watchdog process group");
      struct sigaction alarm_action{};
      alarm_action.sa_handler = watchdog;
      sigemptyset(&alarm_action.sa_mask);
      require(::sigaction(SIGALRM, &alarm_action, nullptr) == 0, "Cannot install bounded process watchdog");
      auto alarm_mask = sigset_t{};
      sigemptyset(&alarm_mask);
      sigaddset(&alarm_mask, SIGALRM);
      require(::sigprocmask(SIG_UNBLOCK, &alarm_mask, nullptr) == 0, "Cannot unblock process watchdog");
      ::alarm(30);
      std::cout << std::unitbuf;
      std::cerr << std::unitbuf;
      auto address = std::uint64_t{};
      const auto text = std::string{argv[5]};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address,
              "Explicit finite helper address-space bytes are required");
      require(std::filesystem::file_size(argv[3]) <= 1024 * 1024, "Session request exceeds 1 MiB");
      auto input = std::ifstream{argv[3], std::ios::binary};
      require(bool(input), "Cannot read current WAMR session request");
      const auto bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
      const auto request = oc::compile_request::decode(bytes, 1024 * 1024);
      require(request.configuration.target == oc::target_profile::native(), "Request is not the native profile");
      const auto identity = oc::identify(request);
      auto pattern = (std::filesystem::path{argv[4]} / "session-late-spawn-XXXXXX").string();
      require(::mkdtemp(pattern.data()) != nullptr, "Cannot create owned late-spawn cache");
      auto configuration = oc::session_options{};
      configuration.helper = argv[2];
      configuration.cache = pattern;
      configuration.worker_count = 1;
      configuration.max_pending_jobs = 4;
      configuration.max_tickets = 8;
      configuration.max_imports = 8;
      configuration.max_configuration_bytes = 16384;
      configuration.max_request_bytes = 1024 * 1024;
      configuration.max_response_bytes = 1024 * 1024;
      configuration.max_inflight_bytes = 512ULL * 1024 * 1024;
      configuration.max_resident_bytes = 64 * 1024 * 1024;
      configuration.max_disk_bytes = 32 * 1024 * 1024;
      configuration.max_disk_entries = 8;
      configuration.helper_timeout = 120s;
      configuration.helper_limits = {120, address, 16 * 1024 * 1024};
      const auto before_signal = signal_snapshot();
      require(before_signal.sa_handler == SIG_DFL && before_signal.sa_flags == 0, "Nondefault cold SIGCHLD");
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = 1, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
      auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
      try {
         const auto base = session.usage();
         gate.helper = configuration.helper.string();
         gate.armed.store(true, std::memory_order_release);
         // Any failure below retains non-quiescent owners until subprocess exit,
         // after recording the ORIGINAL error. Never mask RED with destructor terminate().
         auto failure = std::exception_ptr{};
         auto stopped = std::future<void>{};
         auto held = false;
         auto premature = false;
         auto registered = false;
         auto workspace = std::filesystem::path{};
         try {
            const auto release_on_error = boost::scope::scope_exit{[] { gate.release(); }};
            static_cast<void>(session.initialize(oc::target_profile::native(), {})); // Discard eager waiter.
            {
               auto lock = std::unique_lock{gate.mutex};
               held = gate.changed.wait_for(lock, 5s, [] { return gate.entered; });
               workspace = gate.workspace;
            }
            require(held && gate.pid.load(std::memory_order_acquire) > 0, "Real successful spawn did not enter gate");
            require(!workspace.empty() && std::filesystem::is_directory(workspace), "Missing actual helper workspace");
            require(::kill(gate.pid.load(std::memory_order_acquire), 0) == 0, "Gated real helper is not alive");
            registered = !same_signal(before_signal, signal_snapshot());
            require(registered, "Cold session did not retain its SIGCHLD registration");
            std::cout << "SESSION_CLEANUP stage=held pid=" << gate.pid.load() << " workspace=" << workspace
                      << " cache=" << configuration.cache << " identity=" << identity.str()
                      << " compute_workers=1 session_workers=1\n";
            compute.request_stop();
            stopped = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                            boost::asio::use_future);
            premature = stopped.wait_for(25ms) == std::future_status::ready;
            require(!premature, "Shutdown falsely completed while successful PID publication was held");
         } catch (...) {
            failure = std::current_exception();
         }
         gate.release(); // Also released by the inner scope guard on EVERY failure.
         std::cout << "SESSION_CLEANUP stage=released held=" << held << " premature=" << premature << '\n';
         if (!stopped.valid())
            stopped = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                            boost::asio::use_future);
         const auto shutdown_error = finish(stopped);
         report_error("shutdown", shutdown_error);
         auto retry_error = std::exception_ptr{};
         if (shutdown_error) {
            auto retry = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                               boost::asio::use_future);
            retry_error = finish(retry);
            report_error("shutdown-retry", retry_error);
         }
         auto pool_drain = boost::asio::co_spawn(runtime.context(), compute.shutdown(), boost::asio::use_future);
         const auto pool_error = finish(pool_drain);
         report_error("compute-drain", pool_error);
         const auto pid = gate.pid.load(std::memory_order_acquire);
         const auto reaps = gate.terminal_reaps.load(std::memory_order_acquire);
         auto waited = pid_t{-2};
         auto wait_error = int{};
         // Query ECHILD only AFTER observing the sole production waiter's terminal
         // reap. An incomplete shutdown never gives this test live reap authority.
         if (reaps == 1) {
            auto status = int{};
            do {
               errno = 0;
               waited = ::waitpid(pid, &status, WNOHANG);
               wait_error = errno;
            } while (waited < 0 && wait_error == EINTR);
         }
         const auto workspace_exists = !workspace.empty() && std::filesystem::exists(workspace);
         const auto signal_restored = same_signal(before_signal, signal_snapshot());
         const auto usage = session.usage();
         std::cout << "SESSION_CLEANUP stage=observed pid=" << pid << " launches=" << gate.launches.load()
                   << " production_terminal_reaps=" << reaps << " native_status=" << gate.native_status.load()
                   << " post_reap_waitpid=" << waited << " errno=" << wait_error << " workspace=" << workspace
                   << " workspace_exists=" << workspace_exists << " registration_restored=" << signal_restored
                   << " resident=" << usage.resident_bytes << " base_resident=" << base.resident_bytes
                   << " inflight=" << usage.inflight_bytes << " base_inflight=" << base.inflight_bytes << '\n';
         try {
            if (failure)
               std::rethrow_exception(failure);
            if (shutdown_error)
               std::rethrow_exception(shutdown_error); // This regression is RED, never expected-error PASS.
            if (pool_error)
               std::rethrow_exception(pool_error);
            require(!gate.timed_out && !premature && gate.launches == 1, "Late-publication timeline was not preserved");
            require(reaps == 1 && waited == -1 && wait_error == ECHILD, "Missing sole owned reap/post-drain ECHILD");
            require(!workspace_exists && signal_restored && registered, "Workspace/registration cleanup is incomplete");
            require(!session.ready(identity, request.configuration), "Stopped session retained ready admission");
            require(usage.resident_bytes == base.resident_bytes && usage.inflight_bytes == base.inflight_bytes,
                    "Shutdown did not restore exact reservation baselines (no independent pins)");
         } catch (...) {
            report_error("FAILED", std::current_exception());
            if (reaps != 1)
               bounded_failure("No confirmed owned terminal reap; terminating only the isolated test process group");
            std::_Exit(1); // Keep retained owners/workspace as evidence; no invalid lifecycle destruction.
         }
         gate.armed.store(false, std::memory_order_release);
         std::cout << "PASS late successful spawn, stopped-compute owned drain/workspace/registration/accounting\n";
         return 0;
      } catch (...) {
         gate.release();
         report_error("unexpected-failure", std::current_exception());
         if (gate.terminal_reaps.load(std::memory_order_acquire) != 1)
            bounded_failure("Unexpected failure without confirmed owned terminal reap");
         std::_Exit(1);
      }
   } catch (const std::exception& error) {
      gate.release();
      std::cerr << "FAIL late-spawn setup: " << error.what() << '\n';
      return 1;
   }
}

int session_workspace_refusal(int argc, char** argv) {
   try {
      if (argc != 6)
         return 2;
      require(::geteuid() != 0, "HARNESS_ERROR permission refusal requires an unprivileged process");
      require((::getpgrp() == ::getpid() || ::setpgid(0, 0) == 0) && ::getpgrp() == ::getpid(),
              "Cannot isolate workspace-refusal watchdog process group");
      struct sigaction alarm_action{};
      alarm_action.sa_handler = workspace_watchdog;
      sigemptyset(&alarm_action.sa_mask);
      require(::sigaction(SIGALRM, &alarm_action, nullptr) == 0, "Cannot install bounded process watchdog");
      auto alarm_mask = sigset_t{};
      sigemptyset(&alarm_mask);
      sigaddset(&alarm_mask, SIGALRM);
      require(::sigprocmask(SIG_UNBLOCK, &alarm_mask, nullptr) == 0, "Cannot unblock process watchdog");
      ::alarm(60);
      std::cout << std::unitbuf;
      std::cerr << std::unitbuf;
      auto address = std::uint64_t{};
      const auto text = std::string{argv[5]};
      const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address);
      require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address,
              "Explicit finite helper address-space bytes are required");
      require(std::filesystem::file_size(argv[3]) <= 1024 * 1024, "Session request exceeds 1 MiB");
      auto input = std::ifstream{argv[3], std::ios::binary};
      require(bool(input), "Cannot read current WAMR session request");
      const auto bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
      const auto request = oc::compile_request::decode(bytes, 1024 * 1024);
      require(request.configuration.target == oc::target_profile::native(), "Request is not the native profile");
      const auto identity = oc::identify(request);
      auto pattern = (std::filesystem::path{argv[4]} / "session-workspace-refusal-XXXXXX").string();
      require(::mkdtemp(pattern.data()) != nullptr, "Cannot create owned workspace-refusal cache");
      auto configuration = oc::session_options{};
      configuration.helper = argv[2];
      configuration.cache = pattern;
      configuration.worker_count = 1;
      configuration.max_pending_jobs = 4;
      configuration.max_tickets = 8;
      configuration.max_imports = 8;
      configuration.max_configuration_bytes = 16384;
      configuration.max_request_bytes = 1024 * 1024;
      configuration.max_response_bytes = 1024 * 1024;
      configuration.max_inflight_bytes = 512ULL * 1024 * 1024;
      configuration.max_resident_bytes = 64 * 1024 * 1024;
      configuration.max_disk_bytes = 32 * 1024 * 1024;
      configuration.max_disk_entries = 8;
      configuration.helper_timeout = 120s;
      configuration.helper_limits = {120, address, 16 * 1024 * 1024};
      const auto before_signal = signal_snapshot();
      require(before_signal.sa_handler == SIG_DFL && before_signal.sa_flags == 0, "Nondefault cold SIGCHLD");
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
          .worker_threads = 1, .max_pending_tasks = 0, .max_waiting_submissions = 8}};
      auto session = oc::compiler_session{runtime, compute.get_executor(), configuration};
      auto blocked = std::filesystem::path{};
      try {
         const auto base = session.usage();
         auto initialize = boost::asio::co_spawn(
             runtime.context(), session.initialize(oc::target_profile::native(), {}), boost::asio::use_future);
         if (const auto error = finish(initialize))
            std::rethrow_exception(error);
         require(compute.snapshot().retained == 0, "Initialize retained admission into cache work");
         const auto owned_signal = signal_snapshot();
         require(!same_signal(before_signal, owned_signal), "Initialized session has no SIGCHLD owner");
         const auto lock_path = configuration.cache / "owner.lock";
         const auto descriptor = ::open(lock_path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
         require(descriptor >= 0, "Cannot inspect actual owned cache lock");
         const auto close_lock = boost::scope::scope_exit{[descriptor] { ::close(descriptor); }};
         require_cache_locked(descriptor);
         gate.helper = configuration.helper.string();
         gate.armed.store(true, std::memory_order_release);
         auto admitted = session.enqueue(identity, oc::compile_request{request}, forge::asio::task::priority{});
         require(admitted.status == oc::admission_status::accepted_unverified && admitted.completion,
                 "Permission-refusal request was not admitted");
         auto stopped = std::future<void>{};
         auto workspace = std::filesystem::path{};
         {
            const auto release_on_error = boost::scope::scope_exit{[] { gate.release(); }};
            {
               auto lock = std::unique_lock{gate.mutex};
               require(gate.changed.wait_for(lock, 5s, [] { return gate.entered; }),
                       "Real successful compilation spawn did not enter gate");
               workspace = gate.workspace;
            }
            require(!workspace.empty() && std::filesystem::is_directory(workspace), "Missing actual helper workspace");
            require(gate.pid.load(std::memory_order_acquire) > 0 && ::kill(gate.pid.load(), 0) == 0,
                    "Gated real compilation helper is not alive");
            blocked = workspace / "test-owned-refusal";
            require(std::filesystem::create_directory(blocked), "Cannot create test-owned cleanup refusal directory");
            {
               auto marker = std::ofstream{blocked / "marker", std::ios::binary};
               marker << "owned cleanup refusal";
               marker.close();
               require(bool(marker), "Cannot create nonempty cleanup refusal marker");
            }
            require(::chmod(blocked.c_str(), 0500) == 0, "Cannot set test-owned refusal permissions");
            compute.request_stop();
            stopped = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                            boost::asio::use_future);
            require(stopped.wait_for(25ms) != std::future_status::ready,
                    "Shutdown completed before real successful PID publication");
         }
         const auto first_error = finish(stopped);
         report_error("workspace-refusal-first", first_error);
         const auto first_id = refused_admission(first_error);
         const auto held = session.usage();
         const auto first_metrics = compute.snapshot();
         require(first_metrics.stopped && first_metrics.retained == 1 && first_metrics.running == 0 &&
                     first_metrics.pending == 0 && first_metrics.waiting == 0,
                 "Workspace refusal lost retained idle admission or still owns callback captures");
         require(held.inflight_bytes > base.inflight_bytes && held.inflight_bytes <= configuration.max_inflight_bytes,
                 "Workspace refusal lost its bounded live job charge");
         require(std::filesystem::is_regular_file(blocked / "marker") && same_signal(owned_signal, signal_snapshot()),
                 "Workspace refusal removed protected marker or released SIGCHLD ownership");
         require_cache_locked(descriptor);
         auto pool_drain = boost::asio::co_spawn(runtime.context(), compute.shutdown(), boost::asio::use_future);
         require(pool_drain.wait_for(25ms) != std::future_status::ready,
                 "Pool shutdown ignored retained cleanup owner");
         auto retry = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                            boost::asio::use_future);
         const auto retry_error = finish(retry);
         report_error("workspace-refusal-second", retry_error);
         const auto second_id = refused_admission(retry_error);
         require(first_id == second_id && compute.snapshot().retained == 1,
                 "Repeated refusal replaced or released the SAME retained admission");
         const auto repeated = session.usage();
         require(repeated.resident_bytes == held.resident_bytes && repeated.inflight_bytes == held.inflight_bytes,
                 "Repeated refusal changed retained accounting");
         require_cache_locked(descriptor);
         require(same_signal(owned_signal, signal_snapshot()), "Repeated refusal released SIGCHLD ownership");
         auto after_stop = session.enqueue(identity, oc::compile_request{request}, forge::asio::task::priority{});
         require(after_stop.status == oc::admission_status::stopped && !after_stop.completion &&
                     session.usage().inflight_bytes == held.inflight_bytes,
                 "Stopped session admitted or charged new work");
         require(::chmod(blocked.c_str(), 0700) == 0, "Cannot restore test-owned cleanup permissions");
         auto recovered = boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - 1ms),
                                                boost::asio::use_future);
         if (const auto error = finish(recovered))
            std::rethrow_exception(error);
         if (const auto error = finish(pool_drain))
            std::rethrow_exception(error);
         auto canceled = false;
         try {
            static_cast<void>(
                forge::asio::blocking::run(runtime, std::move(*admitted.completion).wait(clock_type::now() + 5s, {})));
         } catch (const oc::exceptions::cancelled&) {
            canceled = true;
         }
         admitted.completion.reset();
         require(canceled, "Stopped real-helper request did not preserve typed cancellation");
         const auto refunded = session.usage();
         require(refunded.resident_bytes == base.resident_bytes && refunded.inflight_bytes == base.inflight_bytes,
                 "Successful cleanup retry did not restore exact accounting baselines");
         require(compute.snapshot().retained == 0 && !std::filesystem::exists(workspace) &&
                     same_signal(before_signal, signal_snapshot()),
                 "Successful retry left retained admission/workspace/SIGCHLD ownership");
         require(::flock(descriptor, LOCK_EX | LOCK_NB) == 0 && ::flock(descriptor, LOCK_UN) == 0,
                 "Successful retry did not release cache lock");
         const auto pid = gate.pid.load(std::memory_order_acquire);
         require(!gate.timed_out && gate.launches == 1 && gate.terminal_reaps == 1,
                 "Cleanup retry changed actual launch/PID/sole-reap count");
         auto status = int{};
         errno = 0;
         require(::waitpid(pid, &status, WNOHANG) == -1 && errno == ECHILD, "Missing post-drain ECHILD");
         gate.armed.store(false, std::memory_order_release);

         // A fresh existing pool/session must reuse the SAME runtime/cache and
         // complete preparation at capacity=1, not retain admission into prepare.
         auto fresh_compute = forge::asio::compute::pool{forge::asio::compute::pool::options{
             .worker_threads = 1, .max_pending_tasks = 0, .max_waiting_submissions = 8}};
         auto fresh = oc::compiler_session{runtime, fresh_compute.get_executor(), configuration};
         auto recovery_error = std::exception_ptr{};
         try {
            forge::asio::blocking::run(runtime, fresh.initialize(oc::target_profile::native(), {}));
            auto queued = fresh.enqueue(identity, oc::compile_request{request}, forge::asio::task::priority{});
            require(queued.status == oc::admission_status::accepted_unverified && queued.completion,
                    "Fresh session could not reuse cache after cleanup retry");
            auto code =
                forge::asio::blocking::run(runtime, std::move(*queued.completion).wait(clock_type::now() + 15s, {}));
            require(code.code.is_prepared(), "Capacity-one recovery did not finish real native preparation");
            require(fresh_compute.snapshot().retained == 0, "Recovery retained helper admission after preparation");
         } catch (...) {
            recovery_error = std::current_exception();
         }
         forge::asio::blocking::run(runtime, fresh.shutdown(clock_type::now() + 2s));
         forge::asio::blocking::run(runtime, fresh_compute.shutdown());
         require(same_signal(before_signal, signal_snapshot()), "Fresh session failed to release SIGCHLD ownership");
         if (recovery_error)
            std::rethrow_exception(recovery_error);
         std::cout << "PASS workspace permission refusal twice, same_admission=" << first_id << " pid=" << pid
                   << " retained_idle=1->0, stopped-pool drain/retry, exact refund, cache/SIGCHLD reuse, capacity=1\n";
         return 0;
      } catch (...) {
         gate.release();
         if (!blocked.empty())
            static_cast<void>(::chmod(blocked.c_str(), 0700)); // Restore only this test's own denied directory.
         report_error("workspace-refusal-FAILED", std::current_exception());
         bounded_failure("Workspace-refusal regression failed; terminating only its isolated process group");
      }
   } catch (const std::exception& error) {
      gate.release();
      std::cerr << "FAIL workspace-refusal setup: " << error.what() << '\n';
      return 1;
   }
}
