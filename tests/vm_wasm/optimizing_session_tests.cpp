#include "test_prelude.hpp"
#include "fresh_process.hpp"

#include <cstddef>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <chrono>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <coroutine>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#include <span>
#include <array>
#include <cerrno>
#include <csignal>
#include <stop_token>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>

#if defined(FORGE_OC_HELPER_RESOURCE_DIAGNOSTIC)
#include <cstdio>
#include <exception>
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <exception>
#include <mutex>
#include <spawn.h>
#include <typeinfo>
#endif

import forge.asio.blocking;
import forge.exceptions;
import forge.raw.raw;
import forge.vm.wasm.optimizing.compiler_session;
import forge.vm.wasm.optimizing.executor;

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
namespace {
struct spawn_publication_gate {
   std::string helper;
   std::mutex mutex;
   std::condition_variable changed;
   pid_t pid = -1;
   bool entered = false;
   bool released = false;
   bool timed_out = false;
   bool delay = false;
   std::atomic<unsigned> launches{0};
   std::atomic<unsigned> overrides{0};
   std::atomic<bool> exit_next{false};
   std::atomic<bool> stop_next{false};
   std::atomic<pid_t> stopped_pid{-1};
   std::atomic<pid_t> latest_pid{-1};
   std::atomic<int> stop_result{-2};
   std::atomic<unsigned> reap_failures{0};
   std::atomic<unsigned> reap_calls{0};
   std::atomic<unsigned> injected_reap_failures{0};
   std::atomic<std::uint64_t> watched_device{0};
   std::atomic<std::uint64_t> watched_inode{0};
   std::atomic<unsigned> watched_reads{0};

   bool wait_for_entry() {
      auto lock = std::unique_lock{mutex};
      return changed.wait_for(lock, std::chrono::seconds{2}, [this] { return entered; });
   }
   void release() {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }
};
std::atomic<spawn_publication_gate*> active_spawn_gate{nullptr};
} // namespace

// Isolated executable only, like the Asio allocation-symbol control. Delegate
// to the REAL public OS function; never synthesize a PID, protocol or helper.
extern "C" int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
                           const posix_spawnattr_t* attributes, char* const argv[], char* const environment[]) {
   using function = decltype(&::posix_spawn);
   static const auto actual = reinterpret_cast<function>(::dlsym(RTLD_NEXT, "posix_spawn"));
   if (actual == nullptr)
      return ENOSYS;
   const auto gate = active_spawn_gate.load(std::memory_order_acquire);
   const auto matches = gate != nullptr && std::strcmp(path, gate->helper.c_str()) == 0;
   auto selected = path;
   if (matches) {
      gate->launches.fetch_add(1, std::memory_order_relaxed);
      if (gate->exit_next.exchange(false, std::memory_order_acq_rel)) {
         selected = "/usr/bin/false";
         gate->overrides.fetch_add(1, std::memory_order_relaxed);
      }
   }
   const auto result = actual(pid, selected, actions, attributes, argv, environment);
   if (result == 0 && matches)
      gate->latest_pid.store(*pid, std::memory_order_release);
   if (result == 0 && matches && gate->stop_next.exchange(false, std::memory_order_acq_rel)) {
      gate->stopped_pid.store(*pid, std::memory_order_release);
      gate->stop_result.store(::kill(*pid, SIGSTOP), std::memory_order_release);
   }
   if (result == 0 && matches && gate->delay) {
      auto lock = std::unique_lock{gate->mutex};
      gate->pid = *pid;
      gate->entered = true;
      gate->changed.notify_all();
      gate->timed_out = !gate->changed.wait_for(lock, std::chrono::seconds{2}, [gate] { return gate->released; });
   }
   return result; // SAME actual successful launch, delayed before owner publication.
}

// Isolated Darwin ownership-failure control. Fail only the SAME real spawned
// helper's public nonblocking reap, without changing its PID/status or calling
// the actual reaper on the injected path. This is ENOMEM from waitpid, NOT a
// constructor/allocation failure or an ECHILD ownership-loss simulation.
extern "C" pid_t waitpid(pid_t pid, int* status, int options) {
   using function = decltype(&::waitpid);
   static const auto actual = reinterpret_cast<function>(::dlsym(RTLD_NEXT, "waitpid"));
   if (actual == nullptr) {
      errno = ENOSYS;
      return -1;
   }
   const auto gate = active_spawn_gate.load(std::memory_order_acquire);
   if (gate != nullptr && pid > 0 && pid == gate->latest_pid.load(std::memory_order_acquire) && options == WNOHANG) {
      gate->reap_calls.fetch_add(1, std::memory_order_relaxed);
      auto failures = gate->reap_failures.load(std::memory_order_acquire);
      while (failures != 0) {
         if (gate->reap_failures.compare_exchange_weak(failures, failures - 1, std::memory_order_acq_rel)) {
            gate->injected_reap_failures.fetch_add(1, std::memory_order_release);
            errno = ENOMEM;
            return -1;
         }
      }
   }
   return actual(pid, status, options);
}

// Observe ONLY reads of one actual known unrelated cache inode. The public
// read/fstat boundary changes no result, bytes, FD ownership or errno, and all
// other descriptors delegate unchanged. No product instrumentation/hook.
extern "C" ssize_t read(int fd, void* buffer, std::size_t bytes) {
   using function = decltype(&::read);
   static const auto actual = reinterpret_cast<function>(::dlsym(RTLD_NEXT, "read"));
   if (actual == nullptr) {
      errno = ENOSYS;
      return -1;
   }
   const auto saved_error = errno;
   const auto gate = active_spawn_gate.load(std::memory_order_acquire);
   if (gate != nullptr) {
      const auto inode = gate->watched_inode.load(std::memory_order_acquire);
      struct stat state{};
      if (inode != 0 && ::fstat(fd, &state) == 0 && static_cast<std::uint64_t>(state.st_ino) == inode &&
          static_cast<std::uint64_t>(state.st_dev) == gate->watched_device.load(std::memory_order_relaxed))
         gate->watched_reads.fetch_add(1, std::memory_order_relaxed);
   }
   errno = saved_error;
   return actual(fd, buffer, bytes);
}
#endif

namespace {
namespace oc = forge::vm::wasm::optimizing;
using clock_type = std::chrono::steady_clock;

std::vector<std::uint8_t> read_fixture(std::string_view suffix, bool native = true) {
   const auto* directory = std::getenv("FORGE_OC_SESSION_REQUEST_DIRECTORY");
   BOOST_REQUIRE_MESSAGE(directory != nullptr, "accepted compiler fixture directory is required");
#if defined(__APPLE__)
   const auto path =
       std::filesystem::path{directory} / ((native ? "add-darwin-arm64" : "add-linux-x86-64") + std::string{suffix});
#else
   const auto path =
       std::filesystem::path{directory} / ((native ? "add-linux-x86-64" : "add-darwin-arm64") + std::string{suffix});
#endif
   BOOST_REQUIRE(std::filesystem::is_regular_file(path));
   BOOST_REQUIRE(std::filesystem::file_size(path) <= 1024U * 1024U);
   auto input = std::ifstream{path, std::ios::binary};
   BOOST_REQUIRE(input.good());
   auto value = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
   BOOST_REQUIRE(!input.bad());
   return value;
}

std::vector<std::uint8_t> read_request() {
   return read_fixture(".request");
}

oc::session_options options(const std::filesystem::path& cache, const std::filesystem::path& helper) {
   auto value = oc::session_options{};
   value.helper = helper;
   value.cache = cache;
   value.worker_count = 2;
   value.max_pending_jobs = 4;
   value.max_tickets = 8;
   value.max_imports = 8;
   value.max_configuration_bytes = 16384;
   value.max_request_bytes = 1024U * 1024U;
   value.max_response_bytes = 1024U * 1024U;
   value.max_inflight_bytes = 512ULL * 1024U * 1024U;
   value.max_resident_bytes = 8U * 1024U * 1024U;
   value.max_disk_bytes = 8U * 1024U * 1024U;
   value.max_disk_entries = 8;
   value.helper_timeout = std::chrono::seconds{2};
   // Explicit finite TEST input, not a production default or silent growth of
   // the operator's allowance. Darwin's large loader/allocator VA baseline is
   // measured separately. This does not establish RSS or sandbox guarantees.
   const auto* configured = std::getenv("FORGE_OC_HELPER_TEST_AS_BYTES");
   BOOST_REQUIRE_MESSAGE(configured != nullptr, "explicit diagnostic helper AS bytes are required");
   const auto text = std::string_view{configured};
   auto address_bytes = std::uint64_t{};
   const auto parsed = std::from_chars(text.data(), text.data() + text.size(), address_bytes);
   BOOST_REQUIRE(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && address_bytes > 0);
   value.helper_limits = {10, address_bytes, 16U * 1024U * 1024U};
   return value;
}

// co_spawn<T> requires a default T for its error-delivery branch. Preserve the
// real successful completion's mandatory prepared artifact; only this ordinary
// test root wraps a co_await in optional, never creates a default/fake artifact.
boost::asio::awaitable<std::optional<oc::compiler_session::completion>>
collect_completion(boost::asio::awaitable<oc::compiler_session::completion> value) {
   co_return co_await std::move(value);
}

oc::compiler_session::completion await_ticket(forge::asio::runtime& runtime, oc::compiler_session::ticket ticket) {
   auto result = boost::asio::co_spawn(
       runtime.context(), collect_completion(std::move(ticket).wait(clock_type::now() + std::chrono::seconds{3}, {})),
       boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   auto completed = result.get();
   BOOST_REQUIRE(completed.has_value());
   return std::move(*completed);
}

void shutdown(forge::asio::runtime& runtime, oc::compiler_session& session) {
   auto result =
       boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() + std::chrono::milliseconds{100}),
                             boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   result.get();
}

void initialize(forge::asio::runtime& runtime, oc::compiler_session& session, std::stop_token stop = {}) {
   auto result = boost::asio::co_spawn(runtime.context(), session.initialize(oc::target_profile::native(), stop),
                                       boost::asio::use_future);
   BOOST_REQUIRE(result.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
   result.get();
}

void remove_owned_cache(const std::string& directory, const forge::crypto::digest::sha256& identity) {
   ::unlink((std::filesystem::path{directory} / (identity.str() + ".foc")).c_str());
   ::unlink((std::filesystem::path{directory} / "owner.lock").c_str());
   ::rmdir(directory.c_str());
}

bool enter_fresh(const char* name, std::chrono::seconds timeout = std::chrono::seconds{4}) {
   if (fresh_process::mode() != nullptr) {
      fresh_process::disable_core_dumps();
      return true;
   }
   const auto child = fresh_process::run(name, name, timeout);
   BOOST_REQUIRE(child.error == 0);
   BOOST_REQUIRE(!child.timed_out);
   BOOST_REQUIRE(WIFEXITED(child.status));
   BOOST_TEST(WEXITSTATUS(child.status) == 0);
   return false;
}

// A local fixture for this SAME ordinary test owner, not a product lifecycle
// adapter: every captured assertion/failure is reported only after both drains.
template <typename Body> void exercise_session(oc::session_options configuration, Body&& body) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{
       forge::asio::compute::pool::options{.worker_threads = 2, .max_pending_tasks = 8, .max_waiting_submissions = 8}};
   auto error = std::exception_ptr{};
   {
      auto session = oc::compiler_session{runtime, compute.get_executor(), std::move(configuration)};
      try {
         body(session, runtime, compute);
      } catch (...) {
         error = std::current_exception();
      }
      try {
         shutdown(runtime, session);
      } catch (...) {
         if (!error)
            error = std::current_exception();
         shutdown(runtime, session); // explicit retry with injected owners retained
      }
   }
   auto drain = boost::asio::co_spawn(runtime.context(), compute.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(drain.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   drain.get();
   if (error)
      std::rethrow_exception(error);
}

void write_owned_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
   const auto fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
   BOOST_REQUIRE(fd >= 0);
   const auto close_file = boost::scope::scope_exit{[fd] { ::close(fd); }};
   auto offset = std::size_t{};
   while (offset < bytes.size()) {
      const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (count < 0 && errno == EINTR)
         continue;
      BOOST_REQUIRE(count > 0);
      offset += static_cast<std::size_t>(count);
   }
   BOOST_REQUIRE(::fsync(fd) == 0);
}

oc::compiler_session::completion compile_request(forge::asio::runtime& runtime, oc::compiler_session& session,
                                                 const oc::compile_request& request) {
   const auto identity = oc::identify(request); // test setup, never product admission
   auto copy = request;
   auto admitted = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
   BOOST_REQUIRE(admitted.status == oc::admission_status::accepted_unverified);
   BOOST_REQUIRE(admitted.completion.has_value());
   return await_ticket(runtime, std::move(*admitted.completion));
}

bool same_mask(const sigset_t& left, const sigset_t& right) {
   for (auto signal = 1; signal < NSIG; ++signal)
      if (sigismember(&left, signal) != sigismember(&right, signal))
         return false;
   return true;
}

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
void retained_successful_initialize(bool consume) {
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-startup-retained-success-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto waiter = std::optional<boost::asio::awaitable<void>>{};
   auto compiled = false;
   auto prepared = false;
   auto exact_identity = false;
   auto exact_ready = false;
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      waiter.emplace(session.initialize(oc::target_profile::native(), {})); // deliberately NEVER co_spawn here
      const auto deadline = clock_type::now() + std::chrono::seconds{2};
      auto consumer = std::optional<oc::compiler_session::ticket>{};
      for (auto attempt = unsigned{}; attempt < 2000 && clock_type::now() < deadline; ++attempt) {
         auto copy = request;
         auto admitted = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
         if (admitted.status == oc::admission_status::accepted_unverified) {
            BOOST_REQUIRE(admitted.completion.has_value());
            consumer.emplace(std::move(*admitted.completion));
            break;
         }
         // Test/main thread only, bounded caller retry; not a product wait/queue.
         BOOST_REQUIRE(admitted.status == oc::admission_status::not_initialized);
         BOOST_REQUIRE(!admitted.completion.has_value());
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
      }
      BOOST_REQUIRE(consumer.has_value());
      const auto completed = await_ticket(runtime, std::move(*consumer));
      compiled = completed.status == oc::compiler_session::completion_status::compiled;
      prepared = completed.code.is_prepared();
      exact_identity =
          completed.code.identity() == identity && completed.code.image().configuration == request.configuration;
      const auto ready = session.ready(identity, request.configuration);
      exact_ready = ready && ready->native_code().data() == completed.code.native_code().data();
   }); // successful shutdown + compute drain, then ALL old owners/storage destroyed
   BOOST_TEST(compiled);
   BOOST_TEST(prepared);
   BOOST_TEST(exact_identity);
   BOOST_TEST(exact_ready);
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 2U); // genuine HELLO + COMPILE
   BOOST_REQUIRE(waiter.has_value());
   if (consume) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto result = boost::asio::co_spawn(runtime.context(), std::move(*waiter), boost::asio::use_future);
      waiter.reset();
      BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(result.get());
   } else {
      waiter.reset(); // destruction alone after old io_context/services no longer exist
   }
}
#endif
} // namespace

