module;

#include <coroutine>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/process/v2/exit_code.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/scope/scope_exit.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fcntl.h>
#include <forge/exceptions/macros.hpp>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ratio>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

module forge.vm.wasm.optimizing.compiler_session;

import forge.core.version;
import forge.core.git_revision;
import forge.exceptions;

#include "details/compiler_session_ticket_state.hxx"
#include "details/compiler_session_impl.hxx"
#include "details/compiler_session_impl_child.hxx"

namespace forge::vm::wasm::optimizing {
namespace {

constexpr auto running_status = W_STOPCODE(SIGSTOP);
static_assert(WIFSTOPPED(running_status) && !WIFEXITED(running_status) && !WIFSIGNALED(running_status) &&
              !WIFCONTINUED(running_status));
static_assert(std::atomic<pid_t>::is_always_lock_free && std::atomic<int>::is_always_lock_free);
// Public target SDK/libc encoding, not a claimed portable POSIX wire constant.
// The public Boost predicate is additionally checked before actual child launch.

void require_spawn(int error) {
   if (error != 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper spawn setup failed");
}

int duplicate_endpoint(int fd) {
   const auto result = ::fcntl(fd, F_DUPFD_CLOEXEC, 3);
   if (result < 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper endpoint isolation failed");
   return result;
}

} // namespace

compiler_session::impl::child::child(boost::asio::any_io_executor executor, const session_options& options_value,
                                     compiler_protocol::exchange_id exchange_value,
                                     crypto::digest::sha256 identity_value, target_profile target, bool preflight_value)
    : options{options_value}, exchange{exchange_value}, identity{identity_value}, preflight{preflight_value},
      strand{boost::asio::make_strand(std::move(executor))}, request_socket{strand}, response_socket{strand},
      timer{strand} {
   using clock = std::chrono::steady_clock;
   if (options.helper_timeout.count() >
       std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max()).count())
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler helper timeout conversion overflow");
   const auto delay = std::chrono::duration_cast<clock::duration>(options.helper_timeout);
   const auto now = clock::now();
   diagnostic_started = now;
   if (now.time_since_epoch() > clock::duration::max() - delay)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "compiler helper deadline overflow");
   deadline = now + delay;
   if (!boost::process::v2::process_is_running(running_status))
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "public Boost process running status mismatch");
   expected.forge_version = {core::version_major, core::version_minor, core::version_patch};
   expected.build_revision = forge::git_revision_sha;
   expected.build_timestamp = forge::git_revision_unix_timestamp;
   expected.target = std::move(target);
   expected.limits = options.helper_limits;
   cancelled_error = std::make_exception_ptr(exceptions::cancelled{"compiler helper was cancelled"});
   timeout_error = std::make_exception_ptr(exceptions::compile_limit{"compiler helper deadline exceeded"});
   unavailable_error = std::make_exception_ptr(exceptions::compiler_unavailable{"compiler helper failed"});
   allocation_error = std::make_exception_ptr(exceptions::compile_limit{"compiler helper allocation failed"});
}

compiler_session::impl::child::~child() {
   if (!is_quiescent())
      std::terminate();
   if (parent_request_endpoint >= 0)
      ::close(std::exchange(parent_request_endpoint, -1));
   if (parent_response_endpoint >= 0)
      ::close(std::exchange(parent_response_endpoint, -1));
   if (const auto endpoint = launched_request_endpoint.exchange(-1, std::memory_order_relaxed); endpoint >= 0)
      ::close(endpoint);
   if (const auto endpoint = launched_response_endpoint.exchange(-1, std::memory_order_relaxed); endpoint >= 0)
      ::close(endpoint);
}

