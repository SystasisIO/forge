module;

#include <boost/test/unit_test.hpp>

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <limits>
#include <mutex>
#include <new>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.net.yamux.session;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.net.transport.buffer;
import forge.net.transport.exceptions;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;
import forge.net.yamux.options;

#include "../../libraries/net/yamux/details/session_close_failure.hxx"
#include "../../libraries/net/yamux/details/session_start_failure.hxx"
#include "../../libraries/net/yamux/details/stream_close_failure.hxx"

namespace {

using bytes = std::vector<std::uint8_t>;

enum class frame_type : std::uint8_t {
   data = 0,
   window_update = 1,
   ping = 2,
   go_away = 3,
};

std::ostream& operator<<(std::ostream& out, frame_type value) {
   return out << static_cast<int>(value);
}

inline constexpr std::uint16_t syn = 0x01;
inline constexpr std::uint16_t ack = 0x02;
inline constexpr std::uint16_t fin = 0x04;
inline constexpr std::uint16_t rst = 0x08;
inline constexpr std::size_t header_size = 12;
inline constexpr std::uint32_t initial_stream_window = 256U * 1024U;

[[nodiscard]] bytes deterministic_bytes(std::size_t size, std::uint8_t seed = 0x31) {
   auto out = bytes(size);
   for (auto index = std::size_t{0}; index < out.size(); ++index) {
      out[index] = static_cast<std::uint8_t>(seed + (index % 23U));
   }
   return out;
}

[[nodiscard]] bytes text_bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] bytes frame(frame_type type, std::uint16_t flags, std::uint32_t stream_id, std::uint32_t length,
                          std::span<const std::uint8_t> payload = {}) {
   auto out = bytes{};
   out.reserve(header_size + payload.size());
   out.push_back(0);
   out.push_back(static_cast<std::uint8_t>(type));
   out.push_back(static_cast<std::uint8_t>((flags >> 8U) & 0xffU));
   out.push_back(static_cast<std::uint8_t>(flags & 0xffU));
   for (auto shift : {24, 16, 8, 0}) {
      out.push_back(static_cast<std::uint8_t>((stream_id >> shift) & 0xffU));
   }
   for (auto shift : {24, 16, 8, 0}) {
      out.push_back(static_cast<std::uint8_t>((length >> shift) & 0xffU));
   }
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

[[nodiscard]] std::uint32_t load_u32(const bytes& value, std::size_t offset) {
   BOOST_REQUIRE_GE(value.size(), offset + 4);
   return (static_cast<std::uint32_t>(value[offset]) << 24U) | (static_cast<std::uint32_t>(value[offset + 1]) << 16U) |
          (static_cast<std::uint32_t>(value[offset + 2]) << 8U) | static_cast<std::uint32_t>(value[offset + 3]);
}

[[nodiscard]] frame_type type_of(const bytes& value) {
   BOOST_REQUIRE_GE(value.size(), header_size);
   return static_cast<frame_type>(value[1]);
}

[[nodiscard]] std::uint16_t flags_of(const bytes& value) {
   BOOST_REQUIRE_GE(value.size(), header_size);
   return static_cast<std::uint16_t>((static_cast<std::uint16_t>(value[2]) << 8U) | value[3]);
}

[[nodiscard]] std::uint32_t stream_id_of(const bytes& value) {
   return load_u32(value, 4);
}

[[nodiscard]] std::uint32_t length_of(const bytes& value) {
   return load_u32(value, 8);
}

[[nodiscard]] bytes payload_of(const bytes& value) {
   BOOST_REQUIRE_GE(value.size(), header_size);
   return bytes{value.begin() + static_cast<std::ptrdiff_t>(header_size), value.end()};
}

void append_bytes(bytes& target, const bytes& source) {
   target.insert(target.end(), source.begin(), source.end());
}

template <typename T> struct spawned_result {
   forge::asio::notification completion;
   std::optional<T> value;
   std::exception_ptr error;
   std::atomic_bool done = false;
};

template <typename T>
[[nodiscard]] std::shared_ptr<spawned_result<T>> spawn_result(boost::asio::any_io_executor executor,
                                                              boost::asio::awaitable<T> operation) {
   auto state = std::make_shared<spawned_result<T>>();
   boost::asio::co_spawn(executor, std::move(operation), [state](std::exception_ptr error, T value) mutable {
      state->error = std::move(error);
      if (!state->error) {
         state->value.emplace(std::move(value));
      }
      state->done.store(true, std::memory_order_release);
      state->completion.notify();
   });
   return state;
}

template <typename T> boost::asio::awaitable<T> take_result(std::shared_ptr<spawned_result<T>> state) {
   while (!state->done.load(std::memory_order_acquire)) {
      const auto observed = state->completion.epoch();
      if (!state->done.load(std::memory_order_acquire)) {
         (void)co_await state->completion.async_wait(observed);
      }
   }
   if (state->error) {
      std::rethrow_exception(state->error);
   }
   co_return std::move(*state->value);
}

template <> struct spawned_result<void> {
   forge::asio::notification completion;
   std::exception_ptr error;
   std::atomic_bool done = false;
};

template <>
[[nodiscard]] std::shared_ptr<spawned_result<void>> spawn_result(boost::asio::any_io_executor executor,
                                                                 boost::asio::awaitable<void> operation) {
   auto state = std::make_shared<spawned_result<void>>();
   boost::asio::co_spawn(executor, std::move(operation), [state](std::exception_ptr error) mutable {
      state->error = std::move(error);
      state->done.store(true, std::memory_order_release);
      state->completion.notify();
   });
   return state;
}

[[nodiscard]] std::shared_ptr<spawned_result<void>>
spawn_cancelable_result(boost::asio::any_io_executor executor, boost::asio::awaitable<void> operation,
                        boost::asio::cancellation_slot cancellation) {
   auto state = std::make_shared<spawned_result<void>>();
   boost::asio::co_spawn(executor, std::move(operation),
                         boost::asio::bind_cancellation_slot(cancellation, [state](std::exception_ptr error) mutable {
                            state->error = std::move(error);
                            state->done.store(true, std::memory_order_release);
                            state->completion.notify();
                         }));
   return state;
}

template <> boost::asio::awaitable<void> take_result(std::shared_ptr<spawned_result<void>> state) {
   while (!state->done.load(std::memory_order_acquire)) {
      const auto observed = state->completion.epoch();
      if (!state->done.load(std::memory_order_acquire)) {
         (void)co_await state->completion.async_wait(observed);
      }
   }
   if (state->error) {
      std::rethrow_exception(state->error);
   }
}

template <typename T>
boost::asio::awaitable<T> take_result_for(std::shared_ptr<spawned_result<T>> state, std::chrono::milliseconds timeout,
                                          std::string_view label = {}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!state->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      const auto observed = state->completion.epoch();
      if (!state->done.load(std::memory_order_acquire)) {
         (void)co_await state->completion.async_wait_until(observed, deadline);
      }
   }
   BOOST_REQUIRE_MESSAGE(state->done.load(std::memory_order_acquire), "yamux operation timed out: " << label);
   if (state->error) {
      std::rethrow_exception(state->error);
   }
   co_return std::move(*state->value);
}

template <>
boost::asio::awaitable<void> take_result_for(std::shared_ptr<spawned_result<void>> state,
                                             std::chrono::milliseconds timeout, std::string_view label) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!state->done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
      const auto observed = state->completion.epoch();
      if (!state->done.load(std::memory_order_acquire)) {
         (void)co_await state->completion.async_wait_until(observed, deadline);
      }
   }
   BOOST_REQUIRE_MESSAGE(state->done.load(std::memory_order_acquire), "yamux operation timed out: " << label);
   if (state->error) {
      std::rethrow_exception(state->error);
   }
}

boost::asio::awaitable<void> close_transport_for_test(forge::net::transport::stream& stream) {
   try {
      co_await stream.async_close();
   } catch (const forge::net::transport::exceptions::closed&) {
   }
}

boost::asio::awaitable<bytes> read_transport_for_test(forge::net::transport::stream& stream, std::string_view label,
                                                      std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto state = spawn_result<bytes>(executor, stream.async_read());
   BOOST_TEST_CHECKPOINT("waiting for yamux transport operation: " << label);
   co_return co_await take_result_for(state, timeout);
}

boost::asio::awaitable<void> write_transport_for_test(forge::net::transport::stream& stream, bytes value,
                                                      std::string_view label,
                                                      std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto state = spawn_result<void>(executor, stream.async_write(value));
   try {
      co_await take_result_for(state, timeout);
   } catch (...) {
      BOOST_TEST_CHECKPOINT("yamux operation failed while writing " << label);
      throw;
   }
}

struct pending_write {
   pending_write(boost::asio::any_io_executor executor, bytes write_value)
       : timer(std::move(executor), (std::chrono::steady_clock::time_point::max)()), value(std::move(write_value)) {}

   boost::asio::steady_timer timer;
   bytes value;
   bool released = false;
};

struct pipe_state {
   explicit pipe_state(boost::asio::any_io_executor executor)
       : read_timer(std::move(executor), (std::chrono::steady_clock::time_point::max)()) {}

   std::mutex mutex;
   boost::asio::steady_timer read_timer;
   forge::asio::notification changed;
   std::deque<bytes> reads;
   std::deque<std::shared_ptr<pending_write>> pending_writes;
   std::deque<forge::net::transport::chunk> retained_writes;
   bool closed = false;
   bool hold_writes = false;
   bool retain_write_lifetimes = false;
   bool half_close_only = false;
   std::uint64_t writes = 0;
   std::uint64_t write_calls = 0;
   std::size_t close_calls = 0;
   std::size_t active_reads = 0;
   std::size_t max_active_reads = 0;
   std::size_t fail_writes = 0;
   std::size_t fail_write_allocations = 0;
   std::size_t failed_writes = 0;
   std::size_t cancel_calls = 0;
   bool defer_cancel = false;
   bool hold_close = false;
   bool fail_close_after_terminal = false;
   bool throw_after_cancel = false;
};

struct lifetime_barrier_state {
   std::atomic_bool entered = false;
   forge::asio::notification changed;
};

class lifetime_barrier {
 public:
   explicit lifetime_barrier(std::shared_ptr<lifetime_barrier_state> state) : state_(std::move(state)) {}

   ~lifetime_barrier() {
      state_->entered.store(true, std::memory_order_release);
      state_->changed.notify();
   }

 private:
   std::shared_ptr<lifetime_barrier_state> state_;
};

class active_read {
 public:
   explicit active_read(std::shared_ptr<pipe_state> state) : state_(std::move(state)) {
      {
         auto lock = std::scoped_lock{state_->mutex};
         ++state_->active_reads;
         state_->max_active_reads = (std::max)(state_->max_active_reads, state_->active_reads);
      }
      state_->changed.notify();
   }

   active_read(const active_read&) = delete;
   active_read& operator=(const active_read&) = delete;

   ~active_read() {
      {
         auto lock = std::scoped_lock{state_->mutex};
         --state_->active_reads;
      }
      state_->read_timer.cancel();
      state_->changed.notify();
   }

 private:
   std::shared_ptr<pipe_state> state_;
};

class pipe_stream final : public forge::net::transport::detail::stream_concept {
 public:
   pipe_stream(std::int64_t id, std::shared_ptr<pipe_state> inbound, std::shared_ptr<pipe_state> outbound)
       : id_(id), inbound_(std::move(inbound)), outbound_(std::move(outbound)) {}

   [[nodiscard]] bool valid() const noexcept override {
      auto lock = std::scoped_lock{inbound_->mutex};
      return !inbound_->closed;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return id_;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      auto executor = co_await boost::asio::this_coro::executor;
      auto pending = std::shared_ptr<pending_write>{};
      {
         auto lock = std::scoped_lock{outbound_->mutex};
         ++outbound_->write_calls;
         if (outbound_->closed) {
            FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream closed");
         }
         if (outbound_->fail_write_allocations > 0) {
            --outbound_->fail_write_allocations;
            ++outbound_->failed_writes;
            throw std::bad_alloc{};
         }
         if (outbound_->fail_writes > 0) {
            --outbound_->fail_writes;
            ++outbound_->failed_writes;
            FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream write failed");
         }
         auto owned = bytes{value.begin(), value.end()};
         if (outbound_->hold_writes) {
            pending = std::make_shared<pending_write>(executor, std::move(owned));
            outbound_->pending_writes.push_back(pending);
         } else {
            outbound_->reads.push_back(std::move(owned));
            ++outbound_->writes;
         }
      }
      outbound_->read_timer.cancel();
      outbound_->changed.notify();
      if (!pending) {
         co_return;
      }

      auto error = boost::system::error_code{};
      while (true) {
         {
            auto lock = std::scoped_lock{outbound_->mutex};
            if (pending->released) {
               outbound_->reads.push_back(std::move(pending->value));
               ++outbound_->writes;
               break;
            }
            if (outbound_->closed) {
               FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream closed");
            }
         }
         co_await pending->timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         if (error && error != boost::asio::error::operation_aborted) {
            throw boost::system::system_error{error};
         }
      }
      outbound_->read_timer.cancel();
      outbound_->changed.notify();
      co_return;
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk value) override {
      {
         auto lock = std::scoped_lock{outbound_->mutex};
         if (outbound_->closed) {
            FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream closed");
         }
         if (outbound_->retain_write_lifetimes) {
            ++outbound_->write_calls;
            outbound_->reads.push_back(value.to_vector());
            outbound_->retained_writes.push_back(std::move(value));
            ++outbound_->writes;
            outbound_->read_timer.cancel();
            co_return;
         }
      }
      co_await async_write(value.bytes());
   }

   boost::asio::awaitable<bytes> async_read() override {
      auto active = active_read{inbound_};
      while (true) {
         const auto observed = inbound_->changed.epoch();
         {
            auto lock = std::scoped_lock{inbound_->mutex};
            if (!inbound_->reads.empty()) {
               auto out = std::move(inbound_->reads.front());
               inbound_->reads.pop_front();
               co_return out;
            }
            if (inbound_->closed) {
               FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream closed");
            }
         }
         (void)co_await inbound_->changed.async_wait(observed);
      }
   }