BOOST_AUTO_TEST_SUITE(optimizing_session)

BOOST_AUTO_TEST_CASE(real_helper_canonical_completion_dedup_warm_memory_and_restart_disk) {
   if (!enter_fresh("optimizing_session/real_helper_canonical_completion_dedup_warm_memory_and_restart_disk"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-session-real-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
   auto gate = spawn_publication_gate{.helper = helper};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
#endif
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
      auto cold_request = request;
      auto cold = session.enqueue(identity, std::move(cold_request), forge::asio::task::priority{});
      BOOST_CHECK(cold.status == oc::admission_status::not_initialized);
      BOOST_TEST(!cold.completion.has_value());
      initialize(runtime, session);
      auto first_request = request, second_request = request;
      auto first = session.enqueue(identity, std::move(first_request), forge::asio::task::priority{});
      auto second = session.enqueue(identity, std::move(second_request), forge::asio::task::priority{});
      BOOST_REQUIRE(first.status == oc::admission_status::accepted_unverified);
      BOOST_REQUIRE(second.status == oc::admission_status::accepted_unverified);
      BOOST_REQUIRE(first.completion.has_value() && second.completion.has_value());
      const auto a = await_ticket(runtime, std::move(*first.completion));
      const auto b = await_ticket(runtime, std::move(*second.completion));
      BOOST_CHECK(a.status == oc::compiler_session::completion_status::compiled);
      BOOST_REQUIRE(a.code.is_prepared() && b.code.is_prepared());
      BOOST_TEST(a.code.identity() == identity);
      BOOST_TEST(b.code.identity() == identity);
      BOOST_CHECK(a.code.image().configuration == request.configuration);
      BOOST_TEST(a.code.native_code().data() == b.code.native_code().data());
      BOOST_CHECK(a.queued_time == b.queued_time);
      BOOST_CHECK(a.completed_time == b.completed_time);
      BOOST_CHECK(a.queued_time <= a.completed_time);
      const auto ready = session.ready(identity, request.configuration);
      BOOST_REQUIRE(ready.has_value());
      BOOST_TEST(ready->native_code().data() == a.code.native_code().data());
      auto mismatched = request.configuration;
      ++mismatched.execution.max_call_depth;
      BOOST_TEST(!session.ready(identity, mismatched).has_value());
      BOOST_TEST(std::filesystem::is_regular_file(std::filesystem::path{directory} / (identity.str() + ".foc")));
   });
#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 2U); // one HELLO + one deduplicated COMPILE
#endif
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      // SAME process session reconstruction; fresh-exec/count proof is separate.
      initialize(runtime, session);
      const auto code = compile_request(runtime, session, request);
      BOOST_CHECK(code.status == oc::compiler_session::completion_status::loaded_from_disk);
      BOOST_TEST(code.code.is_prepared());
      BOOST_TEST(code.code.identity() == identity);
      const auto ready = session.ready(identity, request.configuration);
      BOOST_REQUIRE(ready.has_value());
      BOOST_TEST(ready->native_code().data() == code.code.native_code().data());
   });
#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 3U); // reconstruction adds HELLO, no COMPILE
#endif
}