void compiler_session::impl::child::spawn(std::stop_token stop) {
   if (stop.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler helper cancelled before spawn");
   workspace = (std::filesystem::temp_directory_path() / "forge-wasm-compiler-XXXXXX").string();
   if (::mkdtemp(workspace.data()) == nullptr) {
      workspace.clear();
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper workspace is unavailable");
   }
   workspace_pending.store(true, std::memory_order_release);
   auto request_endpoints = std::array<int, 2>{-1, -1};
   auto response_endpoints = std::array<int, 2>{-1, -1};
   const auto close_endpoints = boost::scope::scope_exit{[&] {
      for (auto fd : request_endpoints)
         if (fd >= 0)
            ::close(fd);
      for (auto fd : response_endpoints)
         if (fd >= 0)
            ::close(fd);
   }};
   int socket_type = SOCK_STREAM;
#if defined(__linux__)
   socket_type |= SOCK_CLOEXEC; // Atomic at creation, not a later descriptor mutation.
#endif
   if (::socketpair(AF_UNIX, socket_type, 0, request_endpoints.data()) != 0 ||
       ::socketpair(AF_UNIX, socket_type, 0, response_endpoints.data()) != 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper socketpair is unavailable");
   // Ensure child action sources cannot collide with stdin/stdout/stderr,
   // including when the embedding process started with a standard fd closed.
   const auto parent_request = duplicate_endpoint(request_endpoints[0]);
   auto close_parent_request = boost::scope::scope_exit{[parent_request] { ::close(parent_request); }};
   ::close(std::exchange(request_endpoints[0], -1));
   const auto child_request = duplicate_endpoint(request_endpoints[1]);
   const auto close_child_request = boost::scope::scope_exit{[child_request] { ::close(child_request); }};
   ::close(std::exchange(request_endpoints[1], -1));
   const auto parent_response = duplicate_endpoint(response_endpoints[0]);
   auto close_parent_response = boost::scope::scope_exit{[parent_response] { ::close(parent_response); }};
   ::close(std::exchange(response_endpoints[0], -1));
   const auto child_response = duplicate_endpoint(response_endpoints[1]);
   const auto close_child_response = boost::scope::scope_exit{[child_response] { ::close(child_response); }};
   ::close(std::exchange(response_endpoints[1], -1));
#if defined(__APPLE__)
   const auto no_sigpipe = 1;
   if (::setsockopt(parent_request, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler parent endpoint SIGPIPE isolation failed");
#endif
   const auto null_input = ::open("/dev/null", O_RDWR | O_CLOEXEC);
   if (null_input < 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper stderr isolation failed");
   const auto close_null_input = boost::scope::scope_exit{[null_input] { ::close(null_input); }};
   const auto null_fd = duplicate_endpoint(null_input);
   const auto close_null = boost::scope::scope_exit{[null_fd] { ::close(null_fd); }};

   auto actions = posix_spawn_file_actions_t{};
   require_spawn(::posix_spawn_file_actions_init(&actions));
   const auto destroy_actions = boost::scope::scope_exit{[&] { ::posix_spawn_file_actions_destroy(&actions); }};
   require_spawn(::posix_spawn_file_actions_addchdir_np(&actions, workspace.c_str()));
   require_spawn(::posix_spawn_file_actions_adddup2(&actions, child_request, STDIN_FILENO));
   require_spawn(::posix_spawn_file_actions_adddup2(&actions, child_response, STDOUT_FILENO));
   require_spawn(::posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO));
#if defined(__linux__) && defined(FORGE_HAS_POSIX_SPAWN_CLOSEFROM)
   require_spawn(::posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#elif !defined(__APPLE__)
   FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper requires public spawn closefrom support");
#endif
   auto attributes = posix_spawnattr_t{};
   require_spawn(::posix_spawnattr_init(&attributes));
   const auto destroy_attributes = boost::scope::scope_exit{[&] { ::posix_spawnattr_destroy(&attributes); }};
   auto flags = short{POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF};
#if defined(__APPLE__)
   flags = static_cast<short>(flags | POSIX_SPAWN_CLOEXEC_DEFAULT);
#endif
   auto mask = sigset_t{};
   auto defaults = sigset_t{};
   sigemptyset(&mask);
   sigfillset(&defaults);
   require_spawn(::posix_spawnattr_setsigmask(&attributes, &mask));
   require_spawn(::posix_spawnattr_setsigdefault(&attributes, &defaults));
   require_spawn(::posix_spawnattr_setflags(&attributes, flags));

   auto arguments = std::array<std::string, 10>{options.helper.string(),
                                                "--stdio",
                                                std::to_string(exchange.session),
                                                std::to_string(exchange.job),
                                                std::to_string(exchange.attempt),
                                                std::to_string(options.max_request_bytes),
                                                std::to_string(options.max_response_bytes),
                                                std::to_string(options.helper_limits.cpu_seconds),
                                                std::to_string(options.helper_limits.address_space_bytes),
                                                std::to_string(options.helper_limits.stack_bytes)};
   auto argv = std::array<char*, 11>{};
   for (std::size_t i = 0; i < arguments.size(); ++i)
      argv[i] = arguments[i].data();
   auto environment = std::array<char*, 4>{const_cast<char*>("LANG=C"), const_cast<char*>("LC_ALL=C"),
                                           const_cast<char*>("TZ=UTC"), nullptr};
   if (stop.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler helper cancelled at spawn boundary");
   auto spawned = pid_t{-1};
   require_spawn(
       ::posix_spawn(&spawned, options.helper.c_str(), &actions, &attributes, argv.data(), environment.data()));
   // No allocation/throw/cancellation checkpoint after a successful spawn:
   // the pre-existing shared record takes PID+both-fd ownership before return.
   launched_request_endpoint.store(parent_request, std::memory_order_relaxed);
   launched_response_endpoint.store(parent_response, std::memory_order_relaxed);
   close_parent_request.set_active(false);
   close_parent_response.set_active(false);
   launched_pid.store(spawned, std::memory_order_release);
   // Final commit: any actor observing non-quiescence through acquire must
   // already see both owned endpoints and successful PID publication. Cleanup
   // before this commit leaves activation_pending intact; the retained running
   // scheduler/compute operation must resolve launch before shutdown can drain.
   quiescent.store(false, std::memory_order_release);
}

bool compiler_session::impl::child::is_quiescent() const noexcept {
   return process_drained() && !workspace_pending.load(std::memory_order_acquire) && !admission.valid();
}

bool compiler_session::impl::child::process_drained() const noexcept {
   return quiescent.load(std::memory_order_acquire);
}

void compiler_session::impl::child::remove_workspace() {
   if (!process_drained())
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler workspace still has a live child");
   if (!workspace_pending.load(std::memory_order_acquire))
      return;
   auto error = std::error_code{};
   std::filesystem::remove_all(workspace, error);
   if (error)
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler helper workspace cleanup is incomplete",
                            forge::exceptions::ctx("compute_admission", admission.id()));
   workspace.clear();
   workspace_pending.store(false, std::memory_order_release);
}

boost::asio::awaitable<void> compiler_session::impl::child::cleanup_workspace(std::shared_ptr<child> self) {
   if (!self->process_drained())
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler workspace still has a live child");
   if (self->workspace_pending.load(std::memory_order_acquire)) {
      if (!self->admission.valid())
         FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler workspace lost its retained admission");
      co_await self->admission.drain([self] { self->remove_workspace(); });
   }
   // Phase completion has retired its callable/captures. Refusal above keeps
   // this SAME admission for explicit retry, including after pool stop.
   self->admission.release();
}

bool compiler_session::impl::child::drained() const noexcept {
   return !io_inflight && !reap_inflight && timers_inflight == 0;
}

void compiler_session::impl::child::refresh_quiescent() noexcept {
   if (drained() && !activation_pending && !owned && !unresolved && parent_request_endpoint < 0 &&
       parent_response_endpoint < 0) {
      abort_io();
      // No wait is live and physical reap was confirmed. Drop the unmanaged
      // handle/its signal registration before announcing child quiescence;
      // delayed destruction of a scheduler work capture cannot keep it alive
      // into construction of a subsequent cold session.
      process.reset();
      if (!quiescent.exchange(true, std::memory_order_acq_rel))
         changed.notify();
   }
}

void compiler_session::impl::child::abort_io() noexcept {
   boost::system::error_code ignored;
   request_socket.close(ignored);
   response_socket.close(ignored);
}

void compiler_session::impl::child::adopt() {
   if (adopted)
      return; // cleanup and normal activation share one ownership transition
   pid = launched_pid.load(std::memory_order_acquire);
   if (pid <= 0) {
      refresh_quiescent();
      changed.notify();
      return;
   }
   adopted = true;
   owned = true;
   parent_request_endpoint = launched_request_endpoint.exchange(-1, std::memory_order_acq_rel);
   parent_response_endpoint = launched_response_endpoint.exchange(-1, std::memory_order_acq_rel);
   try {
      request_socket.assign(boost::asio::local::stream_protocol{}, parent_request_endpoint);
      parent_request_endpoint = -1;
      response_socket.assign(boost::asio::local::stream_protocol{}, parent_response_endpoint);
      parent_response_endpoint = -1;
   } catch (...) {
      failure = transport_failure(std::current_exception());
      if (parent_request_endpoint >= 0)
         ::close(std::exchange(parent_request_endpoint, -1));
      if (parent_response_endpoint >= 0)
         ::close(std::exchange(parent_response_endpoint, -1));
      abort_io(); // Also retires an already-adopted request fd if response adoption failed.
   }
   start_reap();
}

void compiler_session::impl::child::start_reap() {
   if (!owned || unresolved || reap_inflight)
      return;
   try {
      if (!process)
         process = std::make_unique<boost::process::v2::process_handle>(strand, pid);
      reap_inflight = true;
      process->async_wait(native_status, boost::asio::bind_executor(
                                             strand, [self = shared_from_this()](boost::system::error_code error) {
                                                self->reap_completed(error);
                                             }));
   } catch (...) {
      reap_inflight = false;
      failure = transport_failure(std::current_exception());
      incomplete = true;
      abort_io();
      changed.notify();
   }
}

void compiler_session::impl::child::reap_completed(boost::system::error_code error) {
   reap_inflight = false;
   if (error) {
      if (error.value() == EINTR && owned && boost::process::v2::process_is_running(native_status)) {
         start_reap(); // SAME unmanaged handle; one completed wait, no parallel reaper
         return;
      }
      if (error.value() == ECHILD) {
         // Ownership is unconfirmed. Never kill a potentially reused PID.
         owned = false;
         unresolved = true;
      }
      incomplete = true;
      if (!failure)
         failure = unavailable_error;
      abort_io();
      try {
         static_cast<void>(timer.cancel());
      } catch (...) {
         // Timer cancellation failure cannot escape an IO handler or make
         // outstanding timer callbacks look drained. Retain cleanup state.
         incomplete = true;
         if (!failure)
            failure = unavailable_error;
      }
      refresh_quiescent();
      changed.notify();
      return;
   }
   if (boost::process::v2::process_is_running(native_status) ||
       (!WIFEXITED(native_status) && !WIFSIGNALED(native_status))) {
      incomplete = true;
      if (!failure)
         failure = unavailable_error;
      abort_io();
      refresh_quiescent();
      changed.notify();
      return;
   }
   // This callback and the Boost readiness/reap operation run on the SAME
   // child strand. Clear authority immediately after confirmed reap, before a
   // late timeout/cleanup handler can attempt any signal with the old PID.
   diagnostic_times.reap_ms = diagnostic_elapsed_ms();
   owned = false;
   if (!WIFEXITED(native_status) || WEXITSTATUS(native_status) != 0) {
      if (!failure)
         failure = unavailable_error;
      abort_io();
   }
   try {
      static_cast<void>(timer.cancel());
   } catch (...) {
      incomplete = true;
      if (!failure)
         failure = unavailable_error;
   }
   refresh_quiescent();
   changed.notify();
}

void compiler_session::impl::child::send_signal(int signal) {
   if (!owned || unresolved || pid <= 0)
      return;
   auto result = int{};
   do {
      result = ::kill(pid, signal);
   } while (result < 0 && errno == EINTR);
   if (result < 0 && errno != ESRCH) {
      incomplete = true;
      if (!failure)
         failure = unavailable_error;
      changed.notify();
   }
}

void compiler_session::impl::child::arm_deadline(std::chrono::steady_clock::time_point value) {
   if (timer_generation == UINT64_MAX) {
      incomplete = true;
      failure = unavailable_error;
      changed.notify();
      return;
   }
   const auto current = ++timer_generation;
   auto counted = false;
   try {
      timer.expires_at(value);
      if (!diagnostic_armed_once) {
         diagnostic_armed = std::chrono::steady_clock::now();
         diagnostic_armed_once = true;
      }
      ++timers_inflight;
      counted = true;
      timer.async_wait(
          boost::asio::bind_executor(strand, [self = shared_from_this(), current](boost::system::error_code error) {
             --self->timers_inflight;
             if (!error && current == self->timer_generation) {
                if (!self->failure) {
                   // Freeze FIRST normal helper expiry before abort/kill can
                   // advance IO/reap state. Cleanup escalation is not this
                   // observation. No allocation, formatting or logging here.
                   if (!self->cleanup_requested) {
                      self->expired_phase = self->progress;
                      self->diagnostic_expired = std::chrono::steady_clock::now();
                      self->expired_io_inflight = self->io_inflight;
                      self->expired_reap_inflight = self->reap_inflight;
                      self->expired_owned = self->owned;
                      self->expired_times = self->diagnostic_times;
                      self->expired_pid = self->pid;
                      self->expired_socket_fd =
                          self->response_socket.is_open() ? self->response_socket.native_handle() : -1;
                      self->observe_expired_socket();
                      self->diagnostic_captured = true;
                   }
                   self->failure = self->timeout_error;
                }
                self->abort_io();
                self->send_signal(SIGKILL); // escalation only; async_wait remains sole reap
             }
             self->refresh_quiescent();
             self->changed.notify();
          }));
   } catch (...) {
      if (counted)
         --timers_inflight;
      incomplete = true;
      failure = transport_failure(std::current_exception());
      changed.notify();
   }
}

std::int64_t compiler_session::impl::child::diagnostic_elapsed_ms() const noexcept {
   using clock = std::chrono::steady_clock;
   using rep = clock::duration::rep;
   using scale = std::ratio_divide<clock::period, std::milli>;
   static_assert(noexcept(clock::now()));
   static_assert(std::numeric_limits<rep>::is_integer && std::numeric_limits<rep>::is_signed &&
                 std::numeric_limits<rep>::digits <= std::numeric_limits<std::int64_t>::digits);
   static_assert(scale::num == 1); // Supported steady clocks use integral sub-millisecond ticks.
   const auto started = diagnostic_started.time_since_epoch().count();
   const auto now = clock::now().time_since_epoch().count();
   if (now < started || (started < 0 && now > std::numeric_limits<rep>::max() + started))
      return -2;
   return static_cast<std::int64_t>((now - started) / scale::den);
}

void compiler_session::impl::child::observe_expired_socket() noexcept {
   // SAME strand, before abort_io closes this known socket. One nonconsuming
   // nonblocking observation only; never retry, wait for, or reap the helper.
   const auto saved_errno = errno;
   if (response_socket.is_open()) {
      auto byte = std::uint8_t{};
      const auto count = ::recv(response_socket.native_handle(), &byte, sizeof(byte), MSG_PEEK | MSG_DONTWAIT);
      expired_socket_errno = count < 0 ? errno : 0;
      if (count == 0)
         expired_socket = socket_observation::eof;
      else if (count > 0)
         expired_socket = socket_observation::byte;
      else if (expired_socket_errno == EAGAIN || expired_socket_errno == EWOULDBLOCK)
         expired_socket = socket_observation::would_block;
      else if (expired_socket_errno == EINTR)
         expired_socket = socket_observation::interrupted;
      else
         expired_socket = socket_observation::error;
   }
   errno = saved_errno; // Diagnostic observation must not change its caller.
}

void compiler_session::impl::child::append_deadline_context() noexcept {
   if (!diagnostic_captured || diagnostic_context_attempted || failure != timeout_error)
      return;
   // Called only on the SAME strand after quiescence and stop-callback join,
   // before the failure can be returned/published to a ticket or init waiter.
   diagnostic_context_attempted = true;
   const auto phase = [this] {
      switch (expired_phase) {
      case deadline_phase::before_io:
         return "before_io";
      case deadline_phase::request_write:
         return "request_write";
      case deadline_phase::hello_header:
         return "hello_header";
      case deadline_phase::hello_body:
         return "hello_body";
      case deadline_phase::ack_write:
         return "ack_write";
      case deadline_phase::reply_header:
         return "reply_header";
      case deadline_phase::reply_body:
         return "reply_body";
      case deadline_phase::output_eof:
         return "output_eof";
      case deadline_phase::await_reap:
         return "await_reap";
      }
      return "unknown";
   }();
   const auto socket_state = [this] {
      switch (expired_socket) {
      case socket_observation::not_observed:
         return "not_observed";
      case socket_observation::eof:
         return "eof";
      case socket_observation::byte:
         return "byte";
      case socket_observation::would_block:
         return "would_block";
      case socket_observation::interrupted:
         return "interrupted";
      case socket_observation::error:
         return "error";
      }
      return "not_observed";
   }();
   try {
      std::rethrow_exception(failure);
   } catch (exceptions::compile_limit& error) {
      try {
         using milliseconds = std::chrono::milliseconds;
         const auto arm_ms = std::chrono::duration_cast<milliseconds>(diagnostic_armed - diagnostic_started).count();
         const auto elapsed_ms =
             std::chrono::duration_cast<milliseconds>(diagnostic_expired - diagnostic_started).count();
         error.append_context("compiler helper deadline snapshot",
                              {forge::exceptions::ctx("oc_helper_exchange", preflight ? "preflight" : "compile"),
                               forge::exceptions::ctx("oc_helper_phase", phase),
                               forge::exceptions::ctx("oc_helper_arm_ms", arm_ms),
                               forge::exceptions::ctx("oc_helper_elapsed_ms", elapsed_ms),
                               forge::exceptions::ctx("oc_helper_io", expired_io_inflight),
                               forge::exceptions::ctx("oc_helper_reap", expired_reap_inflight),
                               forge::exceptions::ctx("oc_helper_owned", expired_owned),
                               forge::exceptions::ctx("oc_helper_socket_peek", socket_state),
                               forge::exceptions::ctx("oc_helper_socket_errno", expired_socket_errno),
                               forge::exceptions::ctx("oc_helper_validated_hello_ms", expired_times.validated_hello_ms),
                               forge::exceptions::ctx("oc_helper_ack_shutdown_ms", expired_times.ack_shutdown_ms),
                               forge::exceptions::ctx("oc_helper_eof_ms", expired_times.output_eof_ms),
                               forge::exceptions::ctx("oc_helper_reap_ms", expired_times.reap_ms),
                               forge::exceptions::ctx("oc_helper_pid", expired_pid),
                               forge::exceptions::ctx("oc_helper_socket_fd", expired_socket_fd)});
      } catch (...) {
         // Best-effort metadata must never replace the original exception,
         // code or terminal cleanup result, including allocation failure.
      }
   } catch (...) {
   }
}

std::exception_ptr compiler_session::impl::child::transport_failure(std::exception_ptr error) const noexcept {
   try {
      std::rethrow_exception(error);
   } catch (const forge::exceptions::base& value) {
      const auto& category = exceptions::forge_exceptions_category(exceptions::code::invalid_module);
      if (value.code().category() == category &&
          value.code().value() >= static_cast<int>(exceptions::code::invalid_module) &&
          value.code().value() <= static_cast<int>(exceptions::code::cache_corruption))
         return error; // validated SAME compiler family, including typed reply
   } catch (const std::bad_alloc&) {
      return allocation_error;
   } catch (...) {
   }
   return unavailable_error; // no allocating error construction on terminal IO
}

boost::asio::awaitable<compiler_protocol::header>
compiler_session::impl::child::read_header(std::shared_ptr<child> self, compiler_protocol::message_kind kind) {
   auto bytes = std::vector<std::uint8_t>(compiler_protocol::header_size());
   co_await boost::asio::async_read(self->response_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
   const auto value = compiler_protocol::decode_header(bytes);
   if (value.kind != kind || value.exchange != self->exchange || value.body_bytes > self->options.max_response_bytes ||
       (kind == compiler_protocol::message_kind::hello ? value.identity != crypto::digest::sha256{}
                                                       : value.identity != self->identity))
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper response header mismatch");
   co_return value;
}

boost::asio::awaitable<void> compiler_session::impl::child::exchange_io(std::shared_ptr<child> self,
                                                                        std::vector<std::uint8_t> request) {
   auto header = compiler_protocol::header{};
   header.kind =
       self->preflight ? compiler_protocol::message_kind::preflight : compiler_protocol::message_kind::request;
   header.exchange = self->exchange;
   header.identity = self->identity;
   header.body_bytes = request.size();
   auto bytes = compiler_protocol::encode(header);
   self->progress = deadline_phase::request_write;
   co_await boost::asio::async_write(self->request_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
   co_await boost::asio::async_write(self->request_socket, boost::asio::buffer(request), boost::asio::use_awaitable);
   self->progress = deadline_phase::hello_header;
   const auto hello_header = co_await read_header(self, compiler_protocol::message_kind::hello);
   bytes.resize(static_cast<std::size_t>(hello_header.body_bytes));
   self->progress = deadline_phase::hello_body;
   co_await boost::asio::async_read(self->response_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
   const auto actual = compiler_protocol::decode_hello(bytes, self->options.max_response_bytes);
   if (actual != self->expected)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable,
                            "compiler helper build/target/resource handshake mismatch");
   self->diagnostic_times.validated_hello_ms = self->diagnostic_elapsed_ms();
   // No LLVM work may begin until this exact SAME Raw ack and subsequent EOF.
   header.kind = compiler_protocol::message_kind::hello_ack;
   header.body_bytes = 0;
   bytes = compiler_protocol::encode(header);
   self->progress = deadline_phase::ack_write;
   co_await boost::asio::async_write(self->request_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
   // Retire the only parent writer, rather than relying on a half-close wake.
   // Keep the historical diagnostic key: ACK plus request-writer full close.
   self->request_socket.close();
   self->diagnostic_times.ack_shutdown_ms = self->diagnostic_elapsed_ms();
   if (self->preflight) {
      auto extra = std::array<std::uint8_t, 1>{};
      auto error = boost::system::error_code{};
      self->progress = deadline_phase::output_eof;
      const auto count = co_await self->response_socket.async_read_some(
          boost::asio::buffer(extra), boost::asio::redirect_error(boost::asio::use_awaitable, error));
      if (error != boost::asio::error::eof || count != 0)
         FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler preflight requires clean output EOF");
      self->diagnostic_times.output_eof_ms = self->diagnostic_elapsed_ms();
      self->progress = deadline_phase::await_reap;
      co_return; // run_owned STILL waits for confirmed successful sole reap/drains.
   }
   self->progress = deadline_phase::reply_header;
   const auto reply_header = co_await read_header(self, compiler_protocol::message_kind::reply);
   bytes.resize(static_cast<std::size_t>(reply_header.body_bytes));
   self->progress = deadline_phase::reply_body;
   co_await boost::asio::async_read(self->response_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
   auto reply = compiler_protocol::decode_reply(bytes, self->options.max_response_bytes);
   auto extra = std::array<std::uint8_t, 1>{};
   auto error = boost::system::error_code{};
   self->progress = deadline_phase::output_eof;
   const auto count = co_await self->response_socket.async_read_some(
       boost::asio::buffer(extra), boost::asio::redirect_error(boost::asio::use_awaitable, error));
   if (error != boost::asio::error::eof || count != 0)
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper response has trailing bytes or no EOF");
   self->diagnostic_times.output_eof_ms = self->diagnostic_elapsed_ms();
   if (reply.failure)
      FORGE_THROW_CODE(*reply.failure, "isolated compiler rejected the request");
   self->output = std::move(reply.image);
   self->progress = deadline_phase::await_reap;
}

boost::asio::awaitable<std::vector<std::uint8_t>>
compiler_session::impl::child::run_owned(std::shared_ptr<child> self, std::vector<std::uint8_t> request,
                                         std::stop_token stop) {
   self->adopt();
   if (self->pid <= 0) {
      self->activation_pending = false;
      self->quiescent.store(true, std::memory_order_release);
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "compiler helper was not launched");
   }
   if (self->process_drained()) {
      // A cleanup actor can finish before normal activation is scheduled. Do
      // not rearm IO, re-adopt a reaped PID, or report an empty successful reply.
      if (self->failure)
         std::rethrow_exception(self->failure);
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "compiler helper was drained before activation");
   }
   // The existing notification owner makes cancellation wake-before-wait
   // sticky. The callback does not allocate/post or wait for the child strand.
   self->stop_registration = std::make_unique<std::stop_callback<std::function<void()>>>(
       stop, [weak = std::weak_ptr<child>{self}]() noexcept {
          if (const auto owner = weak.lock()) {
             owner->stop_requested.store(true, std::memory_order_release);
             owner->changed.notify();
          }
       });
   if (!self->cleanup_requested || self->timers_inflight == 0)
      self->arm_deadline(self->deadline);
   if (!self->cleanup_requested && !self->failure && self->request_socket.is_open() &&
       self->response_socket.is_open()) {
      self->io_inflight = true;
      try {
         boost::asio::co_spawn(self->strand, exchange_io(self, std::move(request)),
                               boost::asio::bind_executor(self->strand, [self](std::exception_ptr error) {
                                  self->io_inflight = false;
                                  if (error && !self->failure) {
                                     self->failure = self->transport_failure(std::move(error));
                                     self->abort_io();
                                     self->send_signal(SIGKILL);
                                  }
                                  self->refresh_quiescent();
                                  self->changed.notify();
                               }));
      } catch (...) {
         self->io_inflight = false;
         self->failure = self->transport_failure(std::current_exception());
         self->abort_io();
         self->send_signal(SIGKILL);
      }
   }
   self->activation_pending = false;
   for (;;) {
      const auto observed = self->changed.epoch();
      if (self->stop_requested.load(std::memory_order_acquire) && !self->term_sent) {
         self->term_sent = true;
         if (!self->failure)
            self->failure = self->cancelled_error;
         self->abort_io();
         self->send_signal(SIGTERM);
      }
      if (self->incomplete)
         FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler helper ownership/reap is unresolved");
      self->refresh_quiescent();
      if (self->process_drained()) {
         self->stop_registration.reset(); // actual unregister/join on host, after all actor drains
         if (self->failure) {
            self->append_deadline_context();
            std::rethrow_exception(self->failure);
         }
         co_return std::move(self->output);
      }
      co_await self->changed.async_wait(observed);
   }
}

boost::asio::awaitable<void>
compiler_session::impl::child::cleanup_owned(std::shared_ptr<child> self,
                                             std::chrono::steady_clock::time_point escalation) {
   // Publish escalation even if spawn is still completing on compute. A later
   // actor activation uses this SAME shortened deadline, not its old timeout.
   self->deadline = std::min(escalation, self->deadline);
   self->cleanup_requested = true;
   if (!self->failure)
      self->failure = self->cancelled_error;
   self->stop_requested.store(true, std::memory_order_release);
   self->changed.notify();
   if (self->process_drained())
      co_return;
   if (self->unresolved)
      FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler helper ownership is unconfirmed");
   self->incomplete = false;
   if (self->pid <= 0 && self->launched_pid.load(std::memory_order_acquire) > 0)
      self->adopt();
   self->activation_pending = false;
   self->abort_io();
   self->send_signal(SIGTERM);
   self->arm_deadline(self->deadline);
   self->start_reap(); // retry adoption only if no live wait, never a second reaper
   self->changed.notify();
   for (;;) {
      const auto observed = self->changed.epoch();
      if (self->incomplete)
         FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "compiler helper cleanup is incomplete");
      self->refresh_quiescent();
      if (self->process_drained()) {
         self->stop_registration.reset();
         co_return;
      }
      co_await self->changed.async_wait(observed);
   }
}

} // namespace forge::vm::wasm::optimizing