   boost::asio::awaitable<void> async_close() override {
      auto half_close_only = false;
      auto fail_after_terminal = false;
      {
         auto lock = std::scoped_lock{inbound_->mutex, outbound_->mutex};
         ++inbound_->close_calls;
      }
      inbound_->changed.notify();
      while (true) {
         const auto observed = inbound_->changed.epoch();
         {
            auto lock = std::scoped_lock{inbound_->mutex, outbound_->mutex};
            if (!inbound_->hold_close) {
               half_close_only = inbound_->half_close_only;
               if (!half_close_only) {
                  inbound_->closed = true;
               }
               outbound_->closed = true;
               if (!half_close_only) {
                  for (const auto& pending : inbound_->pending_writes) {
                     pending->timer.cancel();
                  }
                  inbound_->pending_writes.clear();
                  inbound_->retained_writes.clear();
               }
               for (const auto& pending : outbound_->pending_writes) {
                  pending->timer.cancel();
               }
               outbound_->pending_writes.clear();
               outbound_->retained_writes.clear();
               fail_after_terminal = inbound_->fail_close_after_terminal;
               inbound_->fail_close_after_terminal = false;
               break;
            }
         }
         (void)co_await inbound_->changed.async_wait(observed);
      }
      inbound_->changed.notify();
      outbound_->changed.notify();
      if (!half_close_only) {
         inbound_->read_timer.cancel();
      }
      outbound_->read_timer.cancel();
      if (fail_after_terminal) {
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed,
                               "pipe close failed after terminal cleanup");
      }
      co_return;
   }

   void cancel() override {
      auto throw_after_cancel = false;
      auto defer_cancel = false;
      {
         auto lock = std::scoped_lock{inbound_->mutex, outbound_->mutex};
         ++inbound_->cancel_calls;
         throw_after_cancel = inbound_->throw_after_cancel;
         defer_cancel = inbound_->defer_cancel;
         if (!defer_cancel) {
            inbound_->closed = true;
            outbound_->closed = true;
            for (const auto& pending : inbound_->pending_writes) {
               pending->timer.cancel();
            }
            for (const auto& pending : outbound_->pending_writes) {
               pending->timer.cancel();
            }
            inbound_->pending_writes.clear();
            outbound_->pending_writes.clear();
            inbound_->retained_writes.clear();
            outbound_->retained_writes.clear();
         }
      }
      if (defer_cancel) {
         inbound_->changed.notify();
         outbound_->changed.notify();
         return;
      }
      inbound_->read_timer.cancel();
      outbound_->read_timer.cancel();
      inbound_->changed.notify();
      outbound_->changed.notify();
      if (throw_after_cancel) {
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "pipe stream cancel failed after close");
      }
   }

 private:
   std::int64_t id_ = 0;
   std::shared_ptr<pipe_state> inbound_;
   std::shared_ptr<pipe_state> outbound_;
};

struct stream_pair {
   forge::net::transport::stream left;
   forge::net::transport::stream right;
   std::shared_ptr<pipe_state> left_state;
   std::shared_ptr<pipe_state> right_state;
};

[[nodiscard]] stream_pair make_stream_pair(boost::asio::any_io_executor executor) {
   auto left_state = std::make_shared<pipe_state>(executor);
   auto right_state = std::make_shared<pipe_state>(executor);
   return stream_pair{
       .left = forge::net::transport::detail::stream_access::make(
           std::make_shared<pipe_stream>(1, left_state, right_state)),
       .right = forge::net::transport::detail::stream_access::make(
           std::make_shared<pipe_stream>(2, right_state, left_state)),
       .left_state = left_state,
       .right_state = right_state,
   };
}

void hold_writes(const std::shared_ptr<pipe_state>& state, bool value) {
   auto lock = std::scoped_lock{state->mutex};
   state->hold_writes = value;
}

void fail_next_write(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   ++state->fail_writes;
}

void fail_next_write_allocation(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   ++state->fail_write_allocations;
}

void retain_write_lifetimes(const std::shared_ptr<pipe_state>& state, bool value) {
   auto lock = std::scoped_lock{state->mutex};
   state->retain_write_lifetimes = value;
}

void half_close_only(const std::shared_ptr<pipe_state>& state, bool value) {
   auto lock = std::scoped_lock{state->mutex};
   state->half_close_only = value;
}

void defer_pipe_cancel(const std::shared_ptr<pipe_state>& state, bool value) {
   auto lock = std::scoped_lock{state->mutex};
   state->defer_cancel = value;
}

void hold_pipe_close(const std::shared_ptr<pipe_state>& state, bool value) {
   {
      auto lock = std::scoped_lock{state->mutex};
      state->hold_close = value;
   }
   state->changed.notify();
}

void fail_next_pipe_close_after_terminal(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   state->fail_close_after_terminal = true;
}

void drain_retained_writes(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   state->retained_writes.clear();
}

void release_next_write(const std::shared_ptr<pipe_state>& state) {
   auto pending = std::shared_ptr<pending_write>{};
   {
      auto lock = std::scoped_lock{state->mutex};
      BOOST_REQUIRE(!state->pending_writes.empty());
      pending = state->pending_writes.front();
      state->pending_writes.pop_front();
      pending->released = true;
   }
   pending->timer.cancel();
}

void release_pending_writes_if_any(const std::shared_ptr<pipe_state>& state) {
   auto pending = std::deque<std::shared_ptr<pending_write>>{};
   {
      auto lock = std::scoped_lock{state->mutex};
      state->hold_writes = false;
      pending.swap(state->pending_writes);
      for (const auto& write : pending) {
         write->released = true;
      }
   }
   for (const auto& write : pending) {
      write->timer.cancel();
   }
}

boost::asio::awaitable<void> release_pending_writes_after(std::shared_ptr<pipe_state> state,
                                                          std::chrono::milliseconds delay) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(delay);
   co_await timer.async_wait(boost::asio::use_awaitable);
   release_pending_writes_if_any(state);
}

boost::asio::awaitable<void> wait_for_pending_writes(const std::shared_ptr<pipe_state>& state, std::size_t expected,
                                                     std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto observed = state->changed.epoch();
      {
         auto lock = std::scoped_lock{state->mutex};
         if (state->pending_writes.size() >= expected) {
            co_return;
         }
      }
      (void)co_await state->changed.async_wait_until(observed, deadline);
   }
   BOOST_FAIL("yamux pending writes did not reach expected count");
   co_return;
}

[[nodiscard]] std::size_t active_reads(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   return state->active_reads;
}

[[nodiscard]] std::size_t max_active_reads(const std::shared_ptr<pipe_state>& state) {
   auto lock = std::scoped_lock{state->mutex};
   return state->max_active_reads;
}

boost::asio::awaitable<void> wait_for_active_reads(const std::shared_ptr<pipe_state>& state, std::size_t expected,
                                                   std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto observed = state->changed.epoch();
      if (active_reads(state) == expected) {
         co_return;
      }
      (void)co_await state->changed.async_wait_until(observed, deadline);
   }
   BOOST_FAIL("yamux active reads did not reach expected count");
   co_return;
}

boost::asio::awaitable<void> yamux_open_accept_and_early_data() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto initiator = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto responder = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto accept = spawn_result<forge::net::transport::stream>(executor, responder.async_accept_stream());
   auto outbound = co_await initiator.async_open_stream();
   BOOST_CHECK_EQUAL(outbound.id(), 1);

   const auto payload = text_bytes("early request");
   co_await outbound.async_write(payload);
   auto inbound = co_await take_result(accept);
   BOOST_CHECK_EQUAL(inbound.id(), 1);
   auto received = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   const auto chunk_payload = text_bytes("chunk response");
   co_await inbound.async_write(forge::net::transport::chunk{chunk_payload});
   auto received_chunk = co_await outbound.async_read_chunk();
   const auto received_chunk_bytes = received_chunk.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_chunk_bytes.begin(), received_chunk_bytes.end(), chunk_payload.begin(),
                                 chunk_payload.end());

   const auto framed_chunk = text_bytes("framed chunk over yamux");
   co_await outbound.async_write_frame(forge::net::transport::chunk{framed_chunk});
   auto received_frame_chunk = co_await inbound.async_read_frame_chunk();
   const auto received_frame_chunk_bytes = received_frame_chunk.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_frame_chunk_bytes.begin(), received_frame_chunk_bytes.end(),
                                 framed_chunk.begin(), framed_chunk.end());

   co_await initiator.async_close();
   co_await responder.async_close();
}

boost::asio::awaitable<void> yamux_concurrent_first_operations_share_one_read_loop() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto left_open = spawn_result<forge::net::transport::stream>(executor, left.async_open_stream());
   auto left_accept = spawn_result<forge::net::transport::stream>(executor, left.async_accept_stream());
   auto right_open = spawn_result<forge::net::transport::stream>(executor, right.async_open_stream());
   auto right_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());

   auto left_outbound = co_await take_result_for(left_open, std::chrono::seconds{1}, "left open");
   auto left_inbound = co_await take_result_for(left_accept, std::chrono::seconds{1}, "left accept");
   auto right_outbound = co_await take_result_for(right_open, std::chrono::seconds{1}, "right open");
   auto right_inbound = co_await take_result_for(right_accept, std::chrono::seconds{1}, "right accept");

   BOOST_CHECK_EQUAL(left_outbound.id(), right_inbound.id());
   BOOST_CHECK_EQUAL(right_outbound.id(), left_inbound.id());
   BOOST_TEST(max_active_reads(pair.left_state) == 1U);
   BOOST_TEST(max_active_reads(pair.right_state) == 1U);

   auto left_close = spawn_result<void>(executor, left.async_close());
   auto right_close = spawn_result<void>(executor, right.async_close());
   co_await take_result_for(left_close, std::chrono::seconds{1});
   co_await take_result_for(right_close, std::chrono::seconds{1});
}

boost::asio::awaitable<void> yamux_close_waits_for_read_loop() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto session = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto accept = spawn_result<forge::net::transport::stream>(executor, session.async_accept_stream());

   co_await wait_for_active_reads(pair.left_state, 1);
   co_await session.async_close();

   BOOST_TEST(active_reads(pair.left_state) == 0U);
   BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
}

boost::asio::awaitable<void> yamux_close_bounds_underlying_half_close() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   half_close_only(pair.left_state, true);
   auto session =
       forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                  forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25}}};
   auto accept = spawn_result<forge::net::transport::stream>(executor, session.async_accept_stream());

   co_await wait_for_active_reads(pair.left_state, 1);
   const auto started = std::chrono::steady_clock::now();
   co_await session.async_close();
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed >= std::chrono::milliseconds{20});
   BOOST_TEST(elapsed < std::chrono::milliseconds{500});
   BOOST_TEST(active_reads(pair.left_state) == 0U);
   BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
}

boost::asio::awaitable<void> yamux_close_bounds_a_write_that_holds_the_gate() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto session =
       forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                  forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25}}};
   auto stream = co_await session.async_open_stream();
   hold_writes(pair.right_state, true);

   auto write = spawn_result<void>(executor, stream.async_write(text_bytes("blocked write")));
   co_await wait_for_pending_writes(pair.right_state, 1);
   auto delayed_release =
       spawn_result<void>(executor, release_pending_writes_after(pair.right_state, std::chrono::milliseconds{125}));

   const auto started = std::chrono::steady_clock::now();
   co_await session.async_close();
   const auto elapsed = std::chrono::steady_clock::now() - started;

   BOOST_TEST(elapsed >= std::chrono::milliseconds{20});
   BOOST_TEST(elapsed < std::chrono::milliseconds{100});
   BOOST_CHECK_THROW((void)co_await take_result_for(write, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
   co_await take_result_for(delayed_release, std::chrono::seconds{1});
}

boost::asio::awaitable<void> yamux_concurrent_streams_do_not_cross_deliver() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto accept_second = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto first = co_await left.async_open_stream();
   auto second = co_await left.async_open_stream();
   BOOST_CHECK_EQUAL(first.id(), 1);
   BOOST_CHECK_EQUAL(second.id(), 3);

   const auto first_payload = text_bytes("stream-one");
   const auto second_payload = text_bytes("stream-two");
   co_await second.async_write(second_payload);
   co_await first.async_write(first_payload);

   auto inbound_first = co_await take_result(accept_first);
   auto inbound_second = co_await take_result(accept_second);
   if (inbound_first.id() > inbound_second.id()) {
      std::swap(inbound_first, inbound_second);
   }
   auto received_first = co_await inbound_first.async_read();
   auto received_second = co_await inbound_second.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_first.begin(), received_first.end(), first_payload.begin(),
                                 first_payload.end());
   BOOST_CHECK_EQUAL_COLLECTIONS(received_second.begin(), received_second.end(), second_payload.begin(),
                                 second_payload.end());

   co_await left.async_close();
   auto close = spawn_result<void>(executor, right.async_close());
   co_await take_result_for(close, std::chrono::seconds{1});
}