#if defined(__APPLE__)
BOOST_AUTO_TEST_CASE(darwin_sessions_share_native_sigchld_registration_across_runtimes) {
   if (!enter_fresh("optimizing_session/darwin_sessions_share_native_sigchld_registration_across_runtimes",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   auto second = request, third = request;
   --second.configuration.execution.max_call_depth;
   third.configuration.execution.max_call_depth -= 2;
   const auto identity = oc::identify(request), second_identity = oc::identify(second),
              third_identity = oc::identify(third);
   auto first_directory = std::string{"/tmp/forge-oc-signals-a-XXXXXX"};
   auto second_directory = std::string{"/tmp/forge-oc-signals-b-XXXXXX"};
   auto failed_directory = std::string{"/tmp/forge-oc-signals-failed-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(first_directory.data()) != nullptr);
   const auto remove_first = boost::scope::scope_exit{[&] {
      ::unlink((std::filesystem::path{first_directory} / (second_identity.str() + ".foc")).c_str());
      remove_owned_cache(first_directory, identity);
   }};
   BOOST_REQUIRE(::mkdtemp(second_directory.data()) != nullptr);
   const auto remove_second = boost::scope::scope_exit{[&] {
      ::unlink((std::filesystem::path{second_directory} / (second_identity.str() + ".foc")).c_str());
      ::unlink((std::filesystem::path{second_directory} / (third_identity.str() + ".foc")).c_str());
      remove_owned_cache(second_directory, identity);
   }};
   BOOST_REQUIRE(::mkdtemp(failed_directory.data()) != nullptr);
   const auto remove_failed = boost::scope::scope_exit{[&] { remove_owned_cache(failed_directory, identity); }};
   struct sigaction before{};
   BOOST_REQUIRE(::sigaction(SIGCHLD, nullptr, &before) == 0);
   BOOST_REQUIRE((before.sa_flags & SA_SIGINFO) == 0);
   BOOST_REQUIRE(before.sa_handler == SIG_DFL);
   BOOST_REQUIRE((before.sa_flags & SA_NOCLDWAIT) == 0);

   namespace vm = forge::vm::wasm::interpret;
   using registry = vm::registered_host_functions<vm::standalone_function_t>;
   auto execution_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{execution_runtime};
   auto executor = oc::executor{scheduler, {4 * 1024 * 1024 + 256 * 1024, 64 * 1024}};
   auto thread_prepared = false;
   auto failure = std::exception_ptr{};
   const char* last_call = "initialization";
   const auto invoke_native = [&](const oc::compiler_session::completion& completed) {
      BOOST_REQUIRE(completed.code.is_prepared());
      auto binding = oc::executor::bind<registry>(completed.code);
      auto memory = vm::wasm_allocator{};
      const auto arguments =
          std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{std::uint32_t{7}}, vm::i32_const_t{std::uint32_t{9}}};
      const auto result =
          executor.invoke(binding, nullptr, 0, arguments, memory, clock_type::now() + std::chrono::seconds{1});
      BOOST_CHECK(result.trap == oc::trap_reason::none);
      BOOST_TEST(!result.host_error);
      BOOST_REQUIRE(result.value.has_value());
      BOOST_REQUIRE(result.value->is_a<vm::i32_const_t>());
      BOOST_TEST(result.value->to_ui32() == 16U);
   };
   try {
      forge::asio::blocking::run(execution_runtime, executor.start(clock_type::now() + std::chrono::seconds{1}));
      executor.prepare_thread();
      thread_prepared = true;
      exercise_session(options(first_directory, helper), [&](auto& first, auto& first_runtime, auto&) {
         exercise_session(options(second_directory, helper), [&](auto& second_session, auto& second_runtime, auto&) {
            // initialize is eager: both jobs are admitted on DISTINCT runtimes/
            // compute pools before either wait is consumed. No new worker API.
            auto first_wait = first.initialize(oc::target_profile::native(), {});
            auto second_wait = second_session.initialize(oc::target_profile::native(), {});
            auto first_result =
                boost::asio::co_spawn(first_runtime.context(), std::move(first_wait), boost::asio::use_future);
            auto second_result =
                boost::asio::co_spawn(second_runtime.context(), std::move(second_wait), boost::asio::use_future);
            BOOST_REQUIRE(first_result.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
            first_result.get();
            BOOST_REQUIRE(second_result.wait_for(std::chrono::seconds{3}) == std::future_status::ready);
            second_result.get();
            last_call = "A/cold";
            const auto first_code = compile_request(first_runtime, first, request);
            last_call = "B/cold";
            const auto second_code = compile_request(second_runtime, second_session, request);
            BOOST_REQUIRE(first_code.status == oc::compiler_session::completion_status::compiled);
            BOOST_REQUIRE(second_code.status == oc::compiler_session::completion_status::compiled);
            invoke_native(first_code);
            invoke_native(second_code);
            struct sigaction owned{};
            BOOST_REQUIRE(::sigaction(SIGCHLD, nullptr, &owned) == 0);
            BOOST_REQUIRE((owned.sa_flags & SA_SIGINFO) == 0);
            BOOST_REQUIRE(owned.sa_handler != SIG_DFL);
            shutdown(first_runtime, first);
            struct sigaction after_first{};
            BOOST_REQUIRE(::sigaction(SIGCHLD, nullptr, &after_first) == 0);
            BOOST_REQUIRE((after_first.sa_flags & SA_SIGINFO) == 0);
            BOOST_CHECK(after_first.sa_handler == owned.sa_handler);
            BOOST_TEST(after_first.sa_flags == owned.sa_flags);
            BOOST_TEST(same_mask(after_first.sa_mask, owned.sa_mask));
            last_call = "B/after-A-shutdown";
            const auto still_live = compile_request(second_runtime, second_session, second);
            BOOST_REQUIRE(still_live.status == oc::compiler_session::completion_status::compiled);
            invoke_native(still_live); // Real new helper after the other session has drained.

            exercise_session(options(failed_directory, "/nonexistent/forge-oc-helper"),
                             [&](auto& failed, auto& failed_runtime, auto&) {
                                BOOST_CHECK_THROW(initialize(failed_runtime, failed),
                                                  oc::exceptions::compiler_unavailable);
                             }); // Explicit normal fixture shutdown releases committed failed-startup ownership.
            last_call = "B/after-failed-init";
            const auto after_unwind = compile_request(second_runtime, second_session, third);
            BOOST_REQUIRE(after_unwind.status == oc::compiler_session::completion_status::compiled);
            invoke_native(after_unwind);

            exercise_session(options(first_directory, helper), [&](auto& restarted, auto& restarted_runtime, auto&) {
               initialize(restarted_runtime, restarted); // New owner while the second session is still live.
               last_call = "A-restart/disk-hit-expected";
               const auto restored = compile_request(restarted_runtime, restarted, request);
               BOOST_REQUIRE(restored.status == oc::compiler_session::completion_status::loaded_from_disk);
               invoke_native(restored);
               shutdown(second_runtime, second_session);
               last_call = "A-restart/after-B-shutdown";
               const auto last_live = compile_request(restarted_runtime, restarted, second);
               BOOST_REQUIRE(last_live.status == oc::compiler_session::completion_status::compiled);
               invoke_native(last_live); // SAME last owner can still launch/reap a real helper.
            });
         });
      });
      struct sigaction after{};
      BOOST_REQUIRE(::sigaction(SIGCHLD, nullptr, &after) == 0);
      BOOST_REQUIRE((after.sa_flags & SA_SIGINFO) == 0);
      BOOST_CHECK(after.sa_handler == SIG_DFL);
      BOOST_TEST((after.sa_flags & SA_NOCLDWAIT) == 0);
   } catch (...) {
      failure = std::current_exception();
   }
   // Same native owner/drain pattern as the existing cache-pin control. Every
   // session has already drained before this executor/runtime can be retired.
   if (thread_prepared) {
      try {
         executor.release_thread();
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
      }
   }
   try {
      forge::asio::blocking::run(execution_runtime, executor.shutdown());
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
   }
   try {
      forge::asio::blocking::run(execution_runtime, scheduler.shutdown());
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
   }
   if (failure) {
      BOOST_TEST_CHECKPOINT("darwin_sigchld last_call=" << last_call);
      std::rethrow_exception(failure);
   }
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(real_helper_deadline_requires_confirmed_owned_reap_before_destruction) {
   if (!enter_fresh("optimizing_session/real_helper_deadline_requires_confirmed_owned_reap_before_destruction",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-session-deadline-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper};
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto terminal = std::exception_ptr{};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session); // genuine HELLO is never stopped/overridden
      gate.stop_next.store(true, std::memory_order_release);
      active_spawn_gate.store(&gate, std::memory_order_release);
      // REAL child is stopped before spawn returns/parent writes. Only the
      // existing production I/O deadline sends SIGKILL and performs sole reap.
      try {
         static_cast<void>(compile_request(runtime, session, request));
      } catch (...) {
         terminal = std::current_exception(); // inspect only after both drains/ECHILD below
      }
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
   });
   // Observe only after both mandatory owner drains; no observer completes reap.
   const auto pid = gate.stopped_pid.load(std::memory_order_acquire);
   auto status = int{};
   auto waited = pid_t{-2};
   auto observed_error = int{};
   if (pid > 0) {
      do {
         errno = 0;
         waited = ::waitpid(pid, &status, WNOHANG);
         observed_error = errno;
      } while (waited < 0 && observed_error == EINTR);
   }
   BOOST_REQUIRE(pid > 0);
   BOOST_TEST(gate.stop_result.load(std::memory_order_acquire) == 0);
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(gate.overrides.load(std::memory_order_acquire) == 0U);
   BOOST_TEST(waited == -1);
   BOOST_TEST(observed_error == ECHILD);
   // Owned helper I/O timeout, not LLVM CPU/allocator-limit proof.
   BOOST_REQUIRE((waited == -1 && observed_error == ECHILD)); // no context inspection without the reap oracle
   BOOST_REQUIRE((terminal != nullptr));
   try {
      std::rethrow_exception(terminal);
   } catch (const oc::exceptions::compile_limit& error) {
      BOOST_TEST((typeid(error) == typeid(oc::exceptions::compile_limit)));
      BOOST_TEST((error.code() == forge::exceptions::make_error_code(oc::exceptions::code::compile_limit)));
      const forge::exceptions::frame* snapshot = nullptr;
      auto snapshots = std::size_t{};
      for (const auto& frame : error.context_frames()) {
         if (frame.message == "compiler helper deadline snapshot") {
            snapshot = &frame;
            ++snapshots;
         }
      }
      BOOST_REQUIRE_EQUAL(snapshots, 1U);
      BOOST_REQUIRE(snapshot != nullptr);
      constexpr std::array<std::string_view, 9> keys{
          "oc_helper_exchange", "oc_helper_phase", "oc_helper_arm_ms",      "oc_helper_elapsed_ms",  "oc_helper_io",
          "oc_helper_reap",     "oc_helper_owned", "oc_helper_socket_peek", "oc_helper_socket_errno"};
      BOOST_REQUIRE_EQUAL(snapshot->context.size(), keys.size());
      for (std::size_t index = 0; index < keys.size(); ++index) {
         // Boolean predicates prevent Boost.Test from dumping arbitrary context.
         BOOST_REQUIRE((snapshot->context[index].key == keys[index]));
         BOOST_REQUIRE(!snapshot->context[index].redacted);
      }
      BOOST_TEST((snapshot->context[0].value == "compile"));
      constexpr std::array<std::string_view, 9> phases{"before_io",  "request_write", "hello_header",
                                                       "hello_body", "ack_write",     "reply_header",
                                                       "reply_body", "output_eof",    "await_reap"};
      auto known_phase = false;
      for (const auto phase : phases)
         known_phase = known_phase || snapshot->context[1].value == phase;
      BOOST_TEST(known_phase); // socket buffering may change the precise parent IO phase
      auto times = std::array<std::int64_t, 2>{-1, -1};
      for (std::size_t index = 0; index < times.size(); ++index) {
         const auto& text = snapshot->context[index + 2].value;
         BOOST_REQUIRE((!text.empty() && text.size() <= 20));
         const auto result = std::from_chars(text.data(), text.data() + text.size(), times[index]);
         BOOST_REQUIRE((result.ec == std::errc{} && result.ptr == text.data() + text.size() && times[index] >= 0));
      }
      BOOST_TEST(times[0] <= times[1]);
      BOOST_TEST(times[1] >= 2000); // unchanged options(): original helper deadline is 2s
      for (std::size_t index = 4; index < 7U; ++index)
         BOOST_TEST((snapshot->context[index].value == "true" || snapshot->context[index].value == "false"));
      // The SAME stopped helper still owns its open endpoint, with no reply.
      // Extend this existing deadline/reap control; no new signal scenario.
      BOOST_TEST((snapshot->context[7].value == "would_block"));
      const auto& native_error = snapshot->context[8].value;
      auto error_number = int{};
      const auto parsed = std::from_chars(native_error.data(), native_error.data() + native_error.size(), error_number);
      BOOST_REQUIRE((parsed.ec == std::errc{} && parsed.ptr == native_error.data() + native_error.size()));
      BOOST_TEST((error_number == EAGAIN || error_number == EWOULDBLOCK));
   }
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(real_late_spawn_with_expired_shutdown_requires_publication_and_sole_reap) {
   if (fresh_process::mode() == nullptr) {
      const auto child = fresh_process::run(
          "optimizing_session/real_late_spawn_with_expired_shutdown_requires_publication_and_sole_reap",
          "publication-late");
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_TEST(WEXITSTATUS(child.status) == 0);
      return;
   }
   fresh_process::disable_core_dumps();
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-session-publication-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper, .delay = true};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{
       forge::asio::compute::pool::options{.worker_threads = 1, .max_pending_tasks = 4, .max_waiting_submissions = 4}};
   auto entered = false;
   auto prematurely_completed = false;
   auto live_before_release = false;
   auto cancelled = false;
   auto observed_reap = pid_t{-2};
   auto observed_error = int{};
   auto unexpected = std::exception_ptr{};
   {
      auto session = oc::compiler_session{runtime, compute.get_executor(), options(directory, helper)};
      initialize(runtime, session);
      auto stopped = std::optional<std::future<void>>{};
      auto completion = std::optional<std::future<std::optional<oc::compiler_session::completion>>>{};
      // Gate lifetime extends through BOTH mandatory session and compute drains.
      // Any assertion/exception releases the real spawn before cleanup retry.
      const auto release_gate = boost::scope::scope_exit{[&] {
         active_spawn_gate.store(nullptr, std::memory_order_release);
         gate.release();
      }};
      try {
         active_spawn_gate.store(&gate, std::memory_order_release);
         auto admitted = session.enqueue(identity, std::move(request), forge::asio::task::priority{});
         BOOST_REQUIRE(admitted.status == oc::admission_status::accepted_unverified);
         BOOST_REQUIRE(admitted.completion.has_value());
         completion.emplace(boost::asio::co_spawn(
             runtime.context(),
             collect_completion(std::move(*admitted.completion).wait(clock_type::now() + std::chrono::seconds{3}, {})),
             boost::asio::use_future));
         entered = gate.wait_for_entry();
         if (entered) {
            stopped.emplace(boost::asio::co_spawn(runtime.context(),
                                                  session.shutdown(clock_type::now() - std::chrono::milliseconds{1}),
                                                  boost::asio::use_future));
            prematurely_completed = stopped->wait_for(std::chrono::milliseconds{25}) == std::future_status::ready;
            live_before_release = ::kill(gate.pid, 0) == 0;
         }
      } catch (...) {
         unexpected = std::current_exception();
      }
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
      // ALWAYS cleanup before reporting any captured failure. The existing
      // shutdown contract, not a second wait/kill/reaper, owns the real child.
      try {
         if (stopped) {
            BOOST_REQUIRE(stopped->wait_for(std::chrono::seconds{2}) == std::future_status::ready);
            stopped->get();
         } else {
            shutdown(runtime, session);
         }
      } catch (...) {
         if (!unexpected)
            unexpected = std::current_exception();
         shutdown(runtime, session); // explicit retry while injected owners remain live
      }
      if (completion) {
         BOOST_REQUIRE(completion->wait_for(std::chrono::seconds{1}) == std::future_status::ready);
         try {
            static_cast<void>(completion->get());
         } catch (const oc::exceptions::cancelled&) {
            cancelled = true;
         } catch (...) {
            if (!unexpected)
               unexpected = std::current_exception();
         }
      }
      if (entered) {
         auto status = int{};
         do {
            errno = 0;
            observed_reap = ::waitpid(gate.pid, &status, WNOHANG);
            observed_error = errno;
         } while (observed_reap < 0 && observed_error == EINTR);
         // Observer only AFTER claimed cleanup; any other result is FAILURE,
         // never accepted as this observer completing the production reap.
      }
   }
   auto drained = boost::asio::co_spawn(runtime.context(), compute.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   drained.get();
   // The shared fresh-exec helper intentionally uses the ordinary child log
   // level. Emit only these public control values, never request/payload bytes.
   std::printf("late_spawn pid=%ld entered=%u gate_timed_out=%u shutdown_before_release_ready=%u "
               "live_before_release=%u cancelled=%u waitpid_after_claimed_cleanup=%ld errno=%d\n",
               static_cast<long>(gate.pid), static_cast<unsigned>(entered), static_cast<unsigned>(gate.timed_out),
               static_cast<unsigned>(prematurely_completed), static_cast<unsigned>(live_before_release),
               static_cast<unsigned>(cancelled), static_cast<long>(observed_reap), observed_error);
   if (unexpected)
      std::rethrow_exception(unexpected);
   BOOST_REQUIRE(entered);
   BOOST_REQUIRE(!gate.timed_out);
   BOOST_TEST(!prematurely_completed);
   BOOST_TEST(live_before_release);
   BOOST_TEST(cancelled);
   BOOST_TEST(observed_reap == -1);
   BOOST_TEST(observed_error == ECHILD);
   // Neighboring deterministic timeline: after real OS success/before return.
   // It is NOT an instruction-level reproduction of the former false-before-PID
   // window. The SAME owner release/acquire ordering closes that source race.
}
#endif

#if defined(FORGE_OC_HELPER_RESOURCE_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(real_helper_explicit_absolute_as_budget_reports_outcome_after_owned_cleanup) {
   if (fresh_process::mode() == nullptr) {
      const auto child = fresh_process::run(
          "optimizing_session/real_helper_explicit_absolute_as_budget_reports_outcome_after_owned_cleanup",
          "session-resource");
      BOOST_REQUIRE(child.error == 0);
      BOOST_REQUIRE(!child.timed_out);
      BOOST_REQUIRE(WIFEXITED(child.status));
      BOOST_TEST(WEXITSTATUS(child.status) == 0);
      return;
   }
   fresh_process::disable_core_dumps();
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   const auto* expectation = std::getenv("FORGE_OC_HELPER_TEST_EXPECT_SUCCESS");
   BOOST_REQUIRE(helper != nullptr && expectation != nullptr);
   const auto expected = std::string_view{expectation};
   BOOST_REQUIRE(expected == "0" || expected == "1");
   const auto expect_success = expected == "1";
   auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto configuration = request.configuration;
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-session-resource-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] {
      const auto path = std::filesystem::path{directory} / (identity.str() + ".foc");
      ::unlink(path.c_str()); // Only this exact test-owned canonical output.
      ::unlink((std::filesystem::path{directory} / "owner.lock").c_str());
      ::rmdir(directory.c_str());
   }};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto compute = forge::asio::compute::pool{
       forge::asio::compute::pool::options{.worker_threads = 1, .max_pending_tasks = 4, .max_waiting_submissions = 4}};
   auto code = std::optional<oc::compiler_session::completion>{};
   auto outcome = std::exception_ptr{};
   auto cleanup_error = std::exception_ptr{};
   auto ready = false;
   auto address_space_bytes = std::uint64_t{};
   {
      auto configuration_options = options(directory, helper);
      address_space_bytes = configuration_options.helper_limits.address_space_bytes;
      auto session = oc::compiler_session{runtime, compute.get_executor(), std::move(configuration_options)};
      try {
         initialize(runtime, session);
         auto admitted = session.enqueue(identity, std::move(request), forge::asio::task::priority{});
         BOOST_REQUIRE(admitted.status == oc::admission_status::accepted_unverified);
         BOOST_REQUIRE(admitted.completion.has_value());
         code.emplace(await_ticket(runtime, std::move(*admitted.completion)));
      } catch (...) {
         outcome = std::current_exception(); // Record actual typed failure, not a success-only test.
      }
      // ALWAYS drain the actual child and actor before reporting allocation or
      // protocol failure. No synthetic helper/mmap mode or second live reaper.
      ready = session.ready(identity, configuration).has_value(); // observe before shutdown closes ready
      try {
         shutdown(runtime, session);
      } catch (...) {
         cleanup_error = std::current_exception();
         shutdown(runtime, session); // Explicit retry with both injected owners alive.
      }
   }
   auto drained = boost::asio::co_spawn(runtime.context(), compute.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(drained.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   drained.get();
   auto observed_code = int{-1};
   auto compiler_family = false;
   auto operational_failure = false;
   if (outcome) {
      try {
         std::rethrow_exception(outcome);
      } catch (const forge::exceptions::base& error) {
         observed_code = error.code().value();
         compiler_family =
             error.code().category() == oc::exceptions::forge_exceptions_category(oc::exceptions::code::invalid_module);
         operational_failure =
             compiler_family && (observed_code == static_cast<int>(oc::exceptions::code::compile_limit) ||
                                 observed_code == static_cast<int>(oc::exceptions::code::compiler_unavailable));
      } catch (...) {
         // Unknown failure is reported as such and FAILS the assertions below.
      }
   }
   std::printf("helper_as requested=%llu expect_success=%u completed=%u ready=%u "
               "compiler_family=%u observed_code=%d cleanup_succeeded=%u\n",
               static_cast<unsigned long long>(address_space_bytes), static_cast<unsigned>(expect_success),
               static_cast<unsigned>(code.has_value()), static_cast<unsigned>(ready),
               static_cast<unsigned>(compiler_family), observed_code, static_cast<unsigned>(!cleanup_error));
   if (cleanup_error)
      std::rethrow_exception(cleanup_error);
   if (expect_success) {
      BOOST_REQUIRE(!outcome);
      BOOST_REQUIRE(code.has_value());
      BOOST_REQUIRE(code->code.is_prepared());
      BOOST_TEST(code->code.identity() == identity);
      BOOST_CHECK(code->code.image().configuration == configuration);
      BOOST_CHECK(code->status == oc::compiler_session::completion_status::compiled);
      BOOST_TEST(ready);
   } else {
      BOOST_REQUIRE(static_cast<bool>(outcome));
      BOOST_TEST(!code.has_value());
      BOOST_TEST(!ready);
      BOOST_TEST(operational_failure); // invalid_module due solely to cap would be a BUG, never PASS.
   }
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(optimizing_session_early_exit_during_large_write_preserves_parent_SIGPIPE) {
   if (!enter_fresh("optimizing_session/optimizing_session_early_exit_during_large_write_preserves_parent_SIGPIPE"))
      return;
   struct sigaction configured{};
   configured.sa_handler = SIG_DFL;
   sigemptyset(&configured.sa_mask);
   BOOST_REQUIRE(::sigaction(SIGPIPE, &configured, nullptr) == 0);
   auto pipe_mask = sigset_t{};
   sigemptyset(&pipe_mask);
   sigaddset(&pipe_mask, SIGPIPE);
   BOOST_REQUIRE(::sigprocmask(SIG_UNBLOCK, &pipe_mask, nullptr) == 0);
   struct sigaction before{};
   auto mask_before = sigset_t{};
   BOOST_REQUIRE(::sigaction(SIGPIPE, nullptr, &before) == 0);
   BOOST_REQUIRE(::sigprocmask(SIG_SETMASK, nullptr, &mask_before) == 0);
   BOOST_REQUIRE(before.sa_handler == SIG_DFL);
   BOOST_REQUIRE(sigismember(&mask_before, SIGPIPE) == 0);
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   auto directory = std::string{"/tmp/forge-oc-session-socket-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   auto request = oc::compile_request{};
   request.configuration.target = oc::target_profile::native();
   request.configuration.limits = {1024U * 1024U, 16384};
   request.wasm.resize(8U * 1024U * 1024U, 0x5a);
   const auto identity = oc::identify(request);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto configuration = options(directory, helper);
   configuration.max_request_bytes = 16U * 1024U * 1024U;
   exercise_session(std::move(configuration), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session); // actual HELLO/build/target/limits, not /usr/bin/false
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
      BOOST_TEST(gate.overrides.load(std::memory_order_acquire) == 0U);
      gate.exit_next.store(true, std::memory_order_release);
      // Exactly this compiler launch executes the REAL OS /usr/bin/false.
      // No parser/LLVM is entered; production performs large socket write/reap.
      BOOST_CHECK_THROW(compile_request(runtime, session, request), oc::exceptions::compiler_unavailable);
   });
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 2U);
   BOOST_TEST(gate.overrides.load(std::memory_order_acquire) == 1U);
   const auto pid = gate.latest_pid.load(std::memory_order_acquire);
   auto status = int{};
   auto waited = pid_t{-2};
   auto observed_error = int{};
   if (pid > 0) {
      do {
         errno = 0;
         waited = ::waitpid(pid, &status, WNOHANG);
         observed_error = errno;
      } while (waited < 0 && observed_error == EINTR);
   }
   BOOST_REQUIRE(pid > 0);
   BOOST_TEST(waited == -1);
   BOOST_TEST(observed_error == ECHILD); // after cleanup only, never a second live reaper
   struct sigaction after{};
   auto mask_after = sigset_t{};
   BOOST_REQUIRE(::sigaction(SIGPIPE, nullptr, &after) == 0);
   BOOST_REQUIRE(::sigprocmask(SIG_SETMASK, nullptr, &mask_after) == 0);
   BOOST_TEST(after.sa_handler == before.sa_handler);
   BOOST_TEST(after.sa_flags == before.sa_flags);
   BOOST_TEST(same_mask(after.sa_mask, before.sa_mask));
   BOOST_TEST(same_mask(mask_after, mask_before));
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(startup_target_repetition_cancellation_and_failure_keep_admission_closed) {
   if (!enter_fresh("optimizing_session/startup_target_repetition_cancellation_and_failure_keep_admission_closed"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   const auto foreign = oc::compile_request::decode(read_fixture(".request", false), 1024U * 1024U);
   BOOST_TEST(oc::target_profile::native().matches_host());
   BOOST_TEST(!foreign.configuration.target.matches_host());
   auto directory = std::string{"/tmp/forge-oc-startup-target-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      auto waiter = session.initialize(foreign.configuration.target, {});
      BOOST_CHECK_THROW(static_cast<void>(session.initialize(oc::target_profile::native(), {})),
                        forge::asio::exceptions::invalid_state);
      auto result = boost::asio::co_spawn(runtime.context(), std::move(waiter), boost::asio::use_future);
      BOOST_REQUIRE(result.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      BOOST_CHECK_THROW(result.get(), oc::exceptions::unsupported_target);
      BOOST_TEST(!std::filesystem::exists(std::filesystem::path{directory} / "owner.lock"));
      auto copy = request;
      const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
      BOOST_CHECK(rejected.status == oc::admission_status::not_initialized);
      BOOST_TEST(!rejected.completion.has_value());
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
      BOOST_CHECK_THROW(static_cast<void>(session.initialize(oc::target_profile::native(), {})),
                        forge::asio::exceptions::invalid_state);
   });
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      auto stop = std::stop_source{};
      stop.request_stop();
      BOOST_CHECK_THROW(initialize(runtime, session, stop.get_token()), oc::exceptions::cancelled);
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
   });
   exercise_session(options(directory, "/nonexistent/forge-oc-helper"), [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::compiler_unavailable);
      auto copy = request;
      const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
      BOOST_CHECK(rejected.status == oc::admission_status::not_initialized);
      BOOST_TEST(!rejected.completion.has_value());
   });
}

BOOST_AUTO_TEST_CASE(startup_eager_discarded_waiter_and_compute_stop_drain_late_spawn) {
   if (!enter_fresh("optimizing_session/startup_eager_discarded_waiter_and_compute_stop_drain_late_spawn"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-startup-stop-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper, .delay = true};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto entered = false;
   auto prematurely_completed = false;
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto& compute) {
      const auto release_before_cleanup = boost::scope::scope_exit{[&] { gate.release(); }};
      // Dropping the waiter never drops its eagerly accepted control record.
      static_cast<void>(session.initialize(oc::target_profile::native(), {}));
      entered = gate.wait_for_entry(); // real successful OS child, publication delayed
      compute.request_stop();          // no new compute admission, owners/runtime remain alive
      auto stopped =
          boost::asio::co_spawn(runtime.context(), session.shutdown(clock_type::now() - std::chrono::milliseconds{1}),
                                boost::asio::use_future);
      prematurely_completed = stopped.wait_for(std::chrono::milliseconds{25}) == std::future_status::ready;
      gate.release(); // EVERY failure/exception also releases through the scope guard below
      BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
      stopped.get();
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
   });
   BOOST_REQUIRE(entered);
   BOOST_TEST(!gate.timed_out);
   BOOST_TEST(!prematurely_completed);
   const auto pid = gate.latest_pid.load(std::memory_order_acquire);
   auto waited = pid_t{-2};
   auto observed_error = int{};
   if (pid > 0) {
      auto status = int{};
      do {
         errno = 0;
         waited = ::waitpid(pid, &status, WNOHANG);
         observed_error = errno;
      } while (waited < 0 && observed_error == EINTR);
   }
   BOOST_REQUIRE(pid > 0);
   BOOST_TEST(waited == -1);
   BOOST_TEST(observed_error == ECHILD);
   BOOST_TEST(!std::filesystem::exists(std::filesystem::path{directory} / "owner.lock"));
}

