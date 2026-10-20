module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/process/v2/process_handle.hpp>
#include <boost/system/system_error.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <exception>
#include <fcntl.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <spawn.h>
#include <stop_token>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>
#include <forge/log/macros.hpp>

module forge.net.tlsnotary.engine;

import forge.exceptions;
import forge.log.logger;
import forge.net.transport.stream;
import forge.net.transport.exceptions;
import forge.asio.gate;
import forge.asio.notification;
#include "details/native_exchange.hxx"
#include "details/wire.hxx"

namespace forge::net::tlsnotary::detail {

namespace {

void log_channel_failure(std::uint64_t session, bool prover, bool server, const char* operation) noexcept {
   auto kind = "unknown";
   auto category = "unknown";
   auto code = 0;
   auto file = "";
   auto line = std::uint_least32_t{};
   try {
      throw;
   } catch (const forge::exceptions::base& error) {
      kind = "forge";
      category = error.code().category().name();
      code = error.code().value();
      file = error.location().file_name();
      line = error.location().line();
   } catch (const boost::system::system_error& error) {
      kind = "boost";
      category = error.code().category().name();
      code = error.code().value();
   } catch (const std::system_error& error) {
      kind = "std";
      category = error.code().category().name();
      code = error.code().value();
   } catch (...) {
   }

   wlog("forge.net.tlsnotary", "TLSNotary raw channel operation failed",
        ("event", "tlsnotary.channel_failure")("session", session)("role", prover ? "prover" : "verifier")(
            "channel", server ? "server" : "peer")("operation", operation)("cause_kind", kind)(
            "cause_category", category)("cause_code", code)("cause_file", file)("cause_line", line));
}

void spawn_ok(int value) {
   if (value != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native worker spawn setup failed");
}

int duplicate(int value) {
   auto fd = ::fcntl(value, F_DUPFD_CLOEXEC, 3);
   if (fd < 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native worker descriptor isolation failed");
   return fd;
}

exceptions::code status(std::uint32_t code) {
   switch (code) {
   case 1:
      return exceptions::code::invalid_request;
   case 2:
      return exceptions::code::transport_failed;
   case 3:
   case 8:
      return exceptions::code::verification_failed;
   case 5:
      return exceptions::code::cancelled;
   case 6:
      return exceptions::code::deadline_exceeded;
   case 7:
      return exceptions::code::limit_exceeded;
   case 9:
      return exceptions::code::protocol_error;
   default:
      return exceptions::code::worker_failed;
   }
}
} // namespace
native_exchange::native_exchange(forge::asio::runtime& runtime, forge::asio::compute::executor compute,
                                 std::filesystem::path worker, prover_request request,
                                 std::chrono::steady_clock::time_point deadline)
    : _strand{boost::asio::make_strand(runtime.context())}, _socket{_strand}, _timer{_strand},
      _compute{std::move(compute)}, _worker{std::move(worker)}, _peer{std::move(request.peer)},
      _server{std::move(request.server)}, _source{std::move(request.source)}, _options{std::move(request.options)},
      _request{std::move(request.request_bytes)}, _prover{true} {
   _timer.expires_at(deadline);
}

native_exchange::native_exchange(forge::asio::runtime& runtime, forge::asio::compute::executor compute,
                                 std::filesystem::path worker, verifier_request request,
                                 std::chrono::steady_clock::time_point deadline)
    : _strand{boost::asio::make_strand(runtime.context())}, _socket{_strand}, _timer{_strand},
      _compute{std::move(compute)}, _worker{std::move(worker)}, _peer{std::move(request.peer)},
      _source{std::move(request.source)}, _options{std::move(request.options)} {
   _timer.expires_at(deadline);
}

native_exchange::~native_exchange() {
   if (const auto fd = _endpoint.exchange(-1); fd >= 0)
      ::close(fd);
}

void native_exchange::kill_owned() noexcept {
   // This and the Boost reap completion run on the same strand.
   if (_pid > 0)
      ::kill(_pid, SIGKILL);
}

void native_exchange::decide_handoff(bool started) noexcept {
   {
      const auto lock = std::scoped_lock{_handoff_mutex};
      _handoff_decided = true;
      _reaper_started = started;
   }

   _handoff_ready.notify_all();
}

void native_exchange::prepare() {
   try {
      spawn();
   } catch (...) {
      const auto lock = std::scoped_lock{_handoff_mutex};
      _setup_error = std::current_exception();
   }

   {
      const auto lock = std::scoped_lock{_handoff_mutex};
      _spawn_ready = true;
   }

   _changed.notify();
   const auto pid = _launched_pid.load();
   if (pid <= 0)
      return;
   auto lock = std::unique_lock{_handoff_mutex};
   _handoff_ready.wait(lock, [&] { return _handoff_decided; });
   const auto started = _reaper_started;
   lock.unlock();
   if (!started) {
      // Rollback stays in the original spawn reservation; no new admission is
      // required even when construction of the asynchronous reaper failed.
      ::kill(pid, SIGKILL);
      auto waited = pid_t{};
      do {
         waited = ::waitpid(pid, nullptr, 0);
      } while (waited < 0 && errno == EINTR);
      if (waited == pid || (waited < 0 && errno == ECHILD)) {
         _launched_pid.store(-1);
         _ownership_quiescent.store(true);
      }

      if (waited != pid)
         FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native worker setup rollback failed");
   }
}

boost::asio::awaitable<void> native_exchange::watch_worker() {
   _watching_worker = true;
   try {
      co_await std::move(*_spawn_operation).wait();
   } catch (...) {
      _worker_error = std::current_exception();
   }

   {
      const auto lock = std::scoped_lock{_handoff_mutex};
      if (!_spawn_ready) {
         _spawn_ready = true;
         _setup_error = _worker_error;
      }
   }

   _worker_done = true;
   _changed.notify();
}

void native_exchange::adopt() {
   if (_pid <= 0)
      _pid = _launched_pid.load();
   if (_pid <= 0 || _reap_pending)
      return;
   try {
      if (!_process)
         _process = std::make_unique<boost::process::v2::process_handle>(_strand, _pid);
      _native_status = W_STOPCODE(SIGSTOP);
      _reap_pending = true;
      _process->async_wait(
          _native_status,
          boost::asio::bind_executor(_strand, [self = shared_from_this()](boost::system::error_code error) {
             self->_reap_pending = false;
             if (error.value() == EINTR && boost::process::v2::process_is_running(self->_native_status)) {
                try {
                   self->adopt();
                } catch (...) {
                   self->_error = std::make_exception_ptr(exceptions::worker_failed{"native reaper restart failed"});
                   self->kill_owned();
                   self->_changed.notify();
                }

                return;
             }

             const auto terminal = !error && (WIFEXITED(self->_native_status) || WIFSIGNALED(self->_native_status));
             // Unknown/external reap forfeits PID authority; never signal reuse.
             if (terminal || error.value() == ECHILD) {
                self->_pid = -1;
                self->_launched_pid.store(-1);
                self->_ownership_quiescent.store(true);
             }

             if (terminal && !self->_expired && !self->_stopped.load() &&
                 ((WIFEXITED(self->_native_status) && WEXITSTATUS(self->_native_status) != 0) ||
                  (!self->_finishing && WIFSIGNALED(self->_native_status))) &&
                 !self->_error)
                self->_error = std::make_exception_ptr(exceptions::worker_failed{"native worker exited unexpectedly"});
             if (!terminal && !self->_error)
                self->_error = std::make_exception_ptr(exceptions::worker_failed{"native worker reap failed"});
             self->_changed.notify();
          }));
   } catch (...) {
      _reap_pending = false;
      throw;
   }

   if (_abort.load())
      kill_owned();
}

void native_exchange::spawn() {
   if (_abort.load())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "native launch cancelled");
   struct sigaction disposition{};
   if (::sigaction(SIGCHLD, nullptr, &disposition) != 0 || disposition.sa_handler == SIG_IGN ||
       (disposition.sa_flags & SA_NOCLDWAIT) != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native worker requires exclusive child reap authority");
   auto endpoints = std::array<int, 2>{-1, -1};
   if (::socketpair(AF_UNIX, SOCK_STREAM, 0, endpoints.data()) != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native IPC unavailable");
   const auto close_pair = boost::scope::scope_exit{[&] {
      for (auto fd : endpoints)
         if (fd >= 0)
            ::close(fd);
   }};
   const auto parent_fd = duplicate(endpoints[0]);
   auto close_parent = boost::scope::scope_exit{[&] { ::close(parent_fd); }};
   const auto child_fd = duplicate(endpoints[1]);
   const auto close_child = boost::scope::scope_exit{[&] { ::close(child_fd); }};
   const auto null_input = ::open("/dev/null", O_RDWR | O_CLOEXEC);
   if (null_input < 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native stderr isolation failed");
   const auto close_null_input = boost::scope::scope_exit{[&] { ::close(null_input); }};
   const auto null_fd = duplicate(null_input);
   const auto close_null = boost::scope::scope_exit{[&] { ::close(null_fd); }};
#if defined(__APPLE__)
   auto no_sigpipe = 1;
   if (::setsockopt(parent_fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe)) != 0)
      FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native IPC signal isolation failed");
#endif
   auto actions = posix_spawn_file_actions_t{};
   spawn_ok(::posix_spawn_file_actions_init(&actions));
   const auto destroy_actions = boost::scope::scope_exit{[&] { ::posix_spawn_file_actions_destroy(&actions); }};
   spawn_ok(::posix_spawn_file_actions_adddup2(&actions, child_fd, STDIN_FILENO));
   spawn_ok(::posix_spawn_file_actions_adddup2(&actions, child_fd, STDOUT_FILENO));
   spawn_ok(::posix_spawn_file_actions_adddup2(&actions, null_fd, STDERR_FILENO));
   auto attributes = posix_spawnattr_t{};
   spawn_ok(::posix_spawnattr_init(&attributes));
   const auto destroy_attributes = boost::scope::scope_exit{[&] { ::posix_spawnattr_destroy(&attributes); }};
   auto mask = sigset_t{};
   auto defaults = sigset_t{};
   sigemptyset(&mask);
   sigfillset(&defaults);
   spawn_ok(::posix_spawnattr_setsigmask(&attributes, &mask));
   spawn_ok(::posix_spawnattr_setsigdefault(&attributes, &defaults));
#if defined(__APPLE__)
   spawn_ok(::posix_spawnattr_setflags(&attributes,
                                       POSIX_SPAWN_CLOEXEC_DEFAULT | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF));
#else
   FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native session platform is not qualified");
#endif
   auto path = _worker.string();
   auto argv = std::array<char*, 2>{path.data(), nullptr};
   auto environment = std::array<char*, 1>{nullptr};
   auto pid = pid_t{};
   spawn_ok(::posix_spawn(&pid, path.c_str(), &actions, &attributes, argv.data(), environment.data()));
   // Publish PID and IPC before returning; cancellation never loses ownership.
   _ownership_quiescent.store(false);
   _launched_pid.store(pid);
   _endpoint.store(parent_fd);
   close_parent.set_active(false);
}

std::chrono::steady_clock::time_point native_exchange::deadline() const noexcept {
   return _timer.expiry();
}

bool native_exchange::quiescent() const noexcept {
   return _ownership_quiescent.load();
}

void native_exchange::request_stop() noexcept {
   if (_terminal.load())
      return;
   _stopped.store(true);
   _abort.store(true);
   _peer.request_cancel();
   _server.request_cancel();
   try {
      boost::asio::post(_strand, [self = shared_from_this()] {
         if (!self->_terminal.load())
            self->stop_on_strand();
      });
   } catch (...) {
   }
}

void native_exchange::close_ipc_on_strand() noexcept {
   auto error = boost::system::error_code{};
   _socket.cancel(error);
   _socket.close(error);
   _writer.close();
}

void native_exchange::stop_on_strand() noexcept {
   _abort.store(true);
   close_ipc_on_strand();
   _peer.request_cancel();
   _server.request_cancel();
   kill_owned();
}

std::vector<std::uint8_t> native_exchange::bootstrap() const {
   auto bytes = std::vector<std::uint8_t>{};
   wire::put(bytes, 0x46544e31, 4);
   wire::put(bytes, _prover ? 1 : 2, 1);
   wire::put(bytes, _options.deferred_decryption ? 1 : 0, 1);
   wire::put(bytes, 0, 2);
   auto remaining =
       std::chrono::duration_cast<std::chrono::milliseconds>(_timer.expiry() - std::chrono::steady_clock::now())
           .count();
   wire::put(bytes, std::max<std::int64_t>(remaining, 1), 8);
   for (auto value : {_options.max_sent, _options.max_received, _options.max_sent_records,
                      _options.max_received_records_online, _options.mpc_threads})
      wire::put(bytes, value, 4);
   wire::put(bytes, _options.max_peer_bytes, 8);
   wire::blob(bytes, {reinterpret_cast<const std::uint8_t*>(_source.server_name.data()), _source.server_name.size()});
   wire::put(bytes, _source.trust_roots_der.size(), 4);
   for (const auto& root : _source.trust_roots_der)
      wire::blob(bytes, root);
   wire::blob(bytes, _request);
   for (const auto* ranges : {&_options.reveal_sent, &_options.reveal_received}) {
      wire::put(bytes, ranges->size(), 4);
      for (const auto& range : *ranges) {
         wire::put(bytes, range.begin, 4);
         wire::put(bytes, range.end, 4);
      }
   }

   return bytes;
}

boost::asio::awaitable<void> native_exchange::send(std::uint8_t kind, std::vector<std::uint8_t> bytes) {
   auto ticket = co_await _writer.acquire();
   auto header = std::vector<std::uint8_t>{kind};
   wire::put(header, bytes.size(), 4);
   co_await boost::asio::async_write(_socket, boost::asio::buffer(header), boost::asio::use_awaitable);
   if (!bytes.empty())
      co_await boost::asio::async_write(_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
}

void native_exchange::launch(boost::asio::awaitable<void> operation) {
   if (_pumps >= 16)
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "native channel operation limit exceeded");
   ++_pumps;
   try {
      boost::asio::co_spawn(_strand, std::move(operation), [self = shared_from_this()](std::exception_ptr error) {
         if (error && !self->_finishing && !self->_error) {
            self->_error = error;
            self->stop_on_strand();
         }

         --self->_pumps;
         self->_changed.notify();
      });
   } catch (...) {
      --_pumps;
      throw;
   }
}

boost::asio::awaitable<void> native_exchange::read_channel(bool server, std::uint32_t credit) {
   auto index = server ? 1 : 0;
   auto& stream = server ? _server : _peer;
   auto& pending = _pending[index];
   if (pending.empty()) {
      try {
         pending = co_await stream.async_read();
      } catch (const forge::net::transport::exceptions::end_of_stream&) {
         pending.clear();
      } catch (...) {
         log_channel_failure(_options.id, _prover, server, "read");
         FORGE_THROW_EXCEPTION(exceptions::transport_failed, "TLSNotary raw stream read failed");
      }

      if (pending.capacity() > 65536 || pending.size() > _options.max_peer_bytes - _incoming[index]) {
         std::vector<std::uint8_t>{}.swap(pending);
         FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "raw stream read exceeds session wire limit");
      }

      _incoming[index] += pending.size();
   }

   _reading[index] = false;
   if (pending.empty()) {
      _read_eof[index] = true;
      co_await send(server ? 4 : 3, {});
   } else {
      const auto count = std::min<std::size_t>(credit, pending.size());
      auto bytes = std::vector<std::uint8_t>{pending.begin(), pending.begin() + count};
      pending.erase(pending.begin(), pending.begin() + count);
      co_await send(server ? 2 : 1, std::move(bytes));
   }
}

boost::asio::awaitable<void> native_exchange::write_channel(bool server, std::vector<std::uint8_t> bytes, bool eof) {
   auto index = server ? 1 : 0;
   auto& stream = server ? _server : _peer;
   if (bytes.size() > _options.max_peer_bytes - _outgoing[index])
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "raw stream write exceeds session wire limit");
   _outgoing[index] += bytes.size();
   try {
      if (eof)
         co_await stream.async_close_write();
      else
         co_await stream.async_write(bytes);
   } catch (...) {
      log_channel_failure(_options.id, _prover, server, eof ? "close_write" : "write");
      FORGE_THROW_EXCEPTION(exceptions::transport_failed, "TLSNotary raw stream write failed");
   }

   _writing[index] = false;
   co_await send(server ? 7 : 6, {});
}

result native_exchange::decode(std::span<const std::uint8_t> payload) const {
   auto input = wire{payload};
   const auto native_code = input.take(4);
   if (native_code != 0)
      FORGE_THROW_CODE(status(native_code), "native TLSNotary session failed");
   if (input.take(1) != (_prover ? 0 : 1))
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "native authentication role mismatch");
   auto value = result{};
   const auto server = input.blob(253);
   value.server_name.assign(reinterpret_cast<const char*>(server.data()), server.size());
   value.sent_size = input.take(4);
   value.received_size = input.take(4);
   if (value.server_name != _source.server_name || value.sent_size > _options.max_sent ||
       value.received_size > _options.max_received)
      FORGE_THROW_EXCEPTION(exceptions::verification_failed, "native source or transcript bounds mismatch");
   for (auto received : {false, true}) {
      auto& output = received ? value.received : value.sent;
      const auto total = received ? value.received_size : value.sent_size;
      const auto& expected = received ? _options.reveal_received : _options.reveal_sent;
      auto count = input.take(4);
      if (count > 1024 || (_prover && count != 0) || (!_prover && count != expected.size()))
         FORGE_THROW_EXCEPTION(exceptions::verification_failed, "native disclosure mask mismatch");
      for (std::size_t n = 0; n < count; ++n) {
         auto begin = input.take(4);
         auto bytes = input.blob(total);
         auto end = expected[n].end == 0xffffffff ? total : expected[n].end;
         if (begin != expected[n].begin || end > total || end < begin || bytes.size() != end - begin)
            FORGE_THROW_EXCEPTION(exceptions::verification_failed, "native authenticated range mismatch");
         output.push_back({static_cast<std::uint32_t>(begin), {bytes.begin(), bytes.end()}});
      }
   }