boost::asio::awaitable<void> yamux_concurrent_writes_are_fifo_without_timer_spin() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{.max_frame_size = 64};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};

   auto first = co_await left.async_open_stream();
   auto first_syn = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(first_syn), frame_type::window_update);
   BOOST_CHECK_EQUAL(stream_id_of(first_syn), 1U);
   auto second = co_await left.async_open_stream();
   auto second_syn = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(second_syn), frame_type::window_update);
   BOOST_CHECK_EQUAL(stream_id_of(second_syn), 3U);
   auto third = co_await left.async_open_stream();
   auto third_syn = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(third_syn), frame_type::window_update);
   BOOST_CHECK_EQUAL(stream_id_of(third_syn), 5U);

   hold_writes(pair.right_state, true);
   const auto first_payload = text_bytes("first");
   const auto second_payload = text_bytes("second");
   const auto third_payload = text_bytes("third");
   auto first_write = spawn_result<void>(executor, first.async_write(first_payload));
   auto second_write = spawn_result<void>(executor, second.async_write(second_payload));
   auto third_write = spawn_result<void>(executor, third.async_write(third_payload));

   co_await wait_for_pending_writes(pair.right_state, 1);
   release_next_write(pair.right_state);
   auto first_frame = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(first_frame), frame_type::data);
   BOOST_CHECK_EQUAL(stream_id_of(first_frame), 1U);
   BOOST_TEST(payload_of(first_frame) == first_payload, boost::test_tools::per_element());
   co_await take_result_for(first_write, std::chrono::seconds{1});

   co_await wait_for_pending_writes(pair.right_state, 1);
   release_next_write(pair.right_state);
   auto second_frame = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(second_frame), frame_type::data);
   BOOST_CHECK_EQUAL(stream_id_of(second_frame), 3U);
   BOOST_TEST(payload_of(second_frame) == second_payload, boost::test_tools::per_element());
   co_await take_result_for(second_write, std::chrono::seconds{1});

   co_await wait_for_pending_writes(pair.right_state, 1);
   release_next_write(pair.right_state);
   auto third_frame = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(type_of(third_frame), frame_type::data);
   BOOST_CHECK_EQUAL(stream_id_of(third_frame), 5U);
   BOOST_TEST(payload_of(third_frame) == third_payload, boost::test_tools::per_element());
   co_await take_result_for(third_write, std::chrono::seconds{1});

   co_await pair.right.async_close();
   co_await left.async_close();
}

boost::asio::awaitable<void> yamux_concurrent_stream_opens_do_not_lose_write_wakeup() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{.max_streams = 256};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};

   auto opens = std::vector<std::shared_ptr<spawned_result<forge::net::transport::stream>>>{};
   opens.reserve(128);
   for (auto remaining = 128U; remaining != 0U; --remaining) {
      opens.push_back(spawn_result<forge::net::transport::stream>(executor, left.async_open_stream()));
   }
   for (const auto& open : opens) {
      auto stream = co_await take_result_for(open, std::chrono::seconds{2});
      BOOST_TEST(stream.valid());
   }

   co_await pair.right.async_close();
   co_await left.async_close();
}

boost::asio::awaitable<void> yamux_cancel_wakes_pending_write_waiters() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{.max_frame_size = 64};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};

   auto first = co_await left.async_open_stream();
   (void)co_await pair.right.async_read();
   auto second = co_await left.async_open_stream();
   (void)co_await pair.right.async_read();

   hold_writes(pair.right_state, true);
   auto first_write = spawn_result<void>(executor, first.async_write(text_bytes("held")));
   auto second_write = spawn_result<void>(executor, second.async_write(text_bytes("waiting")));
   co_await wait_for_pending_writes(pair.right_state, 1);

   left.cancel();

   BOOST_CHECK_THROW((void)co_await take_result_for(first_write, std::chrono::seconds{1}), forge::exceptions::base);
   BOOST_CHECK_THROW((void)co_await take_result_for(second_write, std::chrono::seconds{1}), forge::exceptions::base);
   co_await pair.right.async_close();
}

boost::asio::awaitable<void> yamux_canceled_queued_write_preserves_fifo_ownership() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{.max_frame_size = 64};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};

   auto first = co_await left.async_open_stream();
   (void)co_await pair.right.async_read();
   auto canceled = co_await left.async_open_stream();
   (void)co_await pair.right.async_read();
   auto successor = co_await left.async_open_stream();
   (void)co_await pair.right.async_read();

   hold_writes(pair.right_state, true);
   const auto first_payload = text_bytes("first");
   const auto successor_payload = text_bytes("successor");
   auto first_write = spawn_result<void>(executor, first.async_write(first_payload));
   co_await wait_for_pending_writes(pair.right_state, 1);

   auto cancellation = boost::asio::cancellation_signal{};
   auto canceled_write =
       spawn_cancelable_result(executor, canceled.async_write(text_bytes("canceled")), cancellation.slot());
   auto successor_write = spawn_result<void>(executor, successor.async_write(successor_payload));

   auto queued = boost::asio::steady_timer{executor};
   queued.expires_after(std::chrono::milliseconds{10});
   co_await queued.async_wait(boost::asio::use_awaitable);
   cancellation.emit(boost::asio::cancellation_type::all);

   release_next_write(pair.right_state);
   auto first_frame = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(stream_id_of(first_frame), 1U);
   BOOST_TEST(payload_of(first_frame) == first_payload, boost::test_tools::per_element());
   co_await take_result_for(first_write, std::chrono::seconds{1});
   BOOST_CHECK_THROW((void)co_await take_result_for(canceled_write, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::canceled);

   co_await wait_for_pending_writes(pair.right_state, 1);
   release_next_write(pair.right_state);
   auto successor_frame = co_await pair.right.async_read();
   BOOST_CHECK_EQUAL(stream_id_of(successor_frame), 5U);
   BOOST_TEST(payload_of(successor_frame) == successor_payload, boost::test_tools::per_element());
   co_await take_result_for(successor_write, std::chrono::seconds{1});

   co_await pair.right.async_close();
   co_await left.async_close();
}

boost::asio::awaitable<void> yamux_canceled_writes_do_not_publish_stream_state() {
   auto executor = co_await boost::asio::this_coro::executor;

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                             forge::net::yamux::options{.max_frame_size = 8, .max_streams = 2}};
      auto first = co_await left.async_open_stream();
      (void)co_await pair.right.async_read();

      hold_writes(pair.right_state, true);
      auto held = spawn_result<void>(executor, first.async_write(text_bytes("held")));
      co_await wait_for_pending_writes(pair.right_state, 1);

      auto cancellation = boost::asio::cancellation_signal{};
      auto canceled_open = spawn_cancelable_result(
          executor, [&left]() -> boost::asio::awaitable<void> { (void)co_await left.async_open_stream(); }(),
          cancellation.slot());
      auto queued = boost::asio::steady_timer{executor};
      queued.expires_after(std::chrono::milliseconds{10});
      co_await queued.async_wait(boost::asio::use_awaitable);
      cancellation.emit(boost::asio::cancellation_type::all);

      release_next_write(pair.right_state);
      (void)co_await pair.right.async_read();
      co_await take_result_for(held, std::chrono::seconds{1});
      BOOST_CHECK_THROW((void)co_await take_result_for(canceled_open, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::canceled);

      auto replacement = spawn_result<forge::net::transport::stream>(executor, left.async_open_stream());
      co_await wait_for_pending_writes(pair.right_state, 1);
      release_next_write(pair.right_state);
      const auto replacement_syn = co_await pair.right.async_read();
      BOOST_CHECK_EQUAL(flags_of(replacement_syn), syn);
      BOOST_CHECK_EQUAL(stream_id_of(replacement_syn), 3U);
      BOOST_TEST((co_await take_result_for(replacement, std::chrono::seconds{1})).valid());

      co_await pair.right.async_close();
      co_await left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                             forge::net::yamux::options{.max_frame_size = 4}};
      auto gate_owner = co_await left.async_open_stream();
      (void)co_await pair.right.async_read();
      auto target = co_await left.async_open_stream();
      (void)co_await pair.right.async_read();

      hold_writes(pair.right_state, true);
      auto held = spawn_result<void>(executor, gate_owner.async_write(text_bytes("x")));
      co_await wait_for_pending_writes(pair.right_state, 1);

      auto data_cancellation = boost::asio::cancellation_signal{};
      auto canceled_data =
          spawn_cancelable_result(executor, target.async_write(text_bytes("data")), data_cancellation.slot());
      auto queued = boost::asio::steady_timer{executor};
      queued.expires_after(std::chrono::milliseconds{10});
      co_await queued.async_wait(boost::asio::use_awaitable);
      data_cancellation.emit(boost::asio::cancellation_type::all);
      release_next_write(pair.right_state);
      (void)co_await pair.right.async_read();
      co_await take_result_for(held, std::chrono::seconds{1});
      BOOST_CHECK_THROW((void)co_await take_result_for(canceled_data, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::canceled);

      auto replacement_data = spawn_result<void>(executor, target.async_write(text_bytes("data")));
      co_await wait_for_pending_writes(pair.right_state, 1);
      release_next_write(pair.right_state);
      const auto data_frame = co_await pair.right.async_read();
      BOOST_CHECK_EQUAL(stream_id_of(data_frame), 3U);
      BOOST_CHECK_EQUAL(length_of(data_frame), 4U);
      co_await take_result_for(replacement_data, std::chrono::seconds{1});

      auto held_again = spawn_result<void>(executor, gate_owner.async_write(text_bytes("y")));
      co_await wait_for_pending_writes(pair.right_state, 1);
      auto fin_cancellation = boost::asio::cancellation_signal{};
      auto terminal_fin = spawn_cancelable_result(executor, target.async_close(), fin_cancellation.slot());
      release_next_write(pair.right_state);
      (void)co_await pair.right.async_read();
      co_await take_result_for(held_again, std::chrono::seconds{1});
      co_await wait_for_pending_writes(pair.right_state, 1);
      fin_cancellation.emit(boost::asio::cancellation_type::all);
      BOOST_TEST(!terminal_fin->done.load(std::memory_order_acquire));
      release_next_write(pair.right_state);
      const auto fin_frame = co_await pair.right.async_read();
      BOOST_CHECK_EQUAL(stream_id_of(fin_frame), 3U);
      BOOST_CHECK_EQUAL(flags_of(fin_frame), fin);
      co_await take_result_for(terminal_fin, std::chrono::seconds{1});

      auto repeated_fin = spawn_result<void>(executor, target.async_close());
      co_await take_result_for(repeated_fin, std::chrono::seconds{1});

      co_await pair.right.async_close();
      co_await left.async_close();
   }
}

boost::asio::awaitable<void> yamux_window_updates_are_deltas() {
   auto executor = co_await boost::asio::this_coro::executor;

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::initiator};
      (void)co_await left.async_open_stream();
      const auto request = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(flags_of(request), syn);
      BOOST_CHECK_EQUAL(length_of(request), 0U);
      co_await pair.left.async_close();
      co_await left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accepted = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await write_transport_for_test(pair.left, frame(frame_type::window_update, syn, 1, 0),
                                        "receive-credit setup SYN");
      const auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(length_of(response), 0U);
      (void)co_await take_result_for(accepted, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      constexpr auto peer_delta = 5U;
      constexpr auto local_delta = 9U;
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{
                                                  .initial_window = initial_stream_window + local_delta,
                                                  .max_stream_window = initial_stream_window + 16U,
                                                  .max_frame_size = initial_stream_window + 16U,
                                              }};
      auto accepted = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, peer_delta));
      const auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(length_of(response), local_delta);

      auto inbound = co_await take_result_for(accepted, std::chrono::seconds{1});
      const auto payload = deterministic_bytes(initial_stream_window + peer_delta);
      auto write = spawn_result<void>(executor, inbound.async_write(payload));
      const auto data = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(length_of(data), payload.size());
      BOOST_TEST(payload_of(data) == payload, boost::test_tools::per_element());
      co_await take_result_for(write, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      constexpr auto local_delta = 5U;
      constexpr auto peer_delta = 9U;
      auto left = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::initiator,
                                             forge::net::yamux::options{
                                                 .initial_window = initial_stream_window + local_delta,
                                                 .max_stream_window = initial_stream_window + 16U,
                                                 .max_frame_size = initial_stream_window,
                                             }};
      auto outbound = co_await left.async_open_stream();
      const auto request = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(flags_of(request), syn);
      BOOST_CHECK_EQUAL(length_of(request), local_delta);
      co_await pair.left.async_write(frame(frame_type::window_update, ack, 1, peer_delta));

      const auto payload = deterministic_bytes(initial_stream_window + peer_delta, 0x52);
      auto write = spawn_result<void>(executor, outbound.async_write(payload));
      auto received = bytes{};
      const auto first = co_await read_transport_for_test(pair.left, "ACK baseline payload");
      BOOST_CHECK_EQUAL(type_of(first), frame_type::data);
      BOOST_CHECK_EQUAL(stream_id_of(first), 1U);
      BOOST_CHECK_EQUAL(length_of(first), initial_stream_window);
      append_bytes(received, payload_of(first));

      const auto second = co_await read_transport_for_test(pair.left, "ACK delta payload");
      BOOST_CHECK_EQUAL(type_of(second), frame_type::data);
      BOOST_CHECK_EQUAL(stream_id_of(second), 1U);
      BOOST_CHECK_EQUAL(length_of(second), peer_delta);
      append_bytes(received, payload_of(second));
      BOOST_TEST(received == payload, boost::test_tools::per_element());
      co_await take_result_for(write, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      constexpr auto local_delta = 7U;
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{
                                                  .initial_window = initial_stream_window + local_delta,
                                                  .max_stream_window = initial_stream_window + local_delta,
                                              }};
      auto accepted = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto early = text_bytes("early");
      co_await pair.left.async_write(frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(early.size()), early));
      const auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(length_of(response), local_delta);

      auto inbound = co_await take_result_for(accepted, std::chrono::seconds{1});
      const auto payload = deterministic_bytes(initial_stream_window + 1U, 0x73);
      auto write = spawn_result<void>(executor, inbound.async_write(payload));
      const auto first = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first), frame_type::data);
      BOOST_CHECK_EQUAL(length_of(first), initial_stream_window);
      BOOST_TEST(!write->done.load(std::memory_order_acquire));

      co_await pair.left.async_write(frame(frame_type::window_update, 0, 1, 1));
      const auto second = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(second), frame_type::data);
      BOOST_CHECK_EQUAL(length_of(second), 1U);
      co_await take_result_for(write, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await right.async_close();
   }
}