BOOST_AUTO_TEST_CASE(cache_owner_lock_is_stable_and_released_after_stopped_compute_drain) {
   if (!enter_fresh("optimizing_session/cache_owner_lock_is_stable_and_released_after_stopped_compute_drain"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-cache-lock-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto lock_path = std::filesystem::path{directory} / "owner.lock";
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto pinned = std::optional<oc::artifact>{};
   auto saved_bytes = std::vector<std::byte>{};
   struct stat original{};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto& compute) {
      initialize(runtime, session);
      BOOST_REQUIRE(::lstat(lock_path.c_str(), &original) == 0);
      const auto other = ::open(lock_path.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC);
      BOOST_REQUIRE(other >= 0);
      const auto close_other = boost::scope::scope_exit{[other] { ::close(other); }};
      BOOST_TEST(::flock(other, LOCK_EX | LOCK_NB) == -1);
      BOOST_TEST((errno == EWOULDBLOCK || errno == EAGAIN));
      auto completed = compile_request(runtime, session, request);
      pinned.emplace(completed.code);
      saved_bytes.assign(pinned->native_code().begin(), pinned->native_code().end());
      struct stat after_publish{};
      BOOST_REQUIRE(::lstat(lock_path.c_str(), &after_publish) == 0);
      BOOST_TEST(original.st_dev == after_publish.st_dev);
      BOOST_TEST(original.st_ino == after_publish.st_ino);
      compute.request_stop();
      shutdown(runtime, session); // cleanup requires NO new compute work
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
      BOOST_REQUIRE(::flock(other, LOCK_EX | LOCK_NB) == 0);
      BOOST_REQUIRE(::flock(other, LOCK_UN) == 0);
   });
   BOOST_REQUIRE(pinned.has_value());
   BOOST_TEST((std::vector<std::byte>(pinned->native_code().begin(), pinned->native_code().end()) == saved_bytes));
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      struct stat restarted{};
      BOOST_REQUIRE(::lstat(lock_path.c_str(), &restarted) == 0);
      BOOST_TEST(original.st_dev == restarted.st_dev);
      BOOST_TEST(original.st_ino == restarted.st_ino);
   });
}