   input.finish();
   return value;
}

boost::asio::awaitable<void> native_exchange::discard(std::shared_ptr<native_exchange> self) {
   auto close = [](std::shared_ptr<native_exchange> state) -> boost::asio::awaitable<void> {
      state->_finishing = true;
      state->stop_on_strand();
      state->_timer.cancel();
      while (state->_pumps != 0 || state->_timer_pending || state->_reap_pending || state->_pid > 0) {
         if (state->_pid > 0 && !state->_reap_pending) {
            try {
               state->adopt();
            } catch (...) {
               FORGE_THROW_EXCEPTION(exceptions::worker_failed, "native child cleanup remains incomplete");
            }
         }

         const auto epoch = state->_changed.epoch();
         if (state->_pumps != 0 || state->_timer_pending || state->_reap_pending)
            static_cast<void>(co_await state->_changed.async_wait(epoch));
      }

      state->_process.reset();
      try {
         co_await state->_peer.async_close();
      } catch (...) {
      }

      if (state->_prover) {
         try {
            co_await state->_server.async_close();
         } catch (...) {
         }
      }

      state->_terminal.store(true);
   };
   co_await boost::asio::co_spawn(self->_strand, close(self), boost::asio::use_awaitable);
}

boost::asio::awaitable<result> native_exchange::run(std::shared_ptr<native_exchange> self) {
   auto operation = [](std::shared_ptr<native_exchange> state) -> boost::asio::awaitable<result> {
      auto result_value = result{};
      auto failure = std::exception_ptr{};
      try {
         if (state->_stopped.load())
            FORGE_THROW_EXCEPTION(exceptions::cancelled, "native session cancelled before launch");
         state->_timer_pending = true;
         try {
            state->_timer.async_wait(boost::asio::bind_executor(state->_strand, [state](auto error) {
               if (!error) {
                  state->_expired = true;
                  state->stop_on_strand();
               }

               state->_timer_pending = false;
               state->_changed.notify();
            }));
         } catch (...) {
            state->_timer_pending = false;
            throw;
         }

         // Reserve once. Compute retains rollback ownership only until the
         // asynchronous reaper has been installed on this strand.
         auto worker = state->_compute.try_submit({.name = "tlsnotary-spawn"}, [state] { state->prepare(); });
         if (!worker)
            FORGE_THROW_EXCEPTION(exceptions::overloaded, "TLSNotary compute admission exhausted");
         state->_spawn_operation = std::move(worker);
         try {
            state->launch(state->watch_worker());
         } catch (...) {
            state->decide_handoff(false);
            throw;
         }

         for (;;) {
            const auto epoch = state->_changed.epoch();
            auto ready = false;
            {
               const auto lock = std::scoped_lock{state->_handoff_mutex};
               ready = state->_spawn_ready;
            }

            if (ready)
               break;
            static_cast<void>(co_await state->_changed.async_wait(epoch));
         }

         auto setup_error = std::exception_ptr{};
         {
            const auto lock = std::scoped_lock{state->_handoff_mutex};
            setup_error = state->_setup_error;
         }

         if (!setup_error) {
            try {
               state->adopt();
            } catch (...) {
               setup_error = std::current_exception();
               state->_pid = -1;
            }
         }

         state->decide_handoff(!setup_error);
         while (!state->_worker_done) {
            auto epoch = state->_changed.epoch();
            if (!state->_worker_done)
               static_cast<void>(co_await state->_changed.async_wait(epoch));
         }

         if (!setup_error)
            setup_error = state->_worker_error;
         if (setup_error) {
            if (state->_pid < 0 && state->_launched_pid.load() > 0)
               state->_pid = state->_launched_pid.load();
            std::rethrow_exception(setup_error);
         }

         const auto endpoint = state->_endpoint.exchange(-1);
         auto close_endpoint = boost::scope::scope_exit{[endpoint] {
            if (endpoint >= 0)
               ::close(endpoint);
         }};
         state->_socket.assign(boost::asio::local::stream_protocol{}, endpoint);
         close_endpoint.set_active(false);
         co_await state->send(0, state->bootstrap());
         for (;;) {
            auto header = std::array<std::uint8_t, 5>{};
            co_await boost::asio::async_read(state->_socket, boost::asio::buffer(header), boost::asio::use_awaitable);
            const auto kind = header[0];
            auto decoder = wire{std::span<const std::uint8_t>{header}.subspan(1)};
            const auto size = decoder.take(4);
            if (size > (kind == 128 ? 2097152 : 65536))
               FORGE_THROW_EXCEPTION(exceptions::protocol_error, "native frame exceeds limit");
            auto bytes = std::vector<std::uint8_t>(size);
            if (size)
               co_await boost::asio::async_read(state->_socket, boost::asio::buffer(bytes), boost::asio::use_awaitable);
            if (kind == 128) {
               result_value = state->decode(bytes);
               break;
            }

            const auto server = kind == 2 || kind == 4 || kind == 18;
            const auto index = server ? 1 : 0;
            if (server && !state->_prover)
               FORGE_THROW_EXCEPTION(exceptions::protocol_error, "verifier worker requested a server stream");
            if (kind == 17 || kind == 18) {
               if (bytes.size() != 4 || state->_reading[index] || state->_read_eof[index])
                  FORGE_THROW_EXCEPTION(exceptions::protocol_error, "invalid native read credit");
               auto credit_decoder = wire{bytes};
               auto credit = credit_decoder.take(4);
               if (credit == 0 || credit > 16384)
                  FORGE_THROW_EXCEPTION(exceptions::protocol_error, "invalid native read size");
               state->_reading[index] = true;
               state->launch(state->read_channel(server, credit));
            } else if (kind >= 1 && kind <= 4) {
               auto eof = kind == 3 || kind == 4;
               if (state->_writing[index] || state->_write_eof[index] || (eof && !bytes.empty()))
                  FORGE_THROW_EXCEPTION(exceptions::protocol_error, "invalid native write frame");
               state->_writing[index] = true;
               if (eof)
                  state->_write_eof[index] = true;
               state->launch(state->write_channel(server, std::move(bytes), eof));
            } else
               FORGE_THROW_EXCEPTION(exceptions::protocol_error, "unknown native frame kind");
         }
      } catch (...) {
         failure = std::current_exception();
      }

      state->_finishing = true;
      state->decide_handoff(false);
      if (state->_spawn_operation && !state->_watching_worker) {
         try {
            co_await std::move(*state->_spawn_operation).wait();
         } catch (...) {
         }
      }

      if (!failure && !state->_error && !state->_expired && !state->_stopped.load()) {
         // The worker publishes success only after its existing bridge writes
         // and FIN acknowledgements have drained. Preserve graceful transport
         // close: terminal cancellation would reset QUIC and discard its queue.
         state->close_ipc_on_strand();
      } else {
         state->_timer.cancel();
         state->stop_on_strand();
      }
      while (state->_pumps != 0 || state->_reap_pending || state->_pid > 0) {
         if (state->_pid > 0 && !state->_reap_pending) {
            try {
               state->adopt();
            } catch (...) {
               failure = std::make_exception_ptr(exceptions::worker_failed{"native child cleanup remains incomplete"});
               break;
            }
         }

         auto epoch = state->_changed.epoch();
         if (state->_pumps != 0 || state->_reap_pending)
            static_cast<void>(co_await state->_changed.async_wait(epoch));
      }

      // Both stream cleanup barriers run before a session leaves admission accounting.
      try {
         co_await state->_peer.async_close();
      } catch (...) {
         if (!failure)
            failure = std::make_exception_ptr(exceptions::transport_failed{"TLSNotary peer cleanup failed"});
      }

      if (state->_prover) {
         try {
            co_await state->_server.async_close();
         } catch (...) {
            if (!failure)
               failure = std::make_exception_ptr(exceptions::transport_failed{"TLSNotary server cleanup failed"});
         }
      }

      state->_timer.cancel();
      while (state->_timer_pending) {
         const auto epoch = state->_changed.epoch();
         if (state->_timer_pending)
            static_cast<void>(co_await state->_changed.async_wait(epoch));
      }

      if (state->quiescent()) {
         state->_process.reset();
         state->_terminal.store(true);
      }

      if (state->_expired)
         FORGE_THROW_EXCEPTION(exceptions::deadline_exceeded, "TLSNotary session deadline exceeded");
      if (state->_stopped.load())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "TLSNotary session cancelled");
      if (state->_error)
         failure = state->_error;
      if (failure) {
         try {
            std::rethrow_exception(failure);
         } catch (const forge::exceptions::runtime_coded_exception<exceptions::code>&) {
            throw;
         } catch (const exceptions::invalid_request&) {
            throw;
         } catch (const exceptions::overloaded&) {
            throw;
         } catch (const exceptions::cancelled&) {
            throw;
         } catch (const exceptions::deadline_exceeded&) {
            throw;
         } catch (const exceptions::worker_failed&) {
            throw;
         } catch (const exceptions::protocol_error&) {
            throw;
         } catch (const exceptions::verification_failed&) {
            throw;
         } catch (const exceptions::limit_exceeded&) {
            throw;
         } catch (const exceptions::transport_failed&) {
            throw;
         } catch (...) {
            FORGE_THROW_EXCEPTION(exceptions::transport_failed, "TLSNotary stream or worker I/O failed");
         }
      }

      co_return result_value;
   };
   co_return co_await boost::asio::co_spawn(self->_strand, operation(self), boost::asio::use_awaitable);
}
} // namespace forge::net::tlsnotary::detail