boost::asio::awaitable<void> yamux_reset_streams_are_invalid_and_cancel_reaches_peer() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto ack_frame = co_await read_transport_for_test(pair.left, "remote RST setup ACK");
      BOOST_CHECK_EQUAL(type_of(ack_frame), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(ack_frame), ack);
      auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});

      co_await pair.left.async_write(frame(frame_type::data, rst, 1, 0));
      auto read = spawn_result<bytes>(executor, inbound.async_read());
      BOOST_CHECK_THROW((void)co_await take_result_for(read, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);
      BOOST_TEST(!inbound.valid());
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      auto outbound = co_await left.async_open_stream();
      auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});

      auto read = spawn_result<bytes>(executor, inbound.async_read());
      outbound.cancel();
      BOOST_CHECK_THROW((void)co_await take_result_for(read, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);
      BOOST_TEST(!inbound.valid());
      co_await left.async_close();
      co_await right.async_close();
   }
}

boost::asio::awaitable<void> yamux_request_cancel_resets_only_the_selected_stream() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto first_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto first = co_await left.async_open_stream();
   auto first_inbound = co_await take_result_for(first_accept, std::chrono::seconds{1});
   auto second_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto second = co_await left.async_open_stream();
   auto second_inbound = co_await take_result_for(second_accept, std::chrono::seconds{1});

   auto canceled_read = spawn_result<bytes>(executor, first_inbound.async_read());
   first.request_cancel();
   BOOST_CHECK_THROW((void)co_await take_result_for(canceled_read, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::stream_reset);

   const auto payload = text_bytes("unaffected yamux sibling");
   co_await second.async_write(payload);
   const auto received = co_await second_inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   co_await second.async_close();
   co_await second_inbound.async_close();
   co_await left.async_close();
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_close_joins_stream_cancel_worker_after_wire_rst() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "persistent reset worker setup ACK");

   hold_writes(pair.left_state, true);
   inbound.request_cancel();
   BOOST_TEST(!inbound.valid());
   co_await wait_for_pending_writes(pair.left_state, 1);

   auto close = spawn_result<void>(executor, right.async_close());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_TEST(!close->done.load(std::memory_order_acquire));
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.left_state->pending_writes.size(), 1U);
   }

   release_next_write(pair.left_state);
   const auto reset = co_await read_transport_for_test(pair.left, "persistent reset worker RST");
   BOOST_CHECK_EQUAL(type_of(reset), frame_type::data);
   BOOST_CHECK_EQUAL(flags_of(reset), rst);
   BOOST_CHECK_EQUAL(stream_id_of(reset), 1U);

   co_await wait_for_pending_writes(pair.left_state, 1);
   BOOST_TEST(!close->done.load(std::memory_order_acquire));
   release_next_write(pair.left_state);
   const auto go_away = co_await read_transport_for_test(pair.left, "close after reset worker join");
   BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
   co_await take_result_for(close, std::chrono::seconds{1});
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_transport_session_cancel_then_close_is_terminal() {
   auto executor = co_await boost::asio::this_coro::executor;

   {
      auto pair = make_stream_pair(executor);
      defer_pipe_cancel(pair.right_state, true);
      auto session = forge::net::yamux::make_session(std::move(pair.right), forge::net::yamux::side::responder);

      session.request_cancel();
      BOOST_TEST(!session.valid());
      auto close = spawn_result<void>(executor, session.async_close());
      BOOST_CHECK_THROW((void)co_await take_result_for(close, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::canceled);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      // This transport records cancellation but only releases its pending read
      // from async_close(), as a real terminal backend is permitted to do.
      defer_pipe_cancel(pair.right_state, true);
      auto session = forge::net::yamux::make_session(std::move(pair.right), forge::net::yamux::side::responder);
      auto accept = spawn_result<forge::net::transport::stream>(executor, session.async_accept_stream());

      co_await wait_for_active_reads(pair.right_state, 1);
      session.request_cancel();
      BOOST_TEST(!session.valid());

      auto close = spawn_result<void>(executor, session.async_close());
      BOOST_CHECK_THROW((void)co_await take_result_for(close, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::canceled);
      BOOST_TEST(active_reads(pair.right_state) == 0U);
      BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::canceled);

      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_reset_write_allocation_failure_is_session_terminal() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "reset allocation failure setup ACK");

   fail_next_write_allocation(pair.left_state);
   inbound.request_cancel();
   for (auto attempt = 0; attempt < 100 && right.valid(); ++attempt) {
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }

   BOOST_TEST(!right.valid());
   BOOST_TEST(!inbound.valid());
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_CHECK_EQUAL(pair.left_state->failed_writes, 1U);
   }
   co_await right.async_close();
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_normalizes_underlying_write_failure() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   pair.right.cancel();

   BOOST_CHECK_THROW((void)co_await left.async_open_stream(), forge::net::yamux::exceptions::closed);
}

boost::asio::awaitable<void> yamux_flow_control_waits_for_window_update() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder, options};

   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result(accept);

   const auto payload = deterministic_bytes(initial_stream_window * 2U);
   auto write = spawn_result<void>(executor, outbound.async_write(payload));
   auto first = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(), payload.begin(), payload.begin() + initial_stream_window);
   auto second = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(second.begin(), second.end(), payload.begin() + initial_stream_window, payload.end());
   co_await take_result(write);

   co_await left.async_close();
   auto close = spawn_result<void>(executor, right.async_close());
   co_await take_result_for(close, std::chrono::seconds{1});
}

boost::asio::awaitable<void> yamux_receive_credit_waits_for_window_update_drain() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});

   const auto full_window = deterministic_bytes(initial_stream_window, 0x6a);
   co_await outbound.async_write(full_window);

   hold_writes(pair.left_state, true);
   auto read = spawn_result<bytes>(executor, inbound.async_read());
   co_await wait_for_pending_writes(pair.left_state, 1);
   auto extra_write = spawn_result<void>(executor, outbound.async_write(bytes{0x7f}));

   auto settle = boost::asio::steady_timer{executor};
   settle.expires_after(std::chrono::milliseconds{20});
   co_await settle.async_wait(boost::asio::use_awaitable);
   BOOST_TEST(!read->done.load(std::memory_order_acquire));
   BOOST_TEST(!extra_write->done.load(std::memory_order_acquire));

   release_next_write(pair.left_state);
   const auto received = co_await take_result_for(read, std::chrono::seconds{1});
   BOOST_TEST(received == full_window, boost::test_tools::per_element());
   co_await take_result_for(extra_write, std::chrono::seconds{1});

   release_pending_writes_if_any(pair.left_state);
   co_await left.async_close();
   auto close = spawn_result<void>(executor, right.async_close());
   co_await take_result_for(close, std::chrono::seconds{1});
}

boost::asio::awaitable<void> yamux_flow_control_retains_upstream_chunk_lifetime() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto options = forge::net::yamux::options{};
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, options};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder, options};

   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result(accept);

   auto owner = std::make_shared<int>(42);
   auto lifetime = std::weak_ptr<void>{owner};
   auto payload = forge::net::transport::chunk{deterministic_bytes(initial_stream_window + 1U)};
   const auto retained_payload_copy = payload;
   forge::net::transport::detail::chunk_access::attach_lifetime(payload, owner);
   owner.reset();

   auto write = spawn_result<void>(executor, outbound.async_write(std::move(payload)));
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{10});
   co_await timer.async_wait(boost::asio::use_awaitable);
   BOOST_TEST(!write->done.load(std::memory_order_acquire));
   BOOST_TEST(!lifetime.expired());

   static_cast<void>(co_await inbound.async_read());
   co_await take_result_for(write, std::chrono::seconds{1});
   BOOST_TEST(!retained_payload_copy.empty());
   BOOST_TEST(lifetime.expired());

   co_await left.async_close();
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_transport_drain_owns_upstream_chunk_lifetime() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};

   auto outbound = co_await left.async_open_stream();
   static_cast<void>(co_await pair.right.async_read());
   retain_write_lifetimes(pair.right_state, true);

   auto owner = std::make_shared<int>(42);
   auto lifetime = std::weak_ptr<void>{owner};
   auto payload = forge::net::transport::chunk{text_bytes("retained until transport drain")};
   forge::net::transport::detail::chunk_access::attach_lifetime(payload, owner);
   owner.reset();

   co_await outbound.async_write(std::move(payload));
   BOOST_TEST(!lifetime.expired());
   const auto received = co_await pair.right.async_read();
   BOOST_TEST(payload_of(received) == text_bytes("retained until transport drain"), boost::test_tools::per_element());
   BOOST_TEST(!lifetime.expired());

   drain_retained_writes(pair.right_state);
   BOOST_TEST(lifetime.expired());
   co_await pair.right.async_close();
   co_await left.async_close();
}