BOOST_AUTO_TEST_CASE(cache_unknown_bytes_entries_and_nonregulars_fail_before_deletion) {
   if (!enter_fresh("optimizing_session/cache_unknown_bytes_entries_and_nonregulars_fail_before_deletion"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-cache-unknown-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto opaque = std::filesystem::path{directory} / "crash-orphan.tmp";
   const auto other = std::filesystem::path{directory} / "unrecognized";
   const auto link = std::filesystem::path{directory} / "opaque-symlink";
   const auto remove_cache = boost::scope::scope_exit{[&] {
      ::unlink(opaque.c_str());
      ::unlink(other.c_str());
      ::unlink(link.c_str());
      remove_owned_cache(directory, identity);
   }};
   const auto bytes = std::vector<std::uint8_t>(32, 0x5a);
   write_owned_file(opaque, bytes);
   auto configuration = options(directory, helper);
   configuration.max_disk_bytes = 31;
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::compile_limit);
      BOOST_TEST(std::filesystem::file_size(opaque) == bytes.size());
   });
   write_owned_file(other, {});
   configuration = options(directory, helper);
   configuration.max_disk_entries = 1;
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::compile_limit);
      BOOST_TEST((std::filesystem::exists(opaque) && std::filesystem::exists(other)));
   });
   BOOST_REQUIRE(::symlink("crash-orphan.tmp", link.c_str()) == 0);
   configuration = options(directory, helper);
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::cache_io);
      BOOST_TEST(std::filesystem::is_symlink(link));
      BOOST_TEST(std::filesystem::file_size(opaque) == bytes.size());
   });
}

BOOST_AUTO_TEST_CASE(cache_corruption_rebuild_and_old_plus_temp_quota_are_exact) {
   if (!enter_fresh("optimizing_session/cache_corruption_rebuild_and_old_plus_temp_quota_are_exact",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto corrupt = read_fixture(".foc");
   BOOST_REQUIRE(!corrupt.empty());
   corrupt.back() ^= 1; // real checksum corruption, no fabricated artifact metadata
   auto directory = std::string{"/tmp/forge-oc-cache-corrupt-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto file = std::filesystem::path{directory} / (identity.str() + ".foc");
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   write_owned_file(file, corrupt);
   auto configuration = options(directory, helper);
   configuration.max_disk_bytes = corrupt.size();
   configuration.max_disk_entries = 2;
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      // Destination cannot be evicted to evade old+temp reservation. Failed
      // admission leaves the previous corrupt bytes charged/preserved, no ready.
      BOOST_CHECK_THROW(compile_request(runtime, session, request), oc::exceptions::compile_limit);
      BOOST_TEST(!session.ready(identity, request.configuration).has_value());
      auto input = std::ifstream{file, std::ios::binary};
      const auto retained = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
      BOOST_TEST(retained == corrupt);
   });
   configuration.max_disk_bytes = 2 * corrupt.size();
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      const auto completed = compile_request(runtime, session, request);
      BOOST_CHECK(completed.status == oc::compiler_session::completion_status::compiled);
      BOOST_TEST(completed.code.is_prepared());
      auto input = std::ifstream{file, std::ios::binary};
      const auto bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
      const auto code = oc::artifact::decode(bytes, identity, configuration.max_response_bytes);
      BOOST_TEST(code.identity() == identity);
      BOOST_TEST(std::filesystem::file_size(file) <= configuration.max_disk_bytes);
   });
}

BOOST_AUTO_TEST_CASE(cache_eviction_does_not_invalidate_external_prepared_mapping) {
   if (!enter_fresh("optimizing_session/cache_eviction_does_not_invalidate_external_prepared_mapping",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   auto second = request;
   auto third = request;
   --second.configuration.execution.max_call_depth;
   third.configuration.execution.max_call_depth -= 2;
   const auto a = oc::identify(request), b = oc::identify(second), c = oc::identify(third);
   auto directory = std::string{"/tmp/forge-oc-cache-evict-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] {
      ::unlink((std::filesystem::path{directory} / (b.str() + ".foc")).c_str());
      ::unlink((std::filesystem::path{directory} / (c.str() + ".foc")).c_str());
      remove_owned_cache(directory, a);
   }};
   auto configuration = options(directory, helper);
   configuration.max_disk_entries = 2;
   namespace vm = forge::vm::wasm::interpret;
   using registry = vm::registered_host_functions<vm::standalone_function_t>;
   auto execution_runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{execution_runtime};
   auto executor = oc::executor{scheduler, {4 * 1024 * 1024 + 256 * 1024, 64 * 1024}};
   auto binding = std::optional<oc::executor::binding<registry>>{};
   auto thread_prepared = false;
   auto failure = std::exception_ptr{};
   const auto invoke_pinned = [&] {
      BOOST_REQUIRE(binding.has_value());
      auto memory = vm::wasm_allocator{};
      const auto arguments =
          std::array<vm::operand_stack_elem, 2>{vm::i32_const_t{std::uint32_t{7}}, vm::i32_const_t{std::uint32_t{9}}};
      const auto result =
          executor.invoke(*binding, nullptr, 0, arguments, memory, clock_type::now() + std::chrono::seconds{1});
      BOOST_CHECK(result.trap == oc::trap_reason::none);
      BOOST_TEST(!result.host_error);
      BOOST_REQUIRE(result.value.has_value());
      BOOST_REQUIRE(result.value->is_a<vm::i32_const_t>());
      BOOST_TEST(result.value->to_ui32() == 16U);
   };
   try {
      forge::asio::blocking::run(execution_runtime, executor.start(clock_type::now() + std::chrono::seconds{1}));
      executor.prepare_thread();
      thread_prepared = true;
      auto pinned = std::optional<oc::artifact>{};
      auto native = std::vector<std::byte>{};
      exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
         initialize(runtime, session);
         const auto first = compile_request(runtime, session, request);
         pinned.emplace(first.code);
         native.assign(pinned->native_code().begin(), pinned->native_code().end());
         binding.emplace(oc::executor::bind<registry>(*pinned));
         invoke_pinned(); // SAME native binding before any disk eviction.
         // Advance the ordinary test/main clock interval, then verify actual file
         // timestamps. Do not mutate recency behind the authoritative owned catalog.
         std::this_thread::sleep_for(std::chrono::seconds{1});
         const auto second_code = compile_request(runtime, session, second);
         BOOST_TEST(second_code.code.is_prepared());
         const auto file = std::filesystem::path{directory} / (a.str() + ".foc");
         struct stat first_state{}, second_state{};
         BOOST_REQUIRE(::stat(file.c_str(), &first_state) == 0);
         BOOST_REQUIRE(::stat((std::filesystem::path{directory} / (b.str() + ".foc")).c_str(), &second_state) == 0);
#if defined(__APPLE__)
         BOOST_REQUIRE(first_state.st_mtimespec.tv_sec < second_state.st_mtimespec.tv_sec ||
                       (first_state.st_mtimespec.tv_sec == second_state.st_mtimespec.tv_sec &&
                        first_state.st_mtimespec.tv_nsec < second_state.st_mtimespec.tv_nsec));
#else
         BOOST_REQUIRE(first_state.st_mtim.tv_sec < second_state.st_mtim.tv_sec ||
             (first_state.st_mtim.tv_sec == second_state.st_mtim.tv_sec &&
              first_state.st_mtim.tv_nsec < second_state.st_mtim.tv_nsec));
#endif
         const auto third_code = compile_request(runtime, session, third);
         BOOST_TEST(third_code.code.is_prepared());
         BOOST_TEST(!std::filesystem::exists(file));
         BOOST_REQUIRE(session.ready(a, request.configuration).has_value()); // same resident index, not remap
         BOOST_TEST((std::vector<std::byte>(pinned->native_code().begin(), pinned->native_code().end()) == native));
         invoke_pinned(); // A is absent on disk; the external binding still executes.
      });
      BOOST_REQUIRE(pinned.has_value());
      BOOST_TEST((std::vector<std::byte>(pinned->native_code().begin(), pinned->native_code().end()) == native));
      invoke_pinned(); // exercise_session already drained/destroyed session and compute.
   } catch (...) {
      failure = std::current_exception();
   }
   // SAME public owner/drain pattern as the ordinary consumer. Assertion,
   // compile and guest failures cannot bypass thread release or either drain.
   if (thread_prepared) {
      try {
         executor.release_thread();
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
      }
   }
   try {
      forge::asio::blocking::run(execution_runtime, executor.shutdown());
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
      try {
         forge::asio::blocking::run(execution_runtime, executor.shutdown());
      } catch (...) {
         // Preserve the failing outcome, but still attempt the scheduler drain.
      }
   }
   try {
      forge::asio::blocking::run(execution_runtime, scheduler.shutdown());
   } catch (...) {
      if (!failure)
         failure = std::current_exception();
      try {
         forge::asio::blocking::run(execution_runtime, scheduler.shutdown());
      } catch (...) {
      }
   }
   if (failure)
      std::rethrow_exception(failure);
}

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(cache_catalog_reads_unrelated_artifact_only_during_bounded_startup_scan) {
   if (!enter_fresh("optimizing_session/cache_catalog_reads_unrelated_artifact_only_during_bounded_startup_scan",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   BOOST_REQUIRE(request.configuration.execution.max_call_depth > 2);
   auto unrelated = request, new_request = request;
   --unrelated.configuration.execution.max_call_depth;
   new_request.configuration.execution.max_call_depth -= 2;
   const auto a = oc::identify(request), b = oc::identify(unrelated), c = oc::identify(new_request);
   auto directory = std::string{"/tmp/forge-oc-cache-catalog-reads-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] {
      ::unlink((std::filesystem::path{directory} / (b.str() + ".foc")).c_str());
      ::unlink((std::filesystem::path{directory} / (c.str() + ".foc")).c_str());
      remove_owned_cache(directory, a);
   }};
   // The unrelated file is a genuinely compiled SAME-input/configuration
   // variant, not synthetic bytes or a bypass of artifact validation.
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      static_cast<void>(compile_request(runtime, session, request));
      static_cast<void>(compile_request(runtime, session, unrelated));
   });
   struct stat watched{};
   const auto watched_path = std::filesystem::path{directory} / (b.str() + ".foc");
   BOOST_REQUIRE(::stat(watched_path.c_str(), &watched) == 0);
   BOOST_REQUIRE(watched.st_ino != 0);
   auto gate = spawn_publication_gate{.helper = helper};
   gate.watched_device.store(static_cast<std::uint64_t>(watched.st_dev), std::memory_order_relaxed);
   gate.watched_inode.store(static_cast<std::uint64_t>(watched.st_ino), std::memory_order_release);
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto startup_reads = unsigned{};
   auto loaded = false, compiled = false, prepared = false;
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session); // mandatory cold scan/checksum of ALL bounded recognized files
      startup_reads = gate.watched_reads.exchange(0, std::memory_order_acq_rel);
      const auto existing = compile_request(runtime, session, request);
      loaded =
          existing.status == oc::compiler_session::completion_status::loaded_from_disk && existing.code.identity() == a;
      const auto published = compile_request(runtime, session, new_request);
      compiled =
          published.status == oc::compiler_session::completion_status::compiled && published.code.identity() == c;
      prepared = existing.code.is_prepared() && published.code.is_prepared();
   });
   struct stat retained{};
   BOOST_REQUIRE(::stat(watched_path.c_str(), &retained) == 0);
   BOOST_TEST(startup_reads > 0U);
   BOOST_TEST(gate.watched_reads.load(std::memory_order_acquire) == 0U); // actual warm load AND persist
   BOOST_TEST(loaded);
   BOOST_TEST(compiled);
   BOOST_TEST(prepared);
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 2U); // HELLO then only the new COMPILE
   BOOST_TEST(retained.st_dev == watched.st_dev);
   BOOST_TEST(retained.st_ino == watched.st_ino);
   std::printf("cache_catalog_reads startup=%u unrelated_warm=%u launches=%u\n", startup_reads,
               gate.watched_reads.load(std::memory_order_acquire), gate.launches.load(std::memory_order_acquire));
}
#endif

BOOST_AUTO_TEST_CASE(cache_selected_checksum_is_revalidated_and_keeps_old_plus_temp_charge) {
   if (!enter_fresh("optimizing_session/cache_selected_checksum_is_revalidated_and_keeps_old_plus_temp_charge",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto encoded = read_fixture(".foc");
   BOOST_REQUIRE(!encoded.empty());
   auto directory = std::string{"/tmp/forge-oc-cache-selected-corrupt-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   const auto file = std::filesystem::path{directory} / (identity.str() + ".foc");
   write_owned_file(file, encoded);
   auto configuration = options(directory, helper);
   configuration.max_disk_bytes = encoded.size();
   configuration.max_disk_entries = 2;
   auto limited = false, no_ready = false, old_preserved = false;
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session); // valid candidate in the authoritative catalog
      // Deliberate corruption NEGATIVE, not supported concurrent external-cache
      // editing: keep inode/size unchanged and prove targeted checksum validation.
      encoded.back() ^= 1;
      const auto fd = ::open(file.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
      BOOST_REQUIRE(fd >= 0);
      const auto close_file = boost::scope::scope_exit{[fd] { ::close(fd); }};
      BOOST_REQUIRE(::pwrite(fd, &encoded.back(), 1, static_cast<off_t>(encoded.size() - 1)) == 1);
      BOOST_REQUIRE(::fsync(fd) == 0);
      try {
         static_cast<void>(compile_request(runtime, session, request));
      } catch (const oc::exceptions::compile_limit&) {
         limited = true;
      }
      no_ready = !session.ready(identity, request.configuration).has_value();
      auto input = std::ifstream{file, std::ios::binary};
      const auto bytes = std::vector<std::uint8_t>{std::istreambuf_iterator<char>{input}, {}};
      old_preserved = bytes == encoded;
   });
   BOOST_TEST(limited);
   BOOST_TEST(no_ready);
   BOOST_TEST(old_preserved);
   configuration.max_disk_bytes = 2 * encoded.size();
   auto rebuilt = false;
   exercise_session(configuration, [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session); // restart reconciles/quarantines the deliberate corrupt file
      const auto completed = compile_request(runtime, session, request);
      rebuilt = completed.status == oc::compiler_session::completion_status::compiled && completed.code.is_prepared() &&
                completed.code.identity() == identity;
   });
   BOOST_TEST(rebuilt);
}

BOOST_AUTO_TEST_CASE(cache_fresh_exec_restart_performs_hello_only_then_zero_compile_and_warm_hit) {
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   if (fresh_process::mode() == nullptr) {
      auto directory = std::string{"/tmp/forge-oc-cache-fresh-exec-XXXXXX"};
      BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
      const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
      const auto* previous = std::getenv("FORGE_OC_RESTART_CACHE");
      const auto saved = previous ? std::optional<std::string>{previous} : std::nullopt;
      const auto restore = boost::scope::scope_exit{[&] {
         if (saved)
            ::setenv("FORGE_OC_RESTART_CACHE", saved->c_str(), 1);
         else
            ::unsetenv("FORGE_OC_RESTART_CACHE");
      }};
      BOOST_REQUIRE(::setenv("FORGE_OC_RESTART_CACHE", directory.c_str(), 1) == 0);
      for (const auto mode : {"cache-write", "cache-read"}) {
         const auto child = fresh_process::run(
             "optimizing_session/cache_fresh_exec_restart_performs_hello_only_then_zero_compile_and_warm_hit", mode);
         BOOST_REQUIRE(child.error == 0 && !child.timed_out);
         BOOST_REQUIRE(WIFEXITED(child.status));
         BOOST_REQUIRE(WEXITSTATUS(child.status) == 0);
      }
      return;
   }
   fresh_process::disable_core_dumps();
   const auto* directory = std::getenv("FORGE_OC_RESTART_CACHE");
   BOOST_REQUIRE(directory != nullptr);
   const auto writer = std::string_view{fresh_process::mode()} == "cache-write";
   BOOST_REQUIRE(writer || std::string_view{fresh_process::mode()} == "cache-read");
   auto gate = spawn_publication_gate{.helper = helper};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
      const auto completed = compile_request(runtime, session, request);
      BOOST_CHECK(completed.status == (writer ? oc::compiler_session::completion_status::compiled
                                              : oc::compiler_session::completion_status::loaded_from_disk));
      BOOST_TEST(completed.code.is_prepared());
      const auto expected = writer ? 2U : 1U;
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == expected);
      const auto ready = session.ready(identity, request.configuration);
      BOOST_REQUIRE(ready.has_value());
      BOOST_TEST(ready->native_code().data() == completed.code.native_code().data());
      const auto again = compile_request(runtime, session, request);
      BOOST_CHECK(again.status == oc::compiler_session::completion_status::already_ready);
      BOOST_TEST(again.code.native_code().data() == completed.code.native_code().data());
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == expected); // warm hit spawns nothing
   });
   std::printf("fresh_cache writer=%u actual_helper_spawns=%u overrides=%u\n", static_cast<unsigned>(writer),
               gate.launches.load(std::memory_order_acquire), gate.overrides.load(std::memory_order_acquire));
   BOOST_TEST(gate.overrides.load(std::memory_order_acquire) == 0U);
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(startup_nonexecutable_helper_readonly_cache_and_contended_lock_fail_closed) {
   if (!enter_fresh("optimizing_session/startup_nonexecutable_helper_readonly_cache_and_contended_lock_fail_closed"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-startup-unavailable-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto nonexecutable = std::filesystem::path{directory} / "helper-not-executable";
   const auto lock_path = std::filesystem::path{directory} / "owner.lock";
   const auto remove_cache = boost::scope::scope_exit{[&] {
      ::chmod(directory.c_str(), 0700);
      ::unlink(nonexecutable.c_str());
      remove_owned_cache(directory, identity);
   }};
   write_owned_file(nonexecutable, {});
   exercise_session(options(directory, nonexecutable), [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::compiler_unavailable);
      BOOST_TEST(!std::filesystem::exists(lock_path));
      auto copy = request;
      const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
      BOOST_CHECK(rejected.status == oc::admission_status::not_initialized);
   });
   BOOST_REQUIRE(::unlink(nonexecutable.c_str()) == 0);
   // Effective-root bypasses ordinary discretionary write permission. This
   // scenario's explicit unprivileged test precondition is not silently skipped.
   BOOST_REQUIRE(::geteuid() != 0);
   BOOST_REQUIRE(::chmod(directory.c_str(), 0500) == 0);
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::cache_io);
      auto copy = request;
      const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
      BOOST_CHECK(rejected.status == oc::admission_status::not_initialized);
   });
   BOOST_REQUIRE(::chmod(directory.c_str(), 0700) == 0);
   const auto held = ::open(lock_path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
   BOOST_REQUIRE(held >= 0);
   const auto close_lock = boost::scope::scope_exit{[held] { ::close(held); }};
   BOOST_REQUIRE(::flock(held, LOCK_EX | LOCK_NB) == 0);
   struct stat original{};
   BOOST_REQUIRE(::fstat(held, &original) == 0);
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      BOOST_CHECK_THROW(initialize(runtime, session), oc::exceptions::cache_io);
      struct stat current{};
      BOOST_REQUIRE(::lstat(lock_path.c_str(), &current) == 0);
      BOOST_TEST(original.st_dev == current.st_dev);
      BOOST_TEST(original.st_ino == current.st_ino);
      auto copy = request;
      const auto rejected = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
      BOOST_CHECK(rejected.status == oc::admission_status::not_initialized);
   });
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(wrong_hint_and_cross_profile_tickets_do_not_poison_verified_ready) {
   if (!enter_fresh("optimizing_session/wrong_hint_and_cross_profile_tickets_do_not_poison_verified_ready"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto foreign = oc::compile_request::decode(read_fixture(".request", false), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-session-wrong-key-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto gate = spawn_publication_gate{.helper = helper};
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      initialize(runtime, session);
      auto wrong = request;
      auto admitted = session.enqueue(oc::identify(foreign), std::move(wrong), forge::asio::task::priority{});
      BOOST_REQUIRE(admitted.completion.has_value());
      BOOST_CHECK_THROW(await_ticket(runtime, std::move(*admitted.completion)), oc::exceptions::invalid_module);
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
      BOOST_CHECK_THROW(compile_request(runtime, session, foreign), oc::exceptions::unsupported_target);
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
      const auto legitimate = compile_request(runtime, session, request);
      BOOST_TEST(legitimate.code.is_prepared());
      BOOST_REQUIRE(session.ready(identity, request.configuration).has_value());
      auto bad_again = request;
      auto rejected = session.enqueue(oc::identify(foreign), std::move(bad_again), forge::asio::task::priority{});
      BOOST_REQUIRE(rejected.completion.has_value());
      BOOST_CHECK_THROW(await_ticket(runtime, std::move(*rejected.completion)), oc::exceptions::invalid_module);
      const auto ready = session.ready(identity, request.configuration);
      BOOST_REQUIRE(ready.has_value());
      BOOST_TEST(ready->native_code().data() == legitimate.code.native_code().data());
      BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 2U);
   });
}
#endif

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
BOOST_AUTO_TEST_CASE(startup_successful_retained_waiter_consumes_after_all_old_owners_are_destroyed) {
   if (!enter_fresh("optimizing_session/startup_successful_retained_waiter_consumes_after_all_old_owners_are_destroyed",
                    std::chrono::seconds{10}))
      return;
   retained_successful_initialize(true);
}

BOOST_AUTO_TEST_CASE(startup_successful_retained_waiter_drops_after_all_old_owners_are_destroyed) {
   if (!enter_fresh("optimizing_session/startup_successful_retained_waiter_drops_after_all_old_owners_are_destroyed",
                    std::chrono::seconds{10}))
      return;
   retained_successful_initialize(false);
}