boost::asio::awaitable<void> yamux_close_flushes_pending_data_and_read_after_close_fails() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result(accept);

   const auto payload = text_bytes("flush-before-close");
   co_await outbound.async_write(payload);
   co_await outbound.async_close();
   auto received = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());
   BOOST_CHECK_EXCEPTION((void)co_await inbound.async_read(), forge::net::yamux::exceptions::closed,
       [](const auto& error) {
          return dynamic_cast<const forge::net::yamux::exceptions::end_of_stream*>(&error) &&
              error.code().value() == 5 &&
              forge::net::yamux::exceptions::code_of(error) == forge::net::yamux::exceptions::code::closed;
       });

   co_await left.async_close();
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_remote_fin_does_not_mask_reset_or_session_loss() {
   const auto executor = co_await boost::asio::this_coro::executor;
   for (const auto reset : {false, true}) {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      auto outbound = co_await left.async_open_stream();
      auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
      co_await outbound.async_close();
      auto fin = spawn_result<bytes>(executor, inbound.async_read());
      BOOST_CHECK_THROW((void)co_await take_result_for(fin, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::end_of_stream);
      if (reset) { inbound.request_cancel(); }
      else { right.cancel(); }
      BOOST_CHECK_EXCEPTION((void)co_await inbound.async_read(), forge::exceptions::base,
          [reset](const auto& error) {
             const auto code = forge::net::yamux::exceptions::code_of(error);
             return !dynamic_cast<const forge::net::yamux::exceptions::end_of_stream*>(&error) &&
                 (reset ? code == forge::net::yamux::exceptions::code::stream_reset
                        : code == forge::net::yamux::exceptions::code::canceled ||
                          code == forge::net::yamux::exceptions::code::closed ||
                          code == forge::net::yamux::exceptions::code::stream_reset);
          });
      for (auto* session : {&left, &right}) {
         try { co_await session->async_close(); }
         catch (const forge::net::yamux::exceptions::canceled&) {
            if (reset) { throw; }
         }
      }
   }
}

boost::asio::awaitable<void> yamux_limits_and_malformed_frames_are_typed() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      BOOST_CHECK_THROW((forge::net::yamux::session{
                            std::move(pair.right),
                            forge::net::yamux::side::responder,
                            forge::net::yamux::options{
                                .initial_window = initial_stream_window - 1U,
                            },
                        }),
                        forge::net::yamux::exceptions::invalid_options);
      co_await pair.left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      BOOST_CHECK_THROW((forge::net::yamux::session{
                            std::move(pair.right),
                            forge::net::yamux::side::responder,
                            forge::net::yamux::options{
                                .initial_window = initial_stream_window + 1U,
                                .max_stream_window = initial_stream_window,
                            },
                        }),
                        forge::net::yamux::exceptions::invalid_options);
      co_await pair.left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      BOOST_CHECK_THROW((forge::net::yamux::session{
                            std::move(pair.right),
                            forge::net::yamux::side::responder,
                            forge::net::yamux::options{
                                .max_stream_buffer = initial_stream_window - 1U,
                            },
                        }),
                        forge::net::yamux::exceptions::invalid_options);
      co_await pair.left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      BOOST_CHECK_THROW((forge::net::yamux::session{
                            std::move(pair.right),
                            forge::net::yamux::side::responder,
                            forge::net::yamux::options{
                                .max_session_buffer = initial_stream_window - 1U,
                            },
                        }),
                        forge::net::yamux::exceptions::invalid_options);
      co_await pair.left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      BOOST_CHECK_THROW((forge::net::yamux::session{
                            std::move(pair.right),
                            forge::net::yamux::side::responder,
                            forge::net::yamux::options{
                                .write_timeout = std::chrono::milliseconds{0},
                            },
                        }),
                        forge::net::yamux::exceptions::invalid_options);
      co_await pair.left.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                             forge::net::yamux::options{.max_frame_size = 3}};
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_frame_size = 3}};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      auto outbound = co_await left.async_open_stream();
      auto inbound = co_await take_result(accept);
      const auto payload = text_bytes("four");
      co_await outbound.async_write(payload);
      auto first = co_await inbound.async_read();
      auto second = co_await inbound.async_read();
      BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(), payload.begin(), payload.begin() + 3);
      BOOST_CHECK_EQUAL_COLLECTIONS(second.begin(), second.end(), payload.begin() + 3, payload.end());
      co_await left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto payload = text_bytes("early-data");
      co_await pair.left.async_write(
          frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(payload.size()), payload));
      auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(response), 1U);
      BOOST_CHECK_EQUAL(length_of(response), 0U);

      auto inbound = co_await take_result(accept);
      BOOST_CHECK_EQUAL(inbound.id(), 1);
      const auto received = co_await inbound.async_read();
      BOOST_TEST(received == payload, boost::test_tools::per_element());
      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(response), 1U);

      auto inbound = co_await take_result(accept);
      const auto payload = text_bytes("initial-window-response");
      auto write = spawn_result<void>(executor, inbound.async_write(payload));
      auto outbound = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(outbound), frame_type::data);
      BOOST_CHECK_EQUAL(stream_id_of(outbound), 1U);
      BOOST_CHECK_EQUAL(length_of(outbound), payload.size());
      co_await take_result_for(write, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{
                                                  .max_stream_window = initial_stream_window,
                                                  .max_frame_size = initial_stream_window + 1U,
                                                  .max_stream_buffer = initial_stream_window,
                                                  .max_session_buffer = initial_stream_window + 1U,
                                              }};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto oversized = deterministic_bytes(initial_stream_window + 1U);
      co_await write_transport_for_test(
          pair.left, frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(oversized.size()), oversized),
          "oversized DATA+SYN");
      const auto go_away = co_await read_transport_for_test(pair.left, "oversized DATA+SYN GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto malformed = frame(frame_type::data, 0, 1, 0);
      malformed[0] = 1;
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(malformed);
      BOOST_CHECK_THROW((void)co_await take_result(accept), forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::data, 0, 0, 0));
      BOOST_CHECK_THROW((void)co_await take_result(accept), forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_frame_size = 3}};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::data, 0, 1, 4));
      BOOST_CHECK_THROW((void)co_await take_result(accept), forge::net::yamux::exceptions::resource_limit);
      const auto go_away = co_await read_transport_for_test(pair.left, "oversized DATA GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 2U);
      co_await right.async_close();
      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_resource_overflow_resets_only_offending_stream() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{
          std::move(pair.right),
          forge::net::yamux::side::responder,
          forge::net::yamux::options{
              .max_stream_window = initial_stream_window,
              .max_stream_buffer = initial_stream_window + 1U,
              .max_session_buffer = initial_stream_window,
          },
      };
      auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto held_payload = deterministic_bytes(initial_stream_window, 0x44);
      co_await pair.left.async_write(
          frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(held_payload.size()), held_payload));
      BOOST_TEST_CHECKPOINT("session buffer overflow: waiting for first ACK");
      auto first_response = co_await read_transport_for_test(pair.left, "session-buffer first ACK");
      BOOST_CHECK_EQUAL(type_of(first_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(first_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(first_response), 1U);
      co_await pair.left.async_write(frame(frame_type::data, syn, 3, 1, text_bytes("x")));
      BOOST_TEST_CHECKPOINT("session buffer overflow: waiting for second ACK");
      auto second_response = co_await read_transport_for_test(pair.left, "session-buffer second ACK");
      BOOST_CHECK_EQUAL(type_of(second_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(second_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(second_response), 3U);
      BOOST_TEST_CHECKPOINT("session buffer overflow: waiting for second RST");
      auto second_reset = co_await read_transport_for_test(pair.left, "session-buffer second RST");
      BOOST_CHECK_EQUAL(type_of(second_reset), frame_type::data);
      BOOST_CHECK_EQUAL(flags_of(second_reset), rst);
      BOOST_CHECK_EQUAL(stream_id_of(second_reset), 3U);

      auto first = co_await take_result_for(accept_first, std::chrono::seconds{1});
      auto second = co_await right.async_accept_stream();
      BOOST_CHECK_EQUAL(first.id(), 1);
      BOOST_CHECK_EQUAL(second.id(), 3);
      BOOST_CHECK_THROW((void)co_await second.async_read(), forge::net::yamux::exceptions::stream_reset);
      auto received = co_await first.async_read();
      BOOST_TEST(received == held_payload, boost::test_tools::per_element());
      auto first_window = co_await read_transport_for_test(pair.left, "session-buffer first WINDOW_UPDATE");
      BOOST_CHECK_EQUAL(type_of(first_window), frame_type::window_update);
      BOOST_CHECK_EQUAL(stream_id_of(first_window), 1U);
      BOOST_CHECK_EQUAL(length_of(first_window), initial_stream_window);

      auto accept_third = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto payload = text_bytes("next");
      co_await pair.left.async_write(
          frame(frame_type::data, syn, 5, static_cast<std::uint32_t>(payload.size()), payload));
      BOOST_TEST_CHECKPOINT("session buffer overflow: waiting for third ACK");
      auto third_response = co_await read_transport_for_test(pair.left, "session-buffer third ACK");
      BOOST_CHECK_EQUAL(type_of(third_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(third_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(third_response), 5U);
      auto third = co_await take_result_for(accept_third, std::chrono::seconds{1});
      auto third_received = co_await third.async_read();
      BOOST_TEST(third_received == payload, boost::test_tools::per_element());

      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_parser_handles_partial_and_buffered_frames() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());

   const auto first_payload = text_bytes("one");
   const auto second_payload = text_bytes("two");
   auto first_frame = frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(first_payload.size()), first_payload);
   auto second_frame =
       frame(frame_type::data, syn, 3, static_cast<std::uint32_t>(second_payload.size()), second_payload);
   auto prefix = bytes{first_frame.begin(), first_frame.begin() + 5};
   auto remainder = bytes{first_frame.begin() + 5, first_frame.end()};
   append_bytes(remainder, second_frame);

   co_await pair.left.async_write(prefix);
   co_await pair.left.async_write(remainder);

   auto first_ack = co_await read_transport_for_test(pair.left, "parser first ACK");
   BOOST_CHECK_EQUAL(type_of(first_ack), frame_type::window_update);
   BOOST_CHECK_EQUAL(flags_of(first_ack), ack);
   BOOST_CHECK_EQUAL(stream_id_of(first_ack), 1U);
   auto second_ack = co_await read_transport_for_test(pair.left, "parser second ACK");
   BOOST_CHECK_EQUAL(type_of(second_ack), frame_type::window_update);
   BOOST_CHECK_EQUAL(flags_of(second_ack), ack);
   BOOST_CHECK_EQUAL(stream_id_of(second_ack), 3U);

   auto first = co_await take_result_for(accept_first, std::chrono::seconds{1});
   auto second = co_await right.async_accept_stream();
   BOOST_CHECK_EQUAL(first.id(), 1);
   BOOST_CHECK_EQUAL(second.id(), 3);
   auto first_received = co_await first.async_read();
   auto second_received = co_await second.async_read();
   BOOST_TEST(first_received == first_payload, boost::test_tools::per_element());
   BOOST_TEST(second_received == second_payload, boost::test_tools::per_element());

   co_await pair.left.async_close();
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_reset_reclaim_releases_buffer_budget_for_open_streams() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{
       std::move(pair.right),
       forge::net::yamux::side::responder,
       forge::net::yamux::options{
           .max_stream_window = initial_stream_window,
           .max_streams = 2,
           .max_pending_accepts = 2,
           .max_stream_buffer = initial_stream_window,
           .max_session_buffer = initial_stream_window,
       },
   };
   auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto first_ack = co_await read_transport_for_test(pair.left, "reclaim first ACK");
   BOOST_CHECK_EQUAL(type_of(first_ack), frame_type::window_update);
   BOOST_CHECK_EQUAL(flags_of(first_ack), ack);
   BOOST_CHECK_EQUAL(stream_id_of(first_ack), 1U);
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
   auto second_ack = co_await read_transport_for_test(pair.left, "reclaim second ACK");
   BOOST_CHECK_EQUAL(type_of(second_ack), frame_type::window_update);
   BOOST_CHECK_EQUAL(flags_of(second_ack), ack);
   BOOST_CHECK_EQUAL(stream_id_of(second_ack), 3U);
   auto first = co_await take_result_for(accept_first, std::chrono::seconds{1});
   auto second = co_await right.async_accept_stream();
   BOOST_CHECK_EQUAL(first.id(), 1);
   BOOST_CHECK_EQUAL(second.id(), 3);

   const auto held_payload = deterministic_bytes(initial_stream_window, 0x26);
   co_await pair.left.async_write(
       frame(frame_type::data, 0, 1, static_cast<std::uint32_t>(held_payload.size()), held_payload));
   co_await pair.left.async_write(frame(frame_type::data, rst, 1, 0));
   BOOST_CHECK_THROW((void)co_await first.async_read(), forge::net::yamux::exceptions::stream_reset);

   const auto payload = text_bytes("pass");
   co_await pair.left.async_write(frame(frame_type::data, 0, 3, static_cast<std::uint32_t>(payload.size()), payload));
   auto received = co_await second.async_read();
   BOOST_TEST(received == payload, boost::test_tools::per_element());

   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_configured_limits_are_behavioral() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator,
                                             forge::net::yamux::options{.max_streams = 1}};
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      auto first = co_await left.async_open_stream();
      auto inbound = co_await take_result(accept);
      BOOST_CHECK_EQUAL(first.id(), 1);
      BOOST_CHECK_EQUAL(inbound.id(), 1);
      BOOST_CHECK_THROW((void)co_await left.async_open_stream(), forge::net::yamux::exceptions::resource_limit);
      co_await left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_pending_accepts = 1}};
      auto local_open = spawn_result<forge::net::transport::stream>(executor, right.async_open_stream());
      auto local_syn = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(local_syn), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(local_syn), syn);
      BOOST_CHECK_EQUAL(stream_id_of(local_syn), 2U);
      (void)co_await take_result(local_open);
      auto local_reset = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(local_reset), frame_type::data);
      BOOST_CHECK_EQUAL(flags_of(local_reset), rst);
      BOOST_CHECK_EQUAL(stream_id_of(local_reset), 2U);

      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto first_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(first_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(first_response), 1U);
      BOOST_CHECK_EQUAL(length_of(first_response), 0U);

      co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
      auto second_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(second_response), frame_type::data);
      BOOST_CHECK_EQUAL(flags_of(second_response), rst);
      BOOST_CHECK_EQUAL(stream_id_of(second_response), 3U);

      auto accepted = co_await right.async_accept_stream();
      BOOST_CHECK_EQUAL(accepted.id(), 1);
      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{
          std::move(pair.right),
          forge::net::yamux::side::responder,
          forge::net::yamux::options{
              .max_stream_window = initial_stream_window,
              .max_frame_size = initial_stream_window + 1U,
              .max_stream_buffer = initial_stream_window + 1U,
              .max_session_buffer = initial_stream_window,
          },
      };
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto oversized = deterministic_bytes(initial_stream_window + 1U, 0x19);
      co_await write_transport_for_test(
          pair.left, frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(oversized.size()), oversized),
          "runtime-limit oversized DATA+SYN");
      const auto go_away = co_await read_transport_for_test(pair.left, "runtime-limit GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right =
          forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                     forge::net::yamux::options{.max_stream_window = initial_stream_window + 1U}};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(response), 1U);

      auto inbound = co_await take_result(accept);
      const auto payload = deterministic_bytes(initial_stream_window + 1U, 0x67);
      auto write = spawn_result<void>(executor, inbound.async_write(payload));
      auto first = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first), frame_type::data);
      BOOST_CHECK_EQUAL(stream_id_of(first), 1U);
      BOOST_CHECK_EQUAL(length_of(first), initial_stream_window);

      if (length_of(first) == initial_stream_window) {
         BOOST_CHECK(!write->done.load(std::memory_order_acquire));
         co_await pair.left.async_write(frame(frame_type::window_update, 0, 1, 1));
         auto second = co_await pair.left.async_read();
         BOOST_CHECK_EQUAL(type_of(second), frame_type::data);
         BOOST_CHECK_EQUAL(stream_id_of(second), 1U);
         BOOST_CHECK_EQUAL(length_of(second), 1U);
      }
      co_await take_result_for(write, std::chrono::seconds{1});
      co_await pair.left.async_close();
      co_await right.async_close();
   }
}