BOOST_AUTO_TEST_CASE(startup_cancelled_waiter_delivers_before_shutdown_and_remains_owner_tracked) {
   if (!enter_fresh("optimizing_session/startup_cancelled_waiter_delivers_before_shutdown_and_remains_owner_tracked"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   auto directory = std::string{"/tmp/forge-oc-startup-pending-delivery-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { ::rmdir(directory.c_str()); }};
   auto stop = std::stop_source{};
   stop.request_stop();
   auto delivered = false;
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      auto result =
          boost::asio::co_spawn(runtime.context(), session.initialize(oc::target_profile::native(), stop.get_token()),
                                boost::asio::use_future);
      BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      try {
         result.get();
      } catch (const oc::exceptions::cancelled&) {
         delivered = true;
      }
      // There has been NO shutdown yet. Terminal cancellation alone must not
      // claim owner quiescence or skip its registration/scheduler drain.
   });
   BOOST_TEST(delivered);
   BOOST_TEST(!std::filesystem::exists(std::filesystem::path{directory} / "owner.lock"));
}

#if defined(__APPLE__)
BOOST_AUTO_TEST_CASE(startup_reap_failure_retains_record_until_successful_shutdown_retry) {
   if (!enter_fresh("optimizing_session/startup_reap_failure_retains_record_until_successful_shutdown_retry",
                    std::chrono::seconds{10}))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-startup-reap-retry-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] {
      ::unlink((std::filesystem::path{directory} / "owner.lock").c_str());
      ::rmdir(directory.c_str());
   }};
   auto gate = spawn_publication_gate{.helper = helper};
   gate.stop_next.store(true, std::memory_order_release);
   gate.reap_failures.store(2, std::memory_order_release);
   active_spawn_gate.store(&gate, std::memory_order_release);
   const auto deactivate = boost::scope::scope_exit{[&] {
      gate.reap_failures.store(0, std::memory_order_release);
      active_spawn_gate.store(nullptr, std::memory_order_release);
      gate.release();
   }};
   auto initialization_failed = false;
   auto first_shutdown_failed = false;
   auto first_count = unsigned{};
   auto retry_count = unsigned{};
   auto waiter = std::optional<boost::asio::awaitable<void>>{};
   exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
      // Any assertion/exception in the body MUST disarm before the common
      // mandatory cleanup path. No real waitpid oracle runs before that drain.
      const auto disarm = boost::scope::scope_exit{[&] { gate.reap_failures.store(0, std::memory_order_release); }};
      waiter.emplace(session.initialize(oc::target_profile::native(), {})); // retain failed-after-spawn lazy outcome
      const auto deadline = clock_type::now() + std::chrono::seconds{2};
      auto retained_failure_observed = false;
      for (auto attempt = unsigned{}; attempt < 2000 && clock_type::now() < deadline; ++attempt) {
         auto copy = request;
         auto admitted = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
         BOOST_REQUIRE(!admitted.completion.has_value());
         if (admitted.status == oc::admission_status::stopped) {
            retained_failure_observed = true;
            break;
         }
         BOOST_REQUIRE(admitted.status == oc::admission_status::not_initialized);
         std::this_thread::sleep_for(std::chrono::milliseconds{1}); // ordinary test/main thread only
      }
      // There has been no request_stop/external cancellation. In this isolated
      // timeline only the retained init finisher can publish stopped, under the
      // same index exclusion as its finalized/error outcome. Not a fake state.
      BOOST_REQUIRE(retained_failure_observed);
      first_count = gate.injected_reap_failures.load(std::memory_order_acquire);
      try {
         shutdown(runtime, session);
      } catch (const oc::exceptions::shutdown_incomplete&) {
         first_shutdown_failed = true;
      }
      retry_count = gate.injected_reap_failures.load(std::memory_order_acquire);
      gate.reap_failures.store(0, std::memory_order_release);
      shutdown(runtime, session); // SAME finalized/live record, SAME sole actual async reaper
   });
   BOOST_REQUIRE(waiter.has_value());
   {
      // Failed-after-spawn setup must be independent too: all old IO services,
      // child/callable storage and injected owners were destroyed above.
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
      auto result = boost::asio::co_spawn(runtime.context(), std::move(*waiter), boost::asio::use_future);
      waiter.reset();
      BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      try {
         result.get();
      } catch (const oc::exceptions::shutdown_incomplete&) {
         initialization_failed = true; // initial error is preserved, never overwritten by shutdown cancel
      }
   }
   const auto pid = gate.latest_pid.load(std::memory_order_acquire);
   const auto reap_calls = gate.reap_calls.load(std::memory_order_acquire);
   active_spawn_gate.store(nullptr, std::memory_order_release);
   auto status = int{};
   auto waited = pid_t{-1};
   auto observed_error = int{};
   if (pid > 0) {
      do {
         errno = 0;
         waited = ::waitpid(pid, &status, WNOHANG);
         observed_error = errno;
      } while (waited < 0 && observed_error == EINTR);
   }
   BOOST_TEST(initialization_failed);
   BOOST_TEST(first_shutdown_failed);
   BOOST_TEST(first_count == 1U);
   BOOST_TEST(retry_count == 2U);
   BOOST_TEST(gate.launches.load(std::memory_order_acquire) == 1U);
   BOOST_TEST(gate.stop_result.load(std::memory_order_acquire) == 0);
   BOOST_TEST(gate.stopped_pid.load(std::memory_order_acquire) == pid);
   BOOST_TEST(pid > 0);
   BOOST_TEST(reap_calls >= 3U);
   BOOST_TEST(waited == -1);
   BOOST_TEST(observed_error == ECHILD);
   BOOST_TEST(!std::filesystem::exists(std::filesystem::path{directory} / "owner.lock"));
   // Failure happened before cache acquisition; do not claim a released cache
   // lock. A fresh owner still proves the SAME cold signal/child drains permit
   // genuine HELLO/cache initialization after the explicit successful retry.
   exercise_session(options(directory, helper),
                    [&](auto& session, auto& runtime, auto&) { initialize(runtime, session); });
   std::printf("startup_reap_retry pid=%ld initial_errors=%u cleanup_errors=%u reap_calls=%u "
               "first_shutdown_incomplete=%u post_cleanup_waitpid=%ld errno=%d\n",
               static_cast<long>(pid), first_count, retry_count, reap_calls, first_shutdown_failed ? 1U : 0U,
               static_cast<long>(waited), observed_error);
}
#endif

BOOST_AUTO_TEST_CASE(startup_cancelled_pending_waiter_survives_terminal_shutdown_and_owner_destruction) {
   if (!enter_fresh(
           "optimizing_session/startup_cancelled_pending_waiter_survives_terminal_shutdown_and_owner_destruction"))
      return;
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   const auto request = oc::compile_request::decode(read_request(), 1024U * 1024U);
   const auto identity = oc::identify(request);
   auto directory = std::string{"/tmp/forge-oc-startup-retained-waiter-XXXXXX"};
   BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
   const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
   auto stop = std::stop_source{};
   stop.request_stop();
   for (const auto consume : {true, false}) {
      auto waiter = std::optional<boost::asio::awaitable<void>>{};
      exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
         waiter.emplace(session.initialize(oc::target_profile::native(), stop.get_token()));
         shutdown(runtime, session); // terminal + registration/handle drain before owner destruction
      });
      BOOST_REQUIRE(waiter.has_value());
      // Both the session AND old injected runtime/compute storage are now gone.
      // This outcome owns no job/child/IO objects tied to that old executor.
      if (consume) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
         auto result = boost::asio::co_spawn(runtime.context(), std::move(*waiter), boost::asio::use_future);
         waiter.reset();
         BOOST_REQUIRE(result.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
         BOOST_CHECK_THROW(result.get(), oc::exceptions::cancelled);
      } else {
         waiter.reset();
      }
   }
   BOOST_TEST(!std::filesystem::exists(std::filesystem::path{directory} / "owner.lock"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()

#if defined(FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC)
namespace {
struct benchmark_request_fixture {
   oc::compile_request request;
   std::array<std::uint64_t, 4> inputs;
};

benchmark_request_fixture load_benchmark_request(const char* name) {
   const auto* configured = std::getenv("FORGE_OC_BENCHMARK_CORPUS_DIRECTORY");
   BOOST_REQUIRE_MESSAGE(configured != nullptr, "explicit immutable benchmark corpus is required");
   const auto directory = std::filesystem::path{configured};
   BOOST_REQUIRE(directory.is_absolute());
   BOOST_REQUIRE(std::filesystem::is_directory(directory));
   for (const auto workload : {"benchmark-integer", "benchmark-memory"})
      for (const auto suffix : {".wasm", ".inputs", "-linux-x86-64.request", "-linux-x86-64.foc",
                                "-darwin-arm64.request", "-darwin-arm64.foc"}) {
         const auto path = directory / (std::string{workload} + suffix);
         BOOST_REQUIRE(std::filesystem::is_regular_file(std::filesystem::symlink_status(path)));
         const auto size = std::filesystem::file_size(path);
         BOOST_REQUIRE((size > 0 && size <= 2 * 1024 * 1024));
      }
   auto count = 0U;
   for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator{directory})
      BOOST_REQUIRE(++count <= 12U);
   BOOST_REQUIRE_EQUAL(count, 12U);
   const auto prefix = directory / name;
   const auto read = [](const std::filesystem::path& path) {
      auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
      BOOST_REQUIRE(input.good());
      const auto size = input.tellg();
      BOOST_REQUIRE((size > 0 && size <= 2 * 1024 * 1024));
      auto bytes = std::vector<std::uint8_t>(static_cast<std::size_t>(size));
      input.seekg(0);
      BOOST_REQUIRE(
          static_cast<bool>(input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))));
      return bytes;
   };
   const auto wasm = read(prefix.string() + ".wasm");
   const auto input_bytes = read(prefix.string() + ".inputs");
   BOOST_REQUIRE_EQUAL(input_bytes.size(), 32U);
   const auto inputs = forge::raw::unpack_exact<std::array<std::uint64_t, 4>>(input_bytes);
   auto selected = std::optional<oc::compile_request>{};
   for (const auto suffix : {"-linux-x86-64", "-darwin-arm64"}) {
      const auto encoded = read(prefix.string() + suffix + ".request");
      auto request = oc::compile_request::decode(encoded, encoded.size());
      BOOST_REQUIRE(request.wasm == wasm);
      const auto bytes = read(prefix.string() + suffix + ".foc");
      const auto code = oc::artifact::decode(bytes, oc::identify(request), bytes.size());
      BOOST_REQUIRE((code.image().configuration == oc::canonicalize(request.configuration)));
      BOOST_REQUIRE_EQUAL(code.image().configuration.imports.size(), 1U);
      const auto& imported = code.image().configuration.imports[0];
      BOOST_REQUIRE(imported.module == "env");
      BOOST_REQUIRE(imported.name == "printui");
      namespace vm = forge::vm::wasm::interpret;
      BOOST_REQUIRE((imported.signature == vm::host_function{{vm::i64}, {}}));
      BOOST_REQUIRE_EQUAL(code.exports().size(), 1U);
      BOOST_REQUIRE(code.exports()[0].name == "apply");
      BOOST_REQUIRE(code.exports()[0].function_index == 1U);
      if (request.configuration.target.matches_host()) {
         BOOST_REQUIRE(!selected.has_value());
         selected.emplace(std::move(request));
      }
   }
   BOOST_REQUIRE_MESSAGE(selected.has_value(), "corpus has no exact current host profile");
   return {std::move(*selected), inputs};
}
} // namespace

// Never selected by the historical optimizing_session suite/default CTest.
// Native execution/checksum is a separate executor entry, not a cache metric.
BOOST_AUTO_TEST_SUITE(optimizing_benchmark_session, *boost::unit_test::disabled())

BOOST_AUTO_TEST_CASE(compile_memory_and_fresh_exec_cache_modes) {
   const auto* helper = std::getenv("FORGE_OC_COMPILER_HELPER");
   BOOST_REQUIRE(helper != nullptr);
   for (const auto name : {"benchmark-integer", "benchmark-memory"}) {
      const auto fixture = load_benchmark_request(name);
      const auto identity = oc::identify(fixture.request);
      if (fresh_process::mode() == nullptr) {
         auto directory = std::string{"/tmp/forge-oc-benchmark-XXXXXX"};
         BOOST_REQUIRE(::mkdtemp(directory.data()) != nullptr);
         const auto remove_cache = boost::scope::scope_exit{[&] { remove_owned_cache(directory, identity); }};
         const auto* previous = std::getenv("FORGE_OC_BENCHMARK_CACHE");
         const auto saved = previous ? std::optional<std::string>{previous} : std::nullopt;
         const auto restore = boost::scope::scope_exit{[&] {
            if (saved)
               ::setenv("FORGE_OC_BENCHMARK_CACHE", saved->c_str(), 1);
            else
               ::unsetenv("FORGE_OC_BENCHMARK_CACHE");
         }};
         BOOST_REQUIRE(::setenv("FORGE_OC_BENCHMARK_CACHE", directory.c_str(), 1) == 0);
         for (const auto stage : {"write", "read"}) {
            const auto mode = std::string{name} + "-" + stage;
            const auto child =
                fresh_process::run("optimizing_benchmark_session/compile_memory_and_fresh_exec_cache_modes", mode,
                                   std::chrono::seconds{10});
            BOOST_REQUIRE((child.error == 0 && !child.timed_out));
            BOOST_REQUIRE(WIFEXITED(child.status));
            BOOST_REQUIRE(WEXITSTATUS(child.status) == 0);
         }
         continue;
      }
      fresh_process::disable_core_dumps();
      const auto mode = std::string_view{fresh_process::mode()};
      const auto writer = mode == std::string{name} + "-write";
      const auto reader = mode == std::string{name} + "-read";
      if (!writer && !reader)
         continue;
      const auto* directory = std::getenv("FORGE_OC_BENCHMARK_CACHE");
      BOOST_REQUIRE(directory != nullptr);
      BOOST_REQUIRE(std::filesystem::path{directory}.is_absolute());
      BOOST_REQUIRE(std::filesystem::is_directory(std::filesystem::symlink_status(directory)));
      auto gate = spawn_publication_gate{.helper = helper};
      active_spawn_gate.store(&gate, std::memory_order_release);
      const auto deactivate = boost::scope::scope_exit{[&] {
         active_spawn_gate.store(nullptr, std::memory_order_release);
         gate.release();
      }};
      auto initialized_ns = std::int64_t{};
      auto service_ns = std::int64_t{};
      auto queued_ns = std::int64_t{};
      auto elapsed = std::array<std::int64_t, 128>{};
      auto wasm = std::string{};
      const auto lifecycle_start = clock_type::now();
      auto phase_start = lifecycle_start;
      const char* phase = "owner_startup";
      try {
         exercise_session(options(directory, helper), [&](auto& session, auto& runtime, auto&) {
            const auto target = oc::target_profile::native();
            phase = "initialize_hello";
            phase_start = clock_type::now();
            auto start = clock_type::now();
            auto init =
                boost::asio::co_spawn(runtime.context(), session.initialize(target, {}), boost::asio::use_future);
            if (init.wait_for(std::chrono::seconds{3}) != std::future_status::ready)
               throw std::runtime_error{"benchmark initialization did not finish within its bound"};
            init.get();
            const auto initialized = clock_type::now();
            initialized_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(initialized - start).count();
            BOOST_REQUIRE(gate.launches.load(std::memory_order_acquire) == 1U); // actual HELLO-only child
            phase = writer ? "compile_service" : "restart_disk_load";
            phase_start = clock_type::now();
            auto copy = fixture.request; // copy/hash/fixture validation excluded from admission timer
            start = clock_type::now();
            auto admitted = session.enqueue(identity, std::move(copy), forge::asio::task::priority{});
            if (admitted.status != oc::admission_status::accepted_unverified || !admitted.completion)
               throw std::runtime_error{"benchmark request was not admitted"};
            auto future = boost::asio::co_spawn(
                runtime.context(),
                collect_completion(std::move(*admitted.completion).wait(start + std::chrono::seconds{3}, {})),
                boost::asio::use_future);
            if (future.wait_for(std::chrono::seconds{3}) != std::future_status::ready)
               throw std::runtime_error{"benchmark compilation/cache load did not finish within its bound"};
            auto completed = future.get();
            if (!completed)
               throw std::runtime_error{"benchmark completion missing"};
            // Product completion timestamp is the checked/prepared endpoint, not
            // the later future delivery/assertions or an LLVM-only timer.
            service_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(completed->completed_time - start).count();
            queued_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(completed->completed_time - completed->queued_time)
                    .count();
            BOOST_REQUIRE(service_ns >= 0);
            BOOST_REQUIRE(queued_ns >= 0);
            BOOST_REQUIRE((completed->status == (writer ? oc::compiler_session::completion_status::compiled
                                                        : oc::compiler_session::completion_status::loaded_from_disk)));
            BOOST_REQUIRE(completed->code.is_prepared());
            BOOST_REQUIRE(completed->code.identity() == identity);
            BOOST_REQUIRE((completed->code.image().configuration == oc::canonicalize(fixture.request.configuration)));
            wasm = completed->code.image().wasm.str();
            phase = "warm_memory_lookup";
            phase_start = clock_type::now();
            for (std::size_t iteration = 0; iteration < elapsed.size(); ++iteration) {
               const auto before = clock_type::now();
               const auto ready = session.ready(identity, completed->code.image().configuration);
               const auto after = clock_type::now();
               elapsed[iteration] = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
               BOOST_REQUIRE(elapsed[iteration] >= 0);
               BOOST_REQUIRE(ready.has_value());
               BOOST_REQUIRE(ready->identity() == identity);
               BOOST_REQUIRE(ready->native_code().data() == completed->code.native_code().data());
            }
            phase = "memory_ticket_control";
            phase_start = clock_type::now();
            const auto again = compile_request(runtime, session, fixture.request); // untimed route/correctness control
            BOOST_REQUIRE(again.status == oc::compiler_session::completion_status::already_ready);
            BOOST_REQUIRE(again.code.native_code().data() == completed->code.native_code().data());
            BOOST_REQUIRE(gate.launches.load(std::memory_order_acquire) == (writer ? 2U : 1U));
            phase = "owner_shutdown";
            phase_start = clock_type::now();
         }); // sole helper reap + session/compute drain before publishing samples
      } catch (...) {
         // Existing exercise_session performs mandatory cleanup before
         // rethrowing its saved error. Diagnostic intervals include that drain,
         // not just helper/LLVM time. Preserve the exact original exception.
         const auto end = clock_type::now();
         std::fprintf(stderr,
                      "oc_benchmark_failure stage=%s workload=%s mode=%s "
                      "elapsed_through_drain_ns=%" PRId64 " phase_elapsed_through_drain_ns=%" PRId64
                      " actual_helper_spawns=%u reap_calls=%u injected_reap_failures=%u "
                      "overrides=%u latest_pid=%ld\n",
                      phase, name, writer ? "write" : "read",
                      std::chrono::duration_cast<std::chrono::nanoseconds>(end - lifecycle_start).count(),
                      std::chrono::duration_cast<std::chrono::nanoseconds>(end - phase_start).count(),
                      gate.launches.load(std::memory_order_acquire), gate.reap_calls.load(std::memory_order_acquire),
                      gate.injected_reap_failures.load(std::memory_order_acquire),
                      gate.overrides.load(std::memory_order_acquire),
                      static_cast<long>(gate.latest_pid.load(std::memory_order_acquire)));
         try {
            std::rethrow_exception(std::current_exception());
         } catch (const oc::exceptions::compile_limit& error) {
            // The owner already drained above. Inspect only our fixed frame
            // keys/values; never print what(), locations, paths or payload.
            constexpr std::array<std::string_view, 7> keys{
                "oc_helper_exchange", "oc_helper_phase", "oc_helper_arm_ms", "oc_helper_elapsed_ms",
                "oc_helper_io",       "oc_helper_reap",  "oc_helper_owned"};
            constexpr std::array<std::string_view, 9> phases{"before_io",  "request_write", "hello_header",
                                                             "hello_body", "ack_write",     "reply_header",
                                                             "reply_body", "output_eof",    "await_reap"};
            auto values = std::array<const char*, 7>{};
            auto frames = std::size_t{};
            auto invalid = false;
            for (const auto& frame : error.context_frames()) {
               if (frame.message != "compiler helper deadline snapshot")
                  continue;
               ++frames;
               for (const auto& field : frame.context) {
                  for (std::size_t index = 0; index < keys.size(); ++index) {
                     if (field.key != keys[index])
                        continue;
                     const auto value = std::string_view{field.value};
                     auto allowed = !field.redacted && !values[index];
                     if (index == 0) {
                        allowed = allowed && (value == "preflight" || value == "compile");
                     } else if (index == 1) {
                        auto known = false;
                        for (const auto phase_name : phases)
                           known = known || value == phase_name;
                        allowed = allowed && known;
                     } else if (index == 2 || index == 3) {
                        allowed = allowed && !value.empty() && value.size() <= 20;
                        for (const auto character : value)
                           allowed = allowed && character >= '0' && character <= '9';
                     } else {
                        allowed = allowed && (value == "true" || value == "false");
                     }
                     if (allowed)
                        values[index] = field.value.c_str();
                     else
                        invalid = true;
                  }
               }
            }
            auto complete = frames == 1 && !invalid;
            for (const auto* value : values)
               complete = complete && value != nullptr;
            const auto field = [&](std::size_t index) { return values[index] ? values[index] : "unavailable"; };
            std::fprintf(stderr,
                         "oc_benchmark_deadline_snapshot code=%d diagnostic=%s exchange=%s helper_phase=%s "
                         "arm_ms=%s elapsed_ms=%s io_inflight=%s reap_inflight=%s owned=%s\n",
                         error.code().value(), complete ? "COMPLETE" : "INCOMPLETE", field(0), field(1), field(2),
                         field(3), field(4), field(5), field(6));
         } catch (...) {
         }
         throw;
      }
      BOOST_REQUIRE(gate.overrides.load(std::memory_order_acquire) == 0U);
      BOOST_REQUIRE(gate.latest_pid.load(std::memory_order_acquire) > 0);
      auto status = 0;
      errno = 0;
      const auto waited = ::waitpid(gate.latest_pid.load(std::memory_order_acquire), &status, WNOHANG);
      const auto wait_error = errno;
      BOOST_REQUIRE(waited == -1);
      BOOST_REQUIRE(wait_error == ECHILD); // only after the production sole reaper/drains
      const auto id = identity.str();
      const auto& target = fixture.request.configuration.target;
      std::printf("oc_input engine=forge workload=%s receiver=%" PRIu64 " code=%" PRIu64 " action=%" PRIu64
                  " expected_checksum=%" PRIu64 " identity=%s wasm=%s llvm=%s codegen=%u host_abi=%u\n",
                  name, fixture.inputs[0], fixture.inputs[1], fixture.inputs[2], fixture.inputs[3], id.c_str(),
                  wasm.c_str(), target.llvm_version.c_str(), target.codegen_version, target.host_abi_version);
      std::printf("oc_metric engine=forge boundary=session_initialize stage=control_startup workload=%s "
                  "fresh_exec=1 elapsed_ns=%" PRId64 " speed_verdict=NOT_EVALUATED\n",
                  name, initialized_ns);
      std::printf("oc_metric engine=forge boundary=session_admission_to_prepared stage=%s workload=%s "
                  "fresh_exec=1 elapsed_ns=%" PRId64 " queued_to_prepared_ns=%" PRId64 " expected_checksum=%" PRIu64
                  " identity=%s wasm=%s actual_helper_spawns=%u "
                  "speed_verdict=NOT_EVALUATED\n",
                  writer ? "compile_service" : "restart_disk_load", name, service_ns, queued_ns, fixture.inputs[3],
                  id.c_str(), wasm.c_str(), gate.launches.load(std::memory_order_acquire));
      auto total = std::int64_t{};
      for (std::size_t iteration = 0; iteration < elapsed.size(); ++iteration) {
         total += elapsed[iteration];
         std::printf("oc_metric engine=forge boundary=ready_lookup stage=warm_memory workload=%s "
                     "cache_origin=%s iteration=%zu elapsed_ns=%" PRId64 " speed_verdict=NOT_EVALUATED\n",
                     name, writer ? "compiled" : "disk", iteration, elapsed[iteration]);
      }
      std::printf("oc_metric engine=forge boundary=ready_lookup stage=warm_aggregate workload=%s "
                  "cache_origin=%s iterations=128 elapsed_ns=%" PRId64 " speed_verdict=NOT_EVALUATED\n",
                  name, writer ? "compiled" : "disk", total);
      return; // this fresh process executes precisely the requested single workload/mode
   }
   BOOST_REQUIRE_MESSAGE(fresh_process::mode() == nullptr, "unrecognized benchmark child mode");
}

BOOST_AUTO_TEST_SUITE_END()
#endif