boost::asio::awaitable<void> yamux_rejects_invalid_peer_window_updates() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right =
          forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                     forge::net::yamux::options{.max_stream_window = initial_stream_window + 8U}};
      auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());

      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 9));
      const auto go_away = co_await read_transport_for_test(pair.left, "excess SYN window GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto left =
          forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::initiator,
                                     forge::net::yamux::options{.max_stream_window = initial_stream_window + 8U}};
      auto outbound = co_await left.async_open_stream();
      (void)co_await pair.left.async_read();
      auto terminal = spawn_result<forge::net::transport::stream>(executor, left.async_accept_stream());

      co_await pair.left.async_write(frame(frame_type::window_update, ack, 1, 9));
      const auto go_away = co_await read_transport_for_test(pair.left, "excess window GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      BOOST_CHECK_THROW((void)co_await outbound.async_write(text_bytes("x")),
                        forge::net::yamux::exceptions::stream_reset);
      BOOST_TEST(!outbound.valid());
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto left = forge::net::yamux::session{
          std::move(pair.right), forge::net::yamux::side::initiator,
          forge::net::yamux::options{.max_stream_window = (std::numeric_limits<std::uint32_t>::max)()}};
      auto outbound = co_await left.async_open_stream();
      (void)co_await pair.left.async_read();
      auto terminal = spawn_result<forge::net::transport::stream>(executor, left.async_accept_stream());

      co_await pair.left.async_write(
          frame(frame_type::window_update, ack, 1, (std::numeric_limits<std::uint32_t>::max)()));
      const auto go_away = co_await read_transport_for_test(pair.left, "overflow window GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      BOOST_CHECK_THROW((void)co_await outbound.async_write(text_bytes("x")),
                        forge::net::yamux::exceptions::stream_reset);
      BOOST_TEST(!outbound.valid());
      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_rejects_receive_over_credit() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accepted = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());

      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      (void)co_await read_transport_for_test(pair.left, "receive-credit setup ACK");
      auto inbound = co_await take_result_for(accepted, std::chrono::seconds{1});

      const auto full_window = deterministic_bytes(initial_stream_window, 0x41);
      co_await write_transport_for_test(pair.left, frame(frame_type::data, 0, 1, initial_stream_window, full_window),
                                        "full receive window DATA");
      co_await write_transport_for_test(pair.left, frame(frame_type::data, 0, 1, 1, bytes{0x7f}), "over-credit DATA");
      const auto go_away = co_await read_transport_for_test(pair.left, "receive over-credit GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);

      auto read = spawn_result<bytes>(executor, inbound.async_read());
      BOOST_CHECK_THROW((void)co_await take_result_for(read, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);
      co_await right.async_close();
      auto read_after_close = spawn_result<bytes>(executor, inbound.async_read());
      BOOST_CHECK_THROW((void)co_await take_result_for(read_after_close, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_frame_size = initial_stream_window + 1U}};
      auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto oversized = deterministic_bytes(initial_stream_window + 1U, 0x62);

      co_await write_transport_for_test(
          pair.left, frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(oversized.size()), oversized),
          "over-credit DATA+SYN");
      const auto go_away = co_await read_transport_for_test(pair.left, "DATA+SYN over-credit GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{
                                                  .initial_window = initial_stream_window + 1U,
                                                  .max_stream_window = initial_stream_window + 1U,
                                                  .max_frame_size = initial_stream_window + 1U,
                                              }};
      auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      const auto oversized = deterministic_bytes(initial_stream_window + 1U, 0x63);

      co_await write_transport_for_test(
          pair.left, frame(frame_type::data, syn, 1, static_cast<std::uint32_t>(oversized.size()), oversized),
          "asymmetric over-credit DATA+SYN");
      const auto go_away = co_await read_transport_for_test(pair.left, "asymmetric DATA+SYN over-credit GO_AWAY");
      BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
      BOOST_CHECK_EQUAL(length_of(go_away), 1U);
      BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::protocol_error);
      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_late_notifications_are_sticky() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto opening = spawn_result<forge::net::transport::stream>(executor, right.async_open_stream());
   const auto local_syn = co_await read_transport_for_test(pair.left, "late-wake local SYN");
   BOOST_CHECK_EQUAL(flags_of(local_syn), syn);
   auto outbound = co_await take_result_for(opening, std::chrono::seconds{1});

   co_await write_transport_for_test(pair.left, frame(frame_type::window_update, syn, 1, 0), "late accept SYN");
   const auto remote_ack = co_await read_transport_for_test(pair.left, "late accept ACK");
   BOOST_CHECK_EQUAL(flags_of(remote_ack), ack);
   auto accepting = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto inbound = co_await take_result_for(accepting, std::chrono::seconds{1});

   const auto late_payload = text_bytes("late-read");
   co_await write_transport_for_test(
       pair.left, frame(frame_type::data, 0, 1, static_cast<std::uint32_t>(late_payload.size()), late_payload),
       "late read DATA");
   co_await write_transport_for_test(pair.left, frame(frame_type::ping, 0, 0, 0x1234), "late read ordering PING");
   const auto ping_ack = co_await read_transport_for_test(pair.left, "late read ordering PING ACK");
   BOOST_CHECK_EQUAL(type_of(ping_ack), frame_type::ping);

   auto reading = spawn_result<bytes>(executor, inbound.async_read());
   const auto received = co_await take_result_for(reading, std::chrono::seconds{1});
   BOOST_TEST(received == late_payload, boost::test_tools::per_element());
   const auto replenished = co_await read_transport_for_test(pair.left, "late read WINDOW_UPDATE");
   BOOST_CHECK_EQUAL(type_of(replenished), frame_type::window_update);
   BOOST_CHECK_EQUAL(length_of(replenished), late_payload.size());

   const auto full_window = deterministic_bytes(initial_stream_window, 0x27);
   auto filling = spawn_result<void>(executor, outbound.async_write(full_window));
   const auto full_data = co_await read_transport_for_test(pair.left, "window exhaustion DATA");
   BOOST_CHECK_EQUAL(length_of(full_data), initial_stream_window);
   co_await take_result_for(filling, std::chrono::seconds{1});

   co_await write_transport_for_test(pair.left, frame(frame_type::window_update, 0, 2, 1), "late send-window update");
   co_await write_transport_for_test(pair.left, frame(frame_type::ping, 0, 0, 0x5678), "late window ordering PING");
   const auto window_ping_ack = co_await read_transport_for_test(pair.left, "late window ordering PING ACK");
   BOOST_CHECK_EQUAL(type_of(window_ping_ack), frame_type::ping);

   auto late_write = spawn_result<void>(executor, outbound.async_write(bytes{0x55}));
   const auto late_data = co_await read_transport_for_test(pair.left, "late window DATA");
   BOOST_CHECK_EQUAL(type_of(late_data), frame_type::data);
   BOOST_CHECK_EQUAL(length_of(late_data), 1U);
   co_await take_result_for(late_write, std::chrono::seconds{1});

   co_await close_transport_for_test(pair.left);
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_reclaims_terminal_streams_before_enforcing_stream_cap() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_streams = 1}};
      auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto first_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(first_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(first_response), 1U);
      auto first = co_await take_result(accept_first);

      co_await pair.left.async_write(frame(frame_type::data, fin, 1, 0));
      co_await first.async_close();
      auto first_fin = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first_fin), frame_type::data);
      BOOST_CHECK_EQUAL(flags_of(first_fin), fin);
      BOOST_CHECK_EQUAL(stream_id_of(first_fin), 1U);

      auto accept_second = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
      auto second_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(second_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(second_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(second_response), 3U);
      auto second = co_await take_result_for(accept_second, std::chrono::seconds{1});
      BOOST_CHECK_EQUAL(second.id(), 3);

      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_streams = 1}};
      auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto first_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(first_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(first_response), 1U);
      auto first = co_await take_result(accept_first);
      BOOST_CHECK_EQUAL(first.id(), 1);

      co_await pair.left.async_write(frame(frame_type::data, rst, 1, 0));

      auto accept_second = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
      auto second_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(second_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(second_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(second_response), 3U);
      auto second = co_await take_result_for(accept_second, std::chrono::seconds{1});
      BOOST_CHECK_EQUAL(second.id(), 3);

      auto reclaimed_read = spawn_result<bytes>(executor, first.async_read());
      BOOST_CHECK_THROW((void)co_await take_result_for(reclaimed_read, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);
      auto reclaimed_write = spawn_result<void>(executor, first.async_write(text_bytes("stale")));
      BOOST_CHECK_THROW((void)co_await take_result_for(reclaimed_write, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::stream_reset);

      co_await pair.left.async_close();
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                              forge::net::yamux::options{.max_streams = 1}};
      auto accept_first = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      auto first_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(first_response), frame_type::window_update);
      BOOST_CHECK_EQUAL(flags_of(first_response), ack);
      BOOST_CHECK_EQUAL(stream_id_of(first_response), 1U);
      auto first = co_await take_result(accept_first);
      BOOST_CHECK_EQUAL(first.id(), 1);

      co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
      auto second_response = co_await pair.left.async_read();
      BOOST_CHECK_EQUAL(type_of(second_response), frame_type::data);
      BOOST_CHECK_EQUAL(flags_of(second_response), rst);
      BOOST_CHECK_EQUAL(stream_id_of(second_response), 3U);

      co_await pair.left.async_close();
      co_await right.async_close();
   }
}

boost::asio::awaitable<void> yamux_ignores_late_frames_for_reclaimed_streams() {
   auto executor = co_await boost::asio::this_coro::executor;
   for (const auto reset_first : {false, true}) {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto first_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
      (void)co_await read_transport_for_test(pair.left, "reclaimed stream setup ACK");
      auto first = co_await take_result_for(first_accept, std::chrono::seconds{1});

      if (reset_first) {
         co_await pair.left.async_write(frame(frame_type::data, rst, 1, 0));
         auto terminal = spawn_result<bytes>(executor, first.async_read());
         BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                           forge::net::yamux::exceptions::stream_reset);
      } else {
         const auto terminal_payload = bytes{0x33, 0x44};
         co_await pair.left.async_write(frame(frame_type::data, fin, 1, terminal_payload.size(), terminal_payload));
         co_await first.async_close();
         const auto local_fin = co_await read_transport_for_test(pair.left, "reclaimed stream FIN");
         BOOST_CHECK_EQUAL(flags_of(local_fin), fin);

         co_await pair.left.async_write(frame(frame_type::data, rst, 1, 0));
         co_await pair.left.async_write(frame(frame_type::window_update, rst, 1, 0));
         auto preserved = co_await first.async_read();
         BOOST_CHECK_EQUAL_COLLECTIONS(preserved.begin(), preserved.end(), terminal_payload.begin(),
                                       terminal_payload.end());
         const auto preserved_credit = co_await read_transport_for_test(pair.left, "terminal payload credit");
         BOOST_CHECK_EQUAL(type_of(preserved_credit), frame_type::window_update);
         BOOST_CHECK_EQUAL(stream_id_of(preserved_credit), 1U);
         BOOST_CHECK_EQUAL(length_of(preserved_credit), terminal_payload.size());
         auto remote_fin = spawn_result<bytes>(executor, first.async_read());
         BOOST_CHECK_THROW((void)co_await take_result_for(remote_fin, std::chrono::seconds{1}),
                           forge::net::yamux::exceptions::closed);
      }

      co_await pair.left.async_write(frame(frame_type::data, 0, 1, 1, bytes{0x44}));
      co_await pair.left.async_write(frame(frame_type::window_update, 0, 1, 1));

      auto next_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
      const auto next_ack = co_await read_transport_for_test(pair.left, "post-reclaim ACK");
      BOOST_CHECK_EQUAL(flags_of(next_ack), ack);
      BOOST_CHECK_EQUAL(stream_id_of(next_ack), 3U);
      auto next = co_await take_result_for(next_accept, std::chrono::seconds{1});
      BOOST_TEST(next.valid());

      co_await close_transport_for_test(pair.left);
      co_await right.async_close();
   }
}

boost::asio::awaitable<void> yamux_concurrent_close_owns_one_transport_close() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "yamux stream ACK before close");

   hold_writes(pair.left_state, true);
   auto writes_before_close = std::uint64_t{};
   auto write_calls_before_close = std::uint64_t{};
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      writes_before_close = pair.left_state->writes;
      write_calls_before_close = pair.left_state->write_calls;
   }

   auto first = spawn_result<void>(executor, right.async_close());
   auto second = spawn_result<void>(executor, right.async_close());
   co_await wait_for_pending_writes(pair.left_state, 1);
   inbound.cancel();
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.left_state->pending_writes.size(), 1U);
   }
   release_next_write(pair.left_state);
   co_await take_result_for(first, std::chrono::seconds{1});
   co_await take_result_for(second, std::chrono::seconds{1});
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 1U);
   }
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_CHECK_EQUAL(pair.left_state->writes, writes_before_close + 1U);
      BOOST_CHECK_EQUAL(pair.left_state->write_calls, write_calls_before_close + 1U);
      BOOST_CHECK(pair.left_state->pending_writes.empty());
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_close_waits_for_admitted_write_after_remote_go_away_scenario() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "yamux stream ACK before remote GO_AWAY");

   hold_writes(pair.left_state, true);
   auto write = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);
   auto barrier_state = std::make_shared<lifetime_barrier_state>();
   auto barrier = std::make_shared<lifetime_barrier>(barrier_state);
   auto queued_payload = forge::net::transport::chunk{bytes{0x42}};
   forge::net::transport::detail::chunk_access::attach_lifetime(queued_payload, barrier);
   barrier.reset();
   auto queued_write = spawn_result<void>(executor, inbound.async_write(std::move(queued_payload)));
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_REQUIRE(!queued_write->done.load(std::memory_order_acquire));
   co_await pair.left.async_write(frame(frame_type::go_away, 0, 0, 0));
   for (auto attempt = 0; attempt < 100 && right.valid(); ++attempt) {
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }
   BOOST_REQUIRE(!right.valid());

   auto close = spawn_result<void>(executor, right.async_close());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 0U);
   }

   const auto observed = barrier_state->changed.epoch();
   release_pending_writes_if_any(pair.left_state);
   if (!barrier_state->entered.load(std::memory_order_acquire)) {
      (void)co_await barrier_state->changed.async_wait_until(observed, std::chrono::steady_clock::now() +
                                                                           std::chrono::seconds{1});
   }
   BOOST_REQUIRE(barrier_state->entered.load(std::memory_order_acquire));
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 0U);
   }
   co_await take_result_for(write, std::chrono::seconds{1});
   BOOST_CHECK_THROW((void)co_await take_result_for(queued_write, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
   co_await take_result_for(close, std::chrono::seconds{1});
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_protocol_error_is_terminal_before_go_away_drain() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "yamux stream ACK before malformed frame");

   hold_writes(pair.left_state, true);
   auto admitted = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);

   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::milliseconds{250}),
                     forge::net::yamux::exceptions::protocol_error);
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_CHECK_EQUAL(pair.left_state->pending_writes.size(), 1U);
   }

   release_pending_writes_if_any(pair.left_state);
   co_await take_result_for(admitted, std::chrono::seconds{1});
   const auto data = co_await read_transport_for_test(pair.left, "admitted DATA before error GO_AWAY");
   BOOST_CHECK_EQUAL(type_of(data), frame_type::data);
   const auto go_away = co_await read_transport_for_test(pair.left, "error GO_AWAY after admitted DATA");
   BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
   BOOST_CHECK_EQUAL(length_of(go_away), 1U);
   auto eof = spawn_result<bytes>(executor, pair.left.async_read());
   BOOST_CHECK_THROW((void)co_await take_result_for(eof, std::chrono::seconds{1}),
                     forge::net::transport::exceptions::closed);

   co_await right.async_close();
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_failed_error_go_away_preserves_protocol_error() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   fail_next_write(pair.left_state);

   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::protocol_error);
   auto next = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   BOOST_CHECK_THROW((void)co_await take_result_for(next, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::protocol_error);
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_CHECK_EQUAL(pair.left_state->failed_writes, 1U);
   }

   co_await right.async_close();
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_protocol_error_deadline_cancels_blocked_transport() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   pair.right_state->throw_after_cancel = true;
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder,
                                           forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25}}};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "yamux stream ACK before blocked protocol failure");

   hold_writes(pair.left_state, true);
   auto admitted = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);

   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::milliseconds{250}),
                     forge::net::yamux::exceptions::protocol_error);
   BOOST_CHECK_THROW((void)co_await inbound.async_read(), forge::net::yamux::exceptions::stream_reset);
   BOOST_CHECK_THROW((void)co_await take_result_for(admitted, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
   auto next = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   BOOST_CHECK_THROW((void)co_await take_result_for(next, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::protocol_error);
   {
      auto lock = std::scoped_lock{pair.left_state->mutex, pair.right_state->mutex};
      BOOST_CHECK(pair.left_state->closed);
      BOOST_CHECK(pair.right_state->closed);
      BOOST_CHECK(pair.left_state->reads.empty());
      BOOST_CHECK_GE(pair.right_state->cancel_calls, 1U);
   }

   co_await right.async_close();
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_protocol_error_cancels_blocked_reset_writer() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{
       std::move(pair.right), forge::net::yamux::side::responder,
       forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25}}};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "blocked reset protocol failure setup ACK");

   hold_writes(pair.left_state, true);
   inbound.request_cancel();
   co_await wait_for_pending_writes(pair.left_state, 1);

   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::milliseconds{250}),
                     forge::net::yamux::exceptions::protocol_error);

   co_await right.async_close();
   {
      auto lock = std::scoped_lock{pair.left_state->mutex, pair.right_state->mutex};
      BOOST_CHECK_GE(pair.right_state->cancel_calls, 1U);
      BOOST_TEST(pair.left_state->closed);
      BOOST_TEST(pair.right_state->closed);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_cancel_during_active_frame_write_finishes_frame_then_resets_stream() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{
       std::move(pair.right), forge::net::yamux::side::responder,
       forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25},
                                  .write_timeout = std::chrono::milliseconds{250}}};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "active frame cancellation setup ACK");

   hold_writes(pair.left_state, true);
   auto write = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);
   inbound.request_cancel();

   release_pending_writes_if_any(pair.left_state);
   co_await take_result_for(write, std::chrono::seconds{1});
   const auto data = co_await read_transport_for_test(pair.left, "completed DATA before canceled stream reset");
   BOOST_CHECK_EQUAL(type_of(data), frame_type::data);
   BOOST_CHECK_EQUAL(flags_of(data), 0U);
   const auto reset = co_await read_transport_for_test(pair.left, "stream-local reset after completed DATA");
   BOOST_CHECK_EQUAL(type_of(reset), frame_type::data);
   BOOST_CHECK_EQUAL(flags_of(reset), rst);
   BOOST_TEST(right.valid());
   {
      auto lock = std::scoped_lock{pair.left_state->mutex, pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->cancel_calls, 0U);
      BOOST_TEST(!pair.left_state->closed);
      BOOST_TEST(!pair.right_state->closed);
   }

   pair.left.request_cancel();
   co_await right.async_close();
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_stalled_frame_write_expires_and_closes_session() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{
       std::move(pair.right), forge::net::yamux::side::responder,
       forge::net::yamux::options{.close_timeout = std::chrono::milliseconds{25},
                                  .write_timeout = std::chrono::milliseconds{25}}};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "stalled frame timeout setup ACK");

   hold_writes(pair.left_state, true);
   auto write = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);
   inbound.request_cancel();

   BOOST_CHECK_THROW((void)co_await take_result_for(write, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::closed);
   BOOST_TEST(!right.valid());
   {
      auto lock = std::scoped_lock{pair.left_state->mutex, pair.right_state->mutex};
      BOOST_CHECK_GE(pair.right_state->cancel_calls, 1U);
      BOOST_TEST(pair.left_state->closed);
      BOOST_TEST(pair.right_state->closed);
   }

   co_await right.async_close();
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_maximum_write_timeout_does_not_wrap() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   const auto limits = forge::net::yamux::options{
       .write_timeout = (std::chrono::milliseconds::max)(),
   };
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator, limits};
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder, limits};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});

   const auto payload = text_bytes("maximum timeout remains in the future");
   co_await outbound.async_write(payload);
   const auto received = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   co_await left.async_close();
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_failed_first_syn_write_terminalizes_session() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   fail_next_write_allocation(pair.right_state);

   BOOST_CHECK_THROW((void)co_await left.async_open_stream(), forge::net::yamux::exceptions::closed);
   BOOST_TEST(!left.valid());
   BOOST_CHECK_THROW((void)co_await left.async_open_stream(), forge::net::yamux::exceptions::closed);

   co_await left.async_close();
   co_await close_transport_for_test(pair.right);
}

boost::asio::awaitable<void> yamux_start_failure_close_waits_for_lower_terminal_cleanup() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   defer_pipe_cancel(pair.right_state, true);
   hold_pipe_close(pair.right_state, true);
   forge::net::yamux::detail::fail_next_session_start_for_test();
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto close = spawn_result<void>(executor, right.async_close());
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
   while (std::chrono::steady_clock::now() < deadline) {
      {
         auto lock = std::scoped_lock{pair.right_state->mutex};
         if (pair.right_state->close_calls == 1U) {
            break;
         }
      }
      const auto observed = pair.right_state->changed.epoch();
      (void)co_await pair.right_state->changed.async_wait_until(observed, deadline);
   }
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.right_state->close_calls, 1U);
      BOOST_TEST(!pair.right_state->closed);
   }
   BOOST_TEST(!close->done.load(std::memory_order_acquire));

   hold_pipe_close(pair.right_state, false);
   BOOST_CHECK_THROW((void)co_await take_result_for(close, std::chrono::seconds{1}), std::bad_alloc);
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->closed);
      BOOST_TEST(pair.right_state->close_calls == 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_normal_close_failure_waits_for_lower_terminal_cleanup() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   defer_pipe_cancel(pair.right_state, true);
   hold_pipe_close(pair.right_state, true);
   forge::net::yamux::detail::fail_next_session_close_for_test();
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto close = spawn_result<void>(executor, right.async_close());
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
   while (std::chrono::steady_clock::now() < deadline) {
      {
         auto lock = std::scoped_lock{pair.right_state->mutex};
         if (pair.right_state->close_calls == 1U) {
            break;
         }
      }
      const auto observed = pair.right_state->changed.epoch();
      (void)co_await pair.right_state->changed.async_wait_until(observed, deadline);
   }
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.right_state->close_calls, 1U);
      BOOST_TEST(!pair.right_state->closed);
   }
   BOOST_TEST(!close->done.load(std::memory_order_acquire));

   hold_pipe_close(pair.right_state, false);
   BOOST_CHECK_THROW((void)co_await take_result_for(close, std::chrono::seconds{1}), std::bad_alloc);
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->closed);
      BOOST_TEST(pair.right_state->close_calls == 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_close_joins_lower_terminal_exception_barrier() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   defer_pipe_cancel(pair.right_state, true);
   hold_pipe_close(pair.right_state, true);
   fail_next_pipe_close_after_terminal(pair.right_state);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};

   auto first = spawn_result<void>(executor, right.async_close());
   auto second = spawn_result<void>(executor, right.async_close());
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
   while (std::chrono::steady_clock::now() < deadline) {
      {
         auto lock = std::scoped_lock{pair.right_state->mutex};
         if (pair.right_state->close_calls == 1U) {
            break;
         }
      }
      const auto observed = pair.right_state->changed.epoch();
      (void)co_await pair.right_state->changed.async_wait_until(observed, deadline);
   }
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.right_state->close_calls, 1U);
      BOOST_TEST(!pair.right_state->closed);
   }
   BOOST_TEST(!first->done.load(std::memory_order_acquire));
   BOOST_TEST(!second->done.load(std::memory_order_acquire));

   hold_pipe_close(pair.right_state, false);
   co_await take_result_for(first, std::chrono::seconds{1});
   co_await take_result_for(second, std::chrono::seconds{1});
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->closed);
      BOOST_TEST(pair.right_state->close_calls == 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_preterminal_stream_close_failure_resets_before_cached_error() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto outbound = co_await left.async_open_stream();
   (void)co_await read_transport_for_test(pair.right, "stream-close failure SYN");

   hold_writes(pair.right_state, true);
   forge::net::yamux::detail::fail_next_stream_close_for_test();
   auto first = spawn_result<void>(executor, outbound.async_close());
   co_await wait_for_pending_writes(pair.right_state, 1);
   auto second = spawn_result<void>(executor, outbound.async_close());
   BOOST_TEST(!first->done.load(std::memory_order_acquire));
   BOOST_TEST(!second->done.load(std::memory_order_acquire));

   release_next_write(pair.right_state);
   const auto reset = co_await read_transport_for_test(pair.right, "stream-close failure RST");
   BOOST_CHECK_EQUAL(type_of(reset), frame_type::data);
   BOOST_CHECK_EQUAL(flags_of(reset), rst);
   BOOST_CHECK_EQUAL(stream_id_of(reset), 1U);

   BOOST_CHECK_THROW((void)co_await take_result_for(first, std::chrono::seconds{1}), std::bad_alloc);
   BOOST_CHECK_THROW((void)co_await take_result_for(second, std::chrono::seconds{1}), std::bad_alloc);
   BOOST_TEST(!outbound.valid());

   co_await pair.right.async_close();
   co_await left.async_close();
}

boost::asio::awaitable<void> yamux_preterminal_stream_close_failure_joins_reset_writer_terminal_error() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::session{std::move(pair.left), forge::net::yamux::side::initiator};
   auto outbound = co_await left.async_open_stream();
   (void)co_await read_transport_for_test(pair.right, "stream-close reset writer failure SYN");

   // The compensating RST fails below Yamux and terminalizes the session while
   // close_stream is waiting for this stream's terminal transition.
   fail_next_write(pair.right_state);
   forge::net::yamux::detail::fail_next_stream_close_for_test();
   auto first = spawn_result<void>(executor, outbound.async_close());
   auto second = spawn_result<void>(executor, outbound.async_close());
   BOOST_CHECK_THROW((void)co_await take_result_for(first, std::chrono::seconds{1}), std::bad_alloc);
   BOOST_CHECK_THROW((void)co_await take_result_for(second, std::chrono::seconds{1}), std::bad_alloc);
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->failed_writes == 1U);
   }

   co_await left.async_close();
   co_await close_transport_for_test(pair.right);
}

boost::asio::awaitable<void> yamux_failed_terminal_go_away_waits_for_lower_terminal_cleanup() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   defer_pipe_cancel(pair.right_state, true);
   hold_pipe_close(pair.right_state, true);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accepted = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   fail_next_write(pair.left_state);

   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   BOOST_CHECK_THROW((void)co_await take_result_for(accepted, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::protocol_error);

   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{1};
   while (std::chrono::steady_clock::now() < deadline) {
      {
         auto lock = std::scoped_lock{pair.right_state->mutex};
         if (pair.right_state->close_calls == 1U) {
            break;
         }
      }
      const auto observed = pair.right_state->changed.epoch();
      (void)co_await pair.right_state->changed.async_wait_until(observed, deadline);
   }
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.right_state->close_calls, 1U);
      BOOST_TEST(!pair.right_state->closed);
      BOOST_TEST(pair.left_state->failed_writes == 1U);
   }

   auto close = spawn_result<void>(executor, right.async_close());
   BOOST_TEST(!close->done.load(std::memory_order_acquire));
   hold_pipe_close(pair.right_state, false);
   co_await take_result_for(close, std::chrono::seconds{1});
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->closed);
      BOOST_TEST(pair.right_state->close_calls == 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_protocol_failure_claims_error_go_away_before_reset_writer_drain() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "protocol close classification setup ACK");

   hold_writes(pair.left_state, true);
   inbound.request_cancel();
   co_await wait_for_pending_writes(pair.left_state, 1);

   auto terminal = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   BOOST_CHECK_THROW((void)co_await take_result_for(terminal, std::chrono::seconds{1}),
                     forge::net::yamux::exceptions::protocol_error);

   auto close = spawn_result<void>(executor, right.async_close());
   BOOST_TEST(!close->done.load(std::memory_order_acquire));

   release_next_write(pair.left_state);
   const auto reset = co_await read_transport_for_test(pair.left, "reset before terminal GO_AWAY");
   BOOST_CHECK_EQUAL(type_of(reset), frame_type::data);
   BOOST_CHECK_EQUAL(flags_of(reset), rst);

   co_await wait_for_pending_writes(pair.left_state, 1);
   release_next_write(pair.left_state);
   co_await take_result_for(close, std::chrono::seconds{1});
   const auto go_away = co_await read_transport_for_test(pair.left, "terminal protocol GO_AWAY");
   BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
   BOOST_CHECK_EQUAL(length_of(go_away), 1U);
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_TEST(pair.right_state->close_calls == 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_graceful_close_and_protocol_failure_share_terminal_owner() {
   BOOST_TEST_CHECKPOINT("creating close/error race fixture");
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   auto inbound = co_await take_result_for(accept, std::chrono::seconds{1});
   (void)co_await read_transport_for_test(pair.left, "yamux stream ACK before close/error race");

   BOOST_TEST_CHECKPOINT("starting graceful close owner");
   hold_writes(pair.left_state, true);
   auto admitted = spawn_result<void>(executor, inbound.async_write(bytes{0x41}));
   co_await wait_for_pending_writes(pair.left_state, 1);
   auto close = spawn_result<void>(executor, right.async_close());
   for (auto attempt = 0; attempt < 100 && right.valid(); ++attempt) {
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
   }
   BOOST_REQUIRE(!right.valid());

   BOOST_TEST_CHECKPOINT("injecting protocol error during graceful close");
   auto malformed = frame(frame_type::data, 0, 1, 0);
   malformed[0] = 1;
   co_await pair.left.async_write(malformed);
   auto protocol_error_observed = false;
   for (auto attempt = 0; attempt < 100 && !protocol_error_observed; ++attempt) {
      auto yield = false;
      try {
         (void)co_await right.async_open_stream();
      } catch (const forge::exceptions::base& error) {
         if (forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::protocol_error)) {
            protocol_error_observed = true;
         } else if (forge::net::yamux::exceptions::is(error, forge::net::yamux::exceptions::code::closed)) {
            yield = true;
         } else {
            throw;
         }
      }
      if (yield) {
         co_await boost::asio::post(executor, boost::asio::use_awaitable);
      }
   }
   BOOST_REQUIRE(protocol_error_observed);
   BOOST_CHECK_THROW((void)co_await inbound.async_read(), forge::net::yamux::exceptions::stream_reset);

   BOOST_TEST_CHECKPOINT("releasing admitted DATA before terminal write");
   release_next_write(pair.left_state);
   co_await take_result_for(admitted, std::chrono::seconds{1});
   const auto data = co_await read_transport_for_test(pair.left, "admitted DATA before raced GO_AWAY");
   BOOST_CHECK_EQUAL(type_of(data), frame_type::data);
   co_await wait_for_pending_writes(pair.left_state, 1);
   release_next_write(pair.left_state);
   co_await take_result_for(close, std::chrono::seconds{1});
   auto go_away = bytes{};
   {
      auto lock = std::scoped_lock{pair.left_state->mutex};
      BOOST_REQUIRE_EQUAL(pair.left_state->reads.size(), 1U);
      go_away = pair.left_state->reads.front();
   }
   BOOST_CHECK_EQUAL(type_of(go_away), frame_type::go_away);
   BOOST_CHECK_EQUAL(length_of(go_away), 1U);
   BOOST_TEST_CHECKPOINT("checking single lower close");
   {
      auto lock = std::scoped_lock{pair.right_state->mutex};
      BOOST_CHECK_EQUAL(pair.right_state->close_calls, 1U);
   }
   co_await close_transport_for_test(pair.left);
}

boost::asio::awaitable<void> yamux_accepts_concurrent_remote_opens_out_of_order() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
   auto first_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto second_accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());

   co_await pair.left.async_write(frame(frame_type::window_update, syn, 3, 0));
   const auto higher_ack = co_await read_transport_for_test(pair.left, "out-of-order stream 3 ACK");
   BOOST_CHECK_EQUAL(stream_id_of(higher_ack), 3U);

   co_await pair.left.async_write(frame(frame_type::window_update, syn, 1, 0));
   const auto lower_ack = co_await read_transport_for_test(pair.left, "out-of-order stream 1 ACK");
   BOOST_CHECK_EQUAL(stream_id_of(lower_ack), 1U);

   auto higher = co_await take_result_for(first_accept, std::chrono::seconds{1});
   auto lower = co_await take_result_for(second_accept, std::chrono::seconds{1});
   BOOST_CHECK_EQUAL(higher.id(), 3);
   BOOST_CHECK_EQUAL(lower.id(), 1);

   co_await close_transport_for_test(pair.left);
   co_await right.async_close();
}

boost::asio::awaitable<void> yamux_control_frames_are_handled() {
   auto executor = co_await boost::asio::this_coro::executor;
   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::ping, 0, 0, 0x01020304));
      auto response = co_await pair.left.async_read();
      BOOST_REQUIRE_EQUAL(response.size(), 12U);
      BOOST_CHECK_EQUAL(response[1], static_cast<std::uint8_t>(frame_type::ping));
      BOOST_CHECK_EQUAL(response[3], 0x02U);
      BOOST_CHECK_EQUAL(response[8], 0x01U);
      BOOST_CHECK_EQUAL(response[9], 0x02U);
      BOOST_CHECK_EQUAL(response[10], 0x03U);
      BOOST_CHECK_EQUAL(response[11], 0x04U);
      co_await pair.left.async_close();
      BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::closed);
      co_await right.async_close();
   }

   {
      auto pair = make_stream_pair(executor);
      auto right = forge::net::yamux::session{std::move(pair.right), forge::net::yamux::side::responder};
      auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
      co_await pair.left.async_write(frame(frame_type::go_away, 0, 0, 0));
      BOOST_CHECK_THROW((void)co_await take_result_for(accept, std::chrono::seconds{1}),
                        forge::net::yamux::exceptions::closed);
      co_await wait_for_active_reads(pair.right_state, 0);
      BOOST_CHECK_EQUAL(active_reads(pair.right_state), 0U);

      co_await right.async_close();
      co_await close_transport_for_test(pair.left);
   }
}

boost::asio::awaitable<void> yamux_transport_session_wrapper_delegates() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pair = make_stream_pair(executor);
   auto left = forge::net::yamux::make_session(std::move(pair.left), forge::net::yamux::side::initiator);
   auto right = forge::net::yamux::make_session(std::move(pair.right), forge::net::yamux::side::responder);

   auto accept = spawn_result<forge::net::transport::stream>(executor, right.async_accept_stream());
   auto outbound = co_await left.async_open_stream();
   auto inbound = co_await take_result(accept);
   const auto payload = text_bytes("transport session");
   co_await outbound.async_write(payload);
   auto received = co_await inbound.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   co_await left.async_close();
   co_await right.async_close();
}

} // namespace

BOOST_AUTO_TEST_SUITE(yamux)

BOOST_AUTO_TEST_CASE(yamux_supports_open_accept_and_early_data) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_open_accept_and_early_data());
}

BOOST_AUTO_TEST_CASE(yamux_concurrent_close_is_single_owner) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_concurrent_close_owns_one_transport_close());
}

BOOST_AUTO_TEST_CASE(yamux_close_waits_for_admitted_write_after_remote_go_away) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::blocking::run(runtime, yamux_close_waits_for_admitted_write_after_remote_go_away_scenario());
}

BOOST_AUTO_TEST_CASE(yamux_protocol_error_precedes_bounded_go_away_drain) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_protocol_error_is_terminal_before_go_away_drain());
}

BOOST_AUTO_TEST_CASE(yamux_terminal_write_failure_preserves_original_protocol_error) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_failed_error_go_away_preserves_protocol_error());
}

BOOST_AUTO_TEST_CASE(yamux_protocol_error_deadline_cancels_blocked_transport_write) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_protocol_error_deadline_cancels_blocked_transport());
}

BOOST_AUTO_TEST_CASE(yamux_protocol_error_deadline_cancels_blocked_reset_writer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_protocol_error_cancels_blocked_reset_writer(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_cancel_during_active_frame_write_finishes_frame_before_stream_reset) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_cancel_during_active_frame_write_finishes_frame_then_resets_stream(),
       std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_stalled_frame_write_is_bounded_and_closes_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_stalled_frame_write_expires_and_closes_session(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_extreme_write_timeout_is_saturated) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_maximum_write_timeout_does_not_wrap(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_failed_first_syn_write_does_not_leave_a_ghost_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_failed_first_syn_write_terminalizes_session(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_start_failure_close_waits_for_lower_terminal_cleanup_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_start_failure_close_waits_for_lower_terminal_cleanup());
}

BOOST_AUTO_TEST_CASE(yamux_normal_close_failure_waits_for_lower_terminal_cleanup_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_normal_close_failure_waits_for_lower_terminal_cleanup());
}

BOOST_AUTO_TEST_CASE(yamux_close_joins_lower_terminal_exception_barrier_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_close_joins_lower_terminal_exception_barrier());
}

BOOST_AUTO_TEST_CASE(yamux_preterminal_stream_close_failure_resets_before_cached_error_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_preterminal_stream_close_failure_resets_before_cached_error());
}

BOOST_AUTO_TEST_CASE(yamux_preterminal_stream_close_failure_joins_reset_writer_terminal_error_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_preterminal_stream_close_failure_joins_reset_writer_terminal_error());
}

BOOST_AUTO_TEST_CASE(yamux_failed_terminal_go_away_waits_for_lower_terminal_cleanup_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_failed_terminal_go_away_waits_for_lower_terminal_cleanup());
}

BOOST_AUTO_TEST_CASE(yamux_protocol_failure_claims_error_go_away_before_reset_writer_drain_regression) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::blocking::run(runtime, yamux_protocol_failure_claims_error_go_away_before_reset_writer_drain());
}

BOOST_AUTO_TEST_CASE(yamux_options_preserve_the_legacy_positional_close_timeout) {
   const auto value = forge::net::yamux::options{
       256U * 1024U,
       16U * 1024U * 1024U,
       256U * 1024U,
       4096,
       256,
       1024U * 1024U,
       16U * 1024U * 1024U,
       std::chrono::milliseconds{123},
   };
   BOOST_TEST(value.close_timeout == std::chrono::milliseconds{123});
   BOOST_TEST(value.write_timeout == std::chrono::milliseconds{5'000});
}

BOOST_AUTO_TEST_CASE(yamux_graceful_and_error_close_have_one_terminal_writer) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   forge::asio::blocking::run(runtime, yamux_graceful_close_and_protocol_failure_share_terminal_owner());
}

BOOST_AUTO_TEST_CASE(yamux_close_drains_the_underlying_read_loop) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_close_waits_for_read_loop());
}

BOOST_AUTO_TEST_CASE(yamux_concurrent_first_operations_share_one_started_read_loop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_concurrent_first_operations_share_one_read_loop());
}

BOOST_AUTO_TEST_CASE(yamux_close_cancels_a_peer_that_never_finishes_its_half_close) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_close_bounds_underlying_half_close());
}

BOOST_AUTO_TEST_CASE(yamux_close_deadline_includes_a_write_that_already_holds_the_gate) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_close_bounds_a_write_that_holds_the_gate());
}

BOOST_AUTO_TEST_CASE(yamux_keeps_concurrent_stream_payloads_isolated) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_concurrent_streams_do_not_cross_deliver());
}

BOOST_AUTO_TEST_CASE(yamux_serializes_concurrent_writes_without_starving_waiters) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_concurrent_writes_are_fifo_without_timer_spin());
}

BOOST_AUTO_TEST_CASE(yamux_concurrent_stream_opens_complete_without_lost_wakeup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_concurrent_stream_opens_do_not_lose_write_wakeup());
}

BOOST_AUTO_TEST_CASE(yamux_cancel_unblocks_pending_write_waiters) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_cancel_wakes_pending_write_waiters());
}

BOOST_AUTO_TEST_CASE(yamux_canceled_queued_write_does_not_lose_fifo_ownership) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_canceled_queued_write_preserves_fifo_ownership());
}

BOOST_AUTO_TEST_CASE(yamux_canceled_writes_do_not_publish_stream_state_or_credit) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_canceled_writes_do_not_publish_stream_state());
}

BOOST_AUTO_TEST_CASE(yamux_reset_invalidates_stream_and_cancel_sends_rst) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_reset_streams_are_invalid_and_cancel_reaches_peer());
}

BOOST_AUTO_TEST_CASE(yamux_request_cancel_preserves_sibling_stream) {
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_request_cancel_resets_only_the_selected_stream(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_close_joins_persistent_stream_cancel_worker_after_rst) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_close_joins_stream_cancel_worker_after_wire_rst(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_transport_session_cancel_then_close_is_a_terminal_barrier) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_transport_session_cancel_then_close_is_terminal(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_stream_cancel_allocation_failure_terminalizes_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, yamux_reset_write_allocation_failure_is_session_terminal(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(yamux_write_failure_throws_typed_yamux_closed) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_normalizes_underlying_write_failure());
}

BOOST_AUTO_TEST_CASE(yamux_applies_flow_control_with_window_updates) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_flow_control_waits_for_window_update());
}

BOOST_AUTO_TEST_CASE(yamux_publishes_receive_credit_after_window_update_drain) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_receive_credit_waits_for_window_update_drain());
}

BOOST_AUTO_TEST_CASE(yamux_treats_syn_and_ack_window_lengths_as_deltas) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_window_updates_are_deltas());
}

BOOST_AUTO_TEST_CASE(yamux_retains_chunk_lifetime_while_flow_control_is_blocked) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_flow_control_retains_upstream_chunk_lifetime());
}

BOOST_AUTO_TEST_CASE(yamux_retains_chunk_lifetime_until_underlying_transport_drains) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_transport_drain_owns_upstream_chunk_lifetime());
}

BOOST_AUTO_TEST_CASE(yamux_close_flushes_and_read_after_close_is_rejected) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_close_flushes_pending_data_and_read_after_close_fails());
}

BOOST_AUTO_TEST_CASE(yamux_remote_fin_never_masks_reset_or_session_loss) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::asio::blocking::run(runtime, yamux_remote_fin_does_not_mask_reset_or_session_loss());
}

BOOST_AUTO_TEST_CASE(yamux_rejects_limits_and_malformed_frames_with_typed_errors) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_limits_and_malformed_frames_are_typed());
}

BOOST_AUTO_TEST_CASE(yamux_resets_only_streams_that_exceed_buffers) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_resource_overflow_resets_only_offending_stream());
}

BOOST_AUTO_TEST_CASE(yamux_parser_preserves_partial_and_buffered_frames) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_parser_handles_partial_and_buffered_frames());
}

BOOST_AUTO_TEST_CASE(yamux_reset_reclaim_releases_buffer_budget) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_reset_reclaim_releases_buffer_budget_for_open_streams());
}

BOOST_AUTO_TEST_CASE(yamux_enforces_configured_runtime_limits) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_configured_limits_are_behavioral());
}

BOOST_AUTO_TEST_CASE(yamux_rejects_excess_and_overflowing_peer_window_updates) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_rejects_invalid_peer_window_updates());
}

BOOST_AUTO_TEST_CASE(yamux_rejects_data_beyond_advertised_receive_credit) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_rejects_receive_over_credit());
}

BOOST_AUTO_TEST_CASE(yamux_preserves_late_accept_read_and_window_notifications) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(runtime, yamux_late_notifications_are_sticky());
}

BOOST_AUTO_TEST_CASE(yamux_reclaims_terminal_streams_before_stream_cap) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_reclaims_terminal_streams_before_enforcing_stream_cap());
}

BOOST_AUTO_TEST_CASE(yamux_ignores_late_frames_after_stream_reclamation) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_ignores_late_frames_for_reclaimed_streams());
}

BOOST_AUTO_TEST_CASE(yamux_accepts_concurrent_remote_stream_opens_out_of_order) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_accepts_concurrent_remote_opens_out_of_order());
}

BOOST_AUTO_TEST_CASE(yamux_handles_ping_and_goaway_control_frames) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_control_frames_are_handled());
}

BOOST_AUTO_TEST_CASE(yamux_exposes_transport_session_wrapper) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, yamux_transport_session_wrapper_delegates());
}

BOOST_AUTO_TEST_SUITE_END()
