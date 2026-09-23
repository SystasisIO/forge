#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/error_code.hpp>

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.duplex_stream;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.stream_reader;
import forge.api.core.stream_writer;
import forge.api.stream.session;
import forge.api.transport.connection;
import forge.api.transport.server;
import forge.api.websocket.binding;
import forge.api.websocket.connection;
import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.net.transport.stream;
import forge.net.websocket.client;
import forge.raw.raw;

namespace transport_live_types {

struct item {
   std::uint32_t value = 0;

   bool operator==(const item&) const = default;
};

struct total {
   std::uint32_t value = 0;

   bool operator==(const total&) const = default;
};

template <typename Stream> Stream& operator<<(Stream& stream, const item& value) {
   forge::raw::pack(stream, value.value);
   return stream;
}

template <typename Stream> Stream& operator>>(Stream& stream, item& value) {
   forge::raw::unpack(stream, value.value);
   return stream;
}

template <typename Stream> Stream& operator<<(Stream& stream, const total& value) {
   forge::raw::pack(stream, value.value);
   return stream;
}

template <typename Stream> Stream& operator>>(Stream& stream, total& value) {
   forge::raw::unpack(stream, value.value);
   return stream;
}

class live_api : public forge::api::core::contract<live_api, forge::api::core::surface::local |
                                                                 forge::api::core::surface::remote> {
 public:
   virtual ~live_api() = default;

   virtual boost::asio::awaitable<item> echo(item value) = 0;
   virtual boost::asio::awaitable<void> download(std::uint32_t count, forge::api::core::stream_writer<item> output) = 0;
   virtual boost::asio::awaitable<total> upload(forge::api::core::stream_reader<item> input) = 0;
   virtual boost::asio::awaitable<void> exchange(forge::api::core::duplex_stream<item, item> stream) = 0;
};

} // namespace transport_live_types

FORGE_API(::transport_live_types::live_api, FORGE_API_CONTRACT("transport.live", 1, 0), FORGE_API_METHOD(echo),
          FORGE_API_METHOD(download, count), FORGE_API_METHOD(upload), FORGE_API_METHOD(exchange))

namespace {

using bytes = std::vector<std::uint8_t>;
using item = transport_live_types::item;
using total = transport_live_types::total;
using live_api = transport_live_types::live_api;

class live_impl final : public live_api {
 public:
   boost::asio::awaitable<item> echo(item value) override {
      echo_calls.fetch_add(1, std::memory_order_release);
      co_return value;
   }

   boost::asio::awaitable<void> download(std::uint32_t count, forge::api::core::stream_writer<item> output) override {
      download_calls.fetch_add(1, std::memory_order_release);
      download_started.store(true, std::memory_order_release);
      for (auto index = std::uint32_t{0}; index < count; ++index) {
         co_await output.async_write(item{.value = index + 1});
         produced.store(index + 1, std::memory_order_release);
      }
      co_await output.async_close();
   }

   boost::asio::awaitable<total> upload(forge::api::core::stream_reader<item> input) override {
      upload_started.store(true, std::memory_order_release);
      if (fail_upload.load(std::memory_order_acquire)) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "declared upload failure");
      }
      if (hold_upload_reads.load(std::memory_order_acquire)) {
         const auto executor = co_await boost::asio::this_coro::executor;
         auto delay = boost::asio::steady_timer{executor};
         while (hold_upload_reads.load(std::memory_order_acquire)) {
            const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
            if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
               FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "held upload was cancelled");
            }
            delay.expires_after(std::chrono::milliseconds{1});
            auto error = boost::system::error_code{};
            co_await delay.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         }
      }
      auto sum = std::uint32_t{0};
      while (auto value = co_await input.async_read()) {
         sum += value->value;
         if (return_upload_after_first.load(std::memory_order_acquire)) {
            break;
         }
      }
      upload_ready_to_complete.store(true, std::memory_order_release);
      if (hold_upload_completion.load(std::memory_order_acquire)) {
         const auto executor = co_await boost::asio::this_coro::executor;
         auto delay = boost::asio::steady_timer{executor};
         while (hold_upload_completion.load(std::memory_order_acquire)) {
            delay.expires_after(std::chrono::milliseconds{1});
            co_await delay.async_wait(boost::asio::use_awaitable);
         }
      }
      upload_completions.fetch_add(1, std::memory_order_release);
      co_return total{.value = sum};
   }

   boost::asio::awaitable<void> exchange(forge::api::core::duplex_stream<item, item> stream) override {
      while (auto value = co_await stream.async_read()) {
         co_await stream.async_write(item{.value = value->value * 2});
         if (return_exchange_after_first.load(std::memory_order_acquire)) {
            break;
         }
      }
      co_await stream.async_close();
      exchange_ready_to_complete.store(true, std::memory_order_release);
      if (hold_exchange_completion.load(std::memory_order_acquire)) {
         const auto executor = co_await boost::asio::this_coro::executor;
         auto delay = boost::asio::steady_timer{executor};
         while (hold_exchange_completion.load(std::memory_order_acquire)) {
            delay.expires_after(std::chrono::milliseconds{1});
            co_await delay.async_wait(boost::asio::use_awaitable);
         }
      }
   }

   std::atomic_uint32_t echo_calls{0};
   std::atomic_bool download_started{false};
   std::atomic_bool upload_started{false};
   std::atomic_bool hold_upload_reads{false};
   std::atomic_bool fail_upload{false};
   std::atomic_bool return_upload_after_first{false};
   std::atomic_bool hold_upload_completion{false};
   std::atomic_bool upload_ready_to_complete{false};
   std::atomic_bool return_exchange_after_first{false};
   std::atomic_bool hold_exchange_completion{false};
   std::atomic_bool exchange_ready_to_complete{false};
   std::atomic_uint32_t produced{0};
   std::atomic_uint32_t download_calls{0};
   std::atomic_uint32_t upload_completions{0};
};

class fake_stream final : public forge::net::transport::detail::stream_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override {
      const auto lock = std::scoped_lock{mutex_};
      return open_;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return 7;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      co_await deliver(bytes{value.begin(), value.end()});
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk value) override {
      co_await deliver(std::move(value).into_vector());
   }

   boost::asio::awaitable<bytes> async_read() override {
      co_return (co_await async_read_chunk()).into_vector();
   }

   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override {
      const auto executor = co_await boost::asio::this_coro::executor;
      while (true) {
         std::shared_ptr<boost::asio::steady_timer> wake;
         {
            const auto lock = std::scoped_lock{mutex_};
            if (!reads_.empty()) {
               auto value = std::move(reads_.front());
               reads_.pop_front();
               ++read_count_;
               co_return forge::net::transport::chunk{std::move(value)};
            }
            if (!open_) {
               FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "fake API transport stream closed");
            }
            if (!read_wake_) {
               read_wake_ = std::make_shared<boost::asio::steady_timer>(executor);
            }
            wake = read_wake_;
            wake->expires_at(boost::asio::steady_timer::time_point::max());
         }
         auto error = boost::system::error_code{};
         co_await wake->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
   }

   boost::asio::awaitable<void> async_close() override {
      if (write_active_.load(std::memory_order_acquire)) {
         close_during_write_.store(true, std::memory_order_release);
      }
      close_local();
      if (auto target = peer_.lock()) {
         target->close_local();
      }
      co_return;
   }

   void cancel() override {
      close_local();
      if (auto target = peer_.lock()) {
         target->close_local();
      }
   }

   void connect(const std::shared_ptr<fake_stream>& value) {
      peer_ = value;
   }

   void push(bytes value) {
      std::shared_ptr<boost::asio::steady_timer> wake;
      {
         const auto lock = std::scoped_lock{mutex_};
         reads_.push_back(std::move(value));
         wake = read_wake_;
      }
      if (wake) {
         wake->cancel();
      }
   }

   [[nodiscard]] std::size_t write_count() const {
      const auto lock = std::scoped_lock{mutex_};
      return writes_.size();
   }

   [[nodiscard]] std::size_t read_count() const {
      const auto lock = std::scoped_lock{mutex_};
      return read_count_;
   }

   [[nodiscard]] bytes written(std::size_t index) const {
      const auto lock = std::scoped_lock{mutex_};
      return writes_.at(index);
   }

   void hold_writes(bool value) noexcept {
      hold_writes_.store(value, std::memory_order_release);
   }

   [[nodiscard]] bool write_active() const noexcept {
      return write_active_.load(std::memory_order_acquire);
   }

   [[nodiscard]] bool close_during_write() const noexcept {
      return close_during_write_.load(std::memory_order_acquire);
   }

   void pause_write_completion_after_delivery() noexcept {
      hold_delivered_completion_.store(true, std::memory_order_release);
   }

   [[nodiscard]] bool delivered_completion_pending() const noexcept {
      return delivered_completion_pending_.load(std::memory_order_acquire);
   }

   void complete_delivered_write(bool fail = false) noexcept {
      fail_delivered_completion_.store(fail, std::memory_order_release);
      hold_delivered_completion_.store(false, std::memory_order_release);
      delivered_completion_wake_.notify();
   }

 private:
   boost::asio::awaitable<void> deliver(bytes value) {
      write_active_.store(true, std::memory_order_release);
      const auto executor = co_await boost::asio::this_coro::executor;
      auto delay = boost::asio::steady_timer{executor};
      while (hold_writes_.load(std::memory_order_acquire)) {
         delay.expires_after(std::chrono::milliseconds{1});
         auto error = boost::system::error_code{};
         co_await delay.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      {
         const auto lock = std::scoped_lock{mutex_};
         if (!open_) {
            write_active_.store(false, std::memory_order_release);
            FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "fake API transport stream closed");
         }
         writes_.push_back(value);
      }
      if (auto target = peer_.lock()) {
         const auto split = std::min<std::size_t>(3, value.size());
         target->push(bytes{value.begin(), value.begin() + static_cast<std::ptrdiff_t>(split)});
         if (split < value.size()) {
            target->push(bytes{value.begin() + static_cast<std::ptrdiff_t>(split), value.end()});
         }
      }
      if (hold_delivered_completion_.load(std::memory_order_acquire)) {
         delivered_completion_pending_.store(true, std::memory_order_release);
         while (hold_delivered_completion_.load(std::memory_order_acquire)) {
            const auto observed = delivered_completion_wake_.epoch();
            if (!hold_delivered_completion_.load(std::memory_order_acquire)) {
               break;
            }
            static_cast<void>(co_await delivered_completion_wake_.async_wait(observed));
         }
         delivered_completion_pending_.store(false, std::memory_order_release);
         if (fail_delivered_completion_.load(std::memory_order_acquire)) {
            write_active_.store(false, std::memory_order_release);
            FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed,
                                  "fake API transport write completion failed after delivery");
         }
      }
      write_active_.store(false, std::memory_order_release);
      co_return;
   }

   void close_local() {
      std::shared_ptr<boost::asio::steady_timer> wake;
      {
         const auto lock = std::scoped_lock{mutex_};
         open_ = false;
         wake = read_wake_;
      }
      if (wake) {
         wake->cancel();
      }
      complete_delivered_write();
   }

   mutable std::mutex mutex_;
   std::deque<bytes> reads_;
   std::vector<bytes> writes_;
   std::size_t read_count_ = 0;
   std::shared_ptr<boost::asio::steady_timer> read_wake_;
   std::weak_ptr<fake_stream> peer_;
   std::atomic_bool hold_writes_{false};
   forge::asio::notification delivered_completion_wake_;
   std::atomic_bool hold_delivered_completion_{false};
   std::atomic_bool delivered_completion_pending_{false};
   std::atomic_bool fail_delivered_completion_{false};
   std::atomic_bool write_active_{false};
   std::atomic_bool close_during_write_{false};
   bool open_ = true;
};

struct service_state {
   explicit service_state(boost::asio::any_io_executor executor) : wake{std::move(executor)} {
      wake.expires_at(boost::asio::steady_timer::time_point::max());
   }

   boost::asio::steady_timer wake;
   std::exception_ptr error;
   bool done = false;
};

[[nodiscard]] forge::net::transport::stream make_stream(const std::shared_ptr<fake_stream>& model) {
   return forge::net::transport::detail::stream_access::make(model);
}

[[nodiscard]] std::pair<forge::net::transport::stream, forge::net::transport::stream>
make_stream_pair(const std::shared_ptr<fake_stream>& client, const std::shared_ptr<fake_stream>& server) {
   client->connect(server);
   server->connect(client);
   return {make_stream(client), make_stream(server)};
}

[[nodiscard]] bytes pack_api_frame(const forge::api::core::frame& value) {
   return forge::net::transport::encode_frame(forge::raw::pack(value));
}

[[nodiscard]] forge::api::core::frame hello_frame(forge::api::core::session_hello value = {}) {
   return forge::api::core::frame{
       .kind = forge::api::core::frame_kind::session_hello,
       .id = {},
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(value),
   };
}

template <typename Error>
[[nodiscard]] bool session_rejects(std::vector<bytes> inbound, forge::api::stream::options options = {}) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>();
   for (auto& frame : inbound) {
      model->push(std::move(frame));
   }
   auto live = forge::api::stream::session{make_stream(model), options};
   try {
      static_cast<void>(forge::asio::blocking::run(runtime, live.async_call(forge::api::core::frame{
                                                                .kind = forge::api::core::frame_kind::request,
                                                                .api = {.id = {"transport.live"}, .major = 1},
                                                                .method = "echo",
                                                                .codec = {.value = "forge.raw"},
                                                                .payload = forge::raw::pack(item{.value = 1}),
                                                            })));
   } catch (const Error&) {
      return true;
   } catch (...) {
      return false;
   }
   return false;
}

template <typename Error>
[[nodiscard]] bool server_stream_rejects(std::vector<forge::api::core::frame> inbound,
                                         forge::api::stream::options options = {}) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>();
   model->push(pack_api_frame(hello_frame()));
   boost::asio::co_spawn(
       runtime.context(),
       [model, inbound = std::move(inbound)]() mutable -> boost::asio::awaitable<void> {
          co_await wait_until([model] { return model->write_count() >= 3U; }, std::chrono::milliseconds{250});
          for (auto& frame : inbound) {
             model->push(pack_api_frame(frame));
          }
       },
       boost::asio::detached);

   auto output =
       forge::api::core::detail::make_local_stream_pair(runtime.context().get_executor(), options.max_item_size,
                                                        options.initial_window_items, options.max_buffered_bytes);
   auto live = forge::api::stream::session{make_stream(model), options};
   const auto api = live_api::describe();
   const auto* descriptor = forge::api::core::find_method(api, "download");
   BOOST_REQUIRE(descriptor != nullptr);
   try {
      static_cast<void>(
          forge::asio::blocking::run(runtime, live.async_stream_call(
                                                  forge::api::core::frame{
                                                      .kind = forge::api::core::frame_kind::request,
                                                      .api = live_api::ref(),
                                                      .method = "download",
                                                      .codec = {.value = "forge.raw"},
                                                      .payload = forge::raw::pack(std::uint32_t{1}),
                                                  },
                                                  forge::api::core::method_kind::server_stream, nullptr, output.writer,
                                                  forge::api::stream::call_options{.id = {.value = 1}}, *descriptor)));
   } catch (const Error&) {
      return true;
   } catch (...) {
      return false;
   }
   return false;
}

[[nodiscard]] forge::api::core::frame unpack_api_frame(const bytes& value) {
   const auto decoded = forge::net::transport::decode_frame(value);
   BOOST_REQUIRE(decoded.status == forge::net::transport::frame_decode_status::complete);
   return forge::raw::unpack_exact<forge::api::core::frame>(decoded.payload);
}

[[nodiscard]] std::size_t count_written_frames(const std::shared_ptr<fake_stream>& stream,
                                               forge::api::core::frame_kind kind, const std::string& method) {
   auto count = std::size_t{0};
   const auto size = stream->write_count();
   for (auto index = std::size_t{0}; index < size; ++index) {
      const auto frame = unpack_api_frame(stream->written(index));
      if (frame.kind == kind && frame.method == method) {
         ++count;
      }
   }
   return count;
}

[[nodiscard]] std::shared_ptr<service_state> start_service(boost::asio::any_io_executor executor,
                                                           boost::asio::awaitable<void> operation) {
   auto state = std::make_shared<service_state>(executor);
   boost::asio::co_spawn(
       executor,
       [state, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
          try {
             co_await std::move(operation);
          } catch (...) {
             state->error = std::current_exception();
          }
          state->done = true;
          state->wake.cancel();
       },
       boost::asio::detached);
   return state;
}

boost::asio::awaitable<void> wait_service(const std::shared_ptr<service_state>& state) {
   while (!state->done) {
      auto error = boost::system::error_code{};
      co_await state->wake.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
   }
   if (state->error) {
      std::rethrow_exception(state->error);
   }
}

template <typename Predicate>
boost::asio::awaitable<void> wait_until(Predicate predicate, std::chrono::milliseconds timeout) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto delay = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   while (!predicate()) {
      BOOST_REQUIRE_MESSAGE(std::chrono::steady_clock::now() < deadline, "timed out waiting for API session state");
      delay.expires_after(std::chrono::milliseconds{1});
      co_await delay.async_wait(boost::asio::use_awaitable);
   }
}

boost::asio::awaitable<void> exercise_delivered_hello_with_pending_write_completion(bool fail_completion) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto model = std::make_shared<fake_stream>();
   model->pause_write_completion_after_delivery();
   auto implementation = std::make_shared<live_impl>();
   auto registry = forge::api::core::registry{};
   registry.install<live_api>(live_api::describe(), implementation);
   auto service = start_service(executor, forge::api::transport::serve_stream(
                                              make_stream(model), forge::api::core::binding().serve(registry).build()));

   co_await wait_until([model] { return model->delivered_completion_pending(); }, std::chrono::milliseconds{250});
   BOOST_REQUIRE_EQUAL(model->write_count(), 1U);
   BOOST_TEST(static_cast<int>(unpack_api_frame(model->written(0)).kind) ==
              static_cast<int>(forge::api::core::frame_kind::session_hello));

   model->push(pack_api_frame(hello_frame()));
   model->push(pack_api_frame(forge::api::core::frame{
       .kind = forge::api::core::frame_kind::request,
       .id = {.value = 7},
       .api = live_api::ref(),
       .method = "echo",
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(item{.value = 17}),
   }));
   co_await wait_until([model] { return model->read_count() >= 2U; }, std::chrono::milliseconds{250});
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_TEST(!service->done);
   BOOST_TEST(implementation->echo_calls.load(std::memory_order_acquire) == 0U);
   BOOST_TEST(model->write_count() == 1U);

   model->complete_delivered_write(fail_completion);
   if (fail_completion) {
      co_await wait_until([service] { return service->done; }, std::chrono::milliseconds{250});
      BOOST_TEST(static_cast<bool>(service->error));
      BOOST_TEST(implementation->echo_calls.load(std::memory_order_acquire) == 0U);
      BOOST_TEST(model->write_count() == 1U);
   } else {
      co_await wait_until(
          [model] { return count_written_frames(model, forge::api::core::frame_kind::response, "echo") == 1U; },
          std::chrono::milliseconds{250});
      BOOST_TEST(implementation->echo_calls.load(std::memory_order_acquire) == 1U);
      BOOST_TEST(forge::raw::unpack_exact<item>(unpack_api_frame(model->written(1)).payload).value == 17U);
      model->cancel();
      co_await wait_service(service);
   }
}

using exchange_call = forge::api::core::bidirectional_stream_call<item, item>;

boost::asio::awaitable<void> produce_until_stopped(const std::shared_ptr<exchange_call>& call,
                                                   const std::shared_ptr<std::atomic_bool>& stop) {
   auto value = std::uint32_t{1};
   while (!stop->load(std::memory_order_acquire)) {
      co_await call->async_write(item{.value = value++});
   }
}

boost::asio::awaitable<void> consume_until_closed(const std::shared_ptr<exchange_call>& call,
                                                  const std::shared_ptr<std::atomic_bool>& stop,
                                                  const std::shared_ptr<std::atomic_bool>& observed) {
   const auto first = co_await call->async_read();
   BOOST_REQUIRE(first.has_value());
   observed->store(true, std::memory_order_release);
   stop->store(true, std::memory_order_release);
   while (co_await call->async_read()) {
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(transport_api_tests)

BOOST_AUTO_TEST_CASE(session_rejects_incompatible_hello_before_request) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>();
   model->push(pack_api_frame(forge::api::core::frame{
       .kind = forge::api::core::frame_kind::session_hello,
       .id = {},
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(forge::api::core::session_hello{
           .version = {.major = 1, .minor = 0},
       }),
   }));

   auto live = forge::api::stream::session{make_stream(model)};
   auto rejected = false;
   try {
      static_cast<void>(forge::asio::blocking::run(runtime, live.async_call(forge::api::core::frame{
                                                                .kind = forge::api::core::frame_kind::request,
                                                                .api = {.id = {"transport.live"}, .major = 1},
                                                                .method = "echo",
                                                                .codec = {.value = "forge.raw"},
                                                                .payload = forge::raw::pack(item{.value = 1}),
                                                            })));
   } catch (const forge::api::core::exceptions::incompatible_version&) {
      rejected = true;
   }
   BOOST_TEST(rejected);
   BOOST_REQUIRE(model->write_count() >= 1U);
   BOOST_TEST(static_cast<int>(unpack_api_frame(model->written(0)).kind) ==
              static_cast<int>(forge::api::core::frame_kind::session_hello));
   const auto local_hello =
       forge::raw::unpack_exact<forge::api::core::session_hello>(unpack_api_frame(model->written(0)).payload);
   BOOST_TEST(local_hello.limits.max_inflight_calls == 16U);
}

BOOST_AUTO_TEST_CASE(session_rejects_application_data_before_hello) {
   auto request = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::request,
       .id = {.value = 9},
       .api = {.id = {"transport.live"}, .major = 1},
       .method = "echo",
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(item{.value = 1}),
   };
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>({pack_api_frame(request)})));
}

BOOST_AUTO_TEST_CASE(session_waits_for_delivered_hello_write_completion_before_dispatch) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, exercise_delivered_hello_with_pending_write_completion(false));
}

BOOST_AUTO_TEST_CASE(session_failed_hello_write_completion_never_dispatches_request) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, exercise_delivered_hello_with_pending_write_completion(true));
}

BOOST_AUTO_TEST_CASE(session_rejects_duplicate_or_non_control_hello) {
   auto duplicate = hello_frame();
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>(
       {pack_api_frame(hello_frame()), pack_api_frame(duplicate)})));

   auto non_control = hello_frame();
   non_control.id.value = 3;
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>({pack_api_frame(non_control)})));

   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>(
       {pack_api_frame(hello_frame()), pack_api_frame(forge::api::core::frame{
                                           .kind = forge::api::core::frame_kind::cancel,
                                           .id = {},
                                           .codec = {.value = "forge.raw"},
                                       })})));
}

BOOST_AUTO_TEST_CASE(session_rejects_incompatible_codec_capabilities_and_limits) {
   auto codec = hello_frame();
   codec.codec.value = "forge.json";
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>({pack_api_frame(codec)})));

   auto capabilities = forge::api::core::session_hello{};
   capabilities.capabilities.bits = static_cast<std::uint64_t>(forge::api::core::capability::unary);
   BOOST_TEST((session_rejects<forge::api::core::exceptions::incompatible_version>(
       {pack_api_frame(hello_frame(capabilities))})));

   auto limits = forge::api::core::session_hello{};
   limits.limits.max_frame_bytes = 1U;
   limits.limits.max_item_bytes = 2U;
   BOOST_TEST(
       (session_rejects<forge::api::core::exceptions::incompatible_version>({pack_api_frame(hello_frame(limits))})));
}

BOOST_AUTO_TEST_CASE(session_rejects_malformed_trailing_and_oversized_frames) {
   auto malformed = forge::net::transport::encode_frame(bytes{0xffU});
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>({std::move(malformed)})));

   auto trailing_payload = forge::raw::pack(hello_frame());
   trailing_payload.push_back(0xffU);
   BOOST_TEST((session_rejects<forge::api::core::exceptions::protocol_error>(
       {forge::net::transport::encode_frame(trailing_payload)})));

   auto oversized = bytes{0x00U, 0x00U, 0x01U, 0x01U};
   BOOST_TEST((session_rejects<forge::api::core::exceptions::resource_exhausted>({std::move(oversized)},
                                                                                 forge::api::stream::options{
                                                                                     .max_frame_size = 256,
                                                                                     .max_item_size = 128,
                                                                                     .initial_window_bytes = 128,
                                                                                     .max_buffered_bytes = 512,
                                                                                 })));
}

BOOST_AUTO_TEST_CASE(session_rejects_credit_direction_and_item_limit_violations) {
   auto item_before_credit = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::stream_item,
       .id = {.value = 1},
       .api = live_api::ref(),
       .method = "download",
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(item{.value = 1}),
   };
   BOOST_TEST((server_stream_rejects<forge::api::core::exceptions::resource_exhausted>(
       {item_before_credit, item_before_credit}, forge::api::stream::options{
                                                     .initial_window_items = 1,
                                                     .initial_window_bytes = 4,
                                                 })));

   auto wrong_direction = forge::api::core::frame{
       .kind = forge::api::core::frame_kind::stream_end,
       .id = {.value = 1},
       .api = live_api::ref(),
       .method = "download",
       .codec = {.value = "forge.raw"},
       .payload = forge::raw::pack(forge::api::core::stream_end{
           .direction = forge::api::core::stream_direction::input,
       }),
   };
   BOOST_TEST((server_stream_rejects<forge::api::core::exceptions::protocol_error>({wrong_direction})));

   auto oversized_item = item_before_credit;
   oversized_item.payload.resize(33U, 0xffU);
   BOOST_TEST((server_stream_rejects<forge::api::core::exceptions::protocol_error>({oversized_item},
                                                                                   forge::api::stream::options{
                                                                                       .max_frame_size = 256,
                                                                                       .max_item_size = 32,
                                                                                       .initial_window_bytes = 32,
                                                                                       .max_buffered_bytes = 256,
                                                                                   })));
}

BOOST_AUTO_TEST_CASE(transport_session_runs_all_method_kinds_incrementally) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build(),
                                      forge::api::transport::options{
                                          .initial_window_items = 1,
                                          .initial_window_bytes = 64,
                                          .disconnect_grace = std::chrono::milliseconds{50},
                                      }));
      auto connection = forge::api::transport::connection{std::move(client_stream),
                                                          forge::api::transport::options{
                                                              .initial_window_items = 1,
                                                              .initial_window_bytes = 64,
                                                              .disconnect_grace = std::chrono::milliseconds{50},
                                                          }};
      auto remote = co_await connection.get_remote_api<live_api>();

      BOOST_TEST((co_await remote->echo(item{.value = 9})).value == 9U);

      auto download = co_await remote.async_open<&live_api::download>(4U);
      co_await wait_until([implementation] { return implementation->download_started.load(std::memory_order_acquire); },
                          std::chrono::milliseconds{250});
      BOOST_TEST(implementation->produced.load(std::memory_order_acquire) < 4U);
      for (auto expected = std::uint32_t{1}; expected <= 4; ++expected) {
         const auto value = co_await download.async_read();
         BOOST_REQUIRE(value.has_value());
         BOOST_TEST(value->value == expected);
      }
      BOOST_TEST(!(co_await download.async_read()).has_value());
      co_await download.async_finish();

      auto fair_first = co_await remote.async_open<&live_api::download>(2U);
      auto fair_second = co_await remote.async_open<&live_api::download>(2U);
      auto fair_first_one = co_await fair_first.async_read();
      auto fair_second_one = co_await fair_second.async_read();
      auto fair_first_two = co_await fair_first.async_read();
      auto fair_second_two = co_await fair_second.async_read();
      BOOST_REQUIRE(fair_first_one.has_value());
      BOOST_REQUIRE(fair_second_one.has_value());
      BOOST_REQUIRE(fair_first_two.has_value());
      BOOST_REQUIRE(fair_second_two.has_value());
      BOOST_TEST(fair_first_one->value == 1U);
      BOOST_TEST(fair_second_one->value == 1U);
      BOOST_TEST(fair_first_two->value == 2U);
      BOOST_TEST(fair_second_two->value == 2U);
      BOOST_TEST(!(co_await fair_first.async_read()).has_value());
      BOOST_TEST(!(co_await fair_second.async_read()).has_value());
      co_await fair_first.async_finish();
      co_await fair_second.async_finish();

      auto upload = co_await remote.async_open<&live_api::upload>();
      co_await upload.async_write(item{.value = 3});
      co_await upload.async_write(item{.value = 4});
      co_await upload.async_close();
      BOOST_TEST((co_await upload.async_finish()).value == 7U);

      auto exchange = co_await remote.async_open<&live_api::exchange>();
      co_await exchange.async_write(item{.value = 5});
      const auto doubled = co_await exchange.async_read();
      BOOST_REQUIRE(doubled.has_value());
      BOOST_TEST(doubled->value == 10U);
      co_await exchange.async_close();
      BOOST_TEST(!(co_await exchange.async_read()).has_value());
      co_await exchange.async_finish();

      BOOST_REQUIRE(client_model->write_count() >= 1U);
      BOOST_REQUIRE(server_model->write_count() >= 1U);
      BOOST_TEST(static_cast<int>(unpack_api_frame(client_model->written(0)).kind) ==
                 static_cast<int>(forge::api::core::frame_kind::session_hello));
      BOOST_TEST(static_cast<int>(unpack_api_frame(server_model->written(0)).kind) ==
                 static_cast<int>(forge::api::core::frame_kind::session_hello));

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(contextual_stream_admission_decodes_once_and_allows_same_session_follow_up) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto descriptor = live_api::describe();
      auto method = std::find_if(descriptor.methods.begin(), descriptor.methods.end(),
                                 [](const auto& value) { return value.name == "download"; });
      BOOST_REQUIRE(method != descriptor.methods.end());

      const auto* fields = forge::api::core::detail::server_fields_for(*method);
      const auto* generated = forge::api::core::detail::contextual_stream_for(*method);
      BOOST_REQUIRE(fields != nullptr);
      BOOST_REQUIRE(generated != nullptr);
      BOOST_REQUIRE(static_cast<bool>(*generated));

      auto admission_decodes = std::make_shared<std::atomic_uint32_t>(0);
      auto contextual_invocations = std::make_shared<std::atomic_uint32_t>(0);
      const auto generated_invoker = *generated;
      method->request_decoder = [admission_decodes](const bytes&, forge::raw::unpack_limits) {
         admission_decodes->fetch_add(1, std::memory_order_release);
      };
      forge::api::core::detail::install_stream_context(
          *method, *fields,
          [generated_invoker,
           contextual_invocations](bytes payload, std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
                                   std::shared_ptr<forge::api::core::detail::stream_endpoint> output,
                                   forge::api::core::trusted_invocation trusted,
                                   forge::api::core::contextual_handler_gate gate) -> boost::asio::awaitable<bytes> {
             contextual_invocations->fetch_add(1, std::memory_order_release);
             co_return co_await generated_invoker(std::move(payload), std::move(input), std::move(output),
                                                  std::move(trusted), std::move(gate));
          });

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(std::move(descriptor), implementation);

      auto malformed_client_model = std::make_shared<fake_stream>();
      auto malformed_server_model = std::make_shared<fake_stream>();
      auto [malformed_client_stream, malformed_server_stream] =
          make_stream_pair(malformed_client_model, malformed_server_model);

      auto service = start_service(
         executor, forge::api::transport::serve_stream(std::move(malformed_server_stream),
                                                       forge::api::core::binding().serve(registry).build()));
      auto malformed_session = forge::api::stream::session{std::move(malformed_client_stream)};

      auto malformed_output = forge::api::core::detail::make_local_stream_pair(executor, 4096, 2, 8192);
      const auto malformed = co_await malformed_session.async_stream_call(
          forge::api::core::frame{
              .kind = forge::api::core::frame_kind::request,
              .api = live_api::ref(),
              .method = "download",
              .codec = {.value = "forge.raw"},
              .payload = {},
          },
          forge::api::core::method_kind::server_stream, {}, malformed_output.writer);
      BOOST_TEST(static_cast<int>(malformed.kind) == static_cast<int>(forge::api::core::frame_kind::error));

      auto neighbor_output = forge::api::core::detail::make_local_stream_pair(executor, 4096, 2, 8192);
      auto neighbor_result = std::make_shared<std::optional<forge::api::core::frame>>();
      const auto run_neighbor =
         [](forge::api::stream::session& session,
            std::shared_ptr<forge::api::core::detail::stream_endpoint> output,
            std::shared_ptr<std::optional<forge::api::core::frame>> result) -> boost::asio::awaitable<void> {
         *result = co_await session.async_stream_call(
            forge::api::core::frame{
                .kind = forge::api::core::frame_kind::request,
                .api = live_api::ref(),
                .method = "download",
                .codec = {.value = "forge.raw"},
                .payload = forge::raw::pack(std::uint32_t{1}),
            },
            forge::api::core::method_kind::server_stream, {}, std::move(output));
      };
      auto neighbor_call =
         start_service(executor, run_neighbor(malformed_session, neighbor_output.writer, neighbor_result));
      const auto item = co_await neighbor_output.reader->async_read();
      BOOST_REQUIRE(item.has_value());
      BOOST_TEST(forge::api::core::unpack_body<transport_live_types::item>(*item).value == 1U);
      BOOST_TEST(!(co_await neighbor_output.reader->async_read()).has_value());
      co_await wait_service(neighbor_call);
      BOOST_REQUIRE(neighbor_result->has_value());
      BOOST_TEST(static_cast<int>((*neighbor_result)->kind) ==
                 static_cast<int>(forge::api::core::frame_kind::response));

      BOOST_TEST(admission_decodes->load(std::memory_order_acquire) == 0U);
      BOOST_TEST(contextual_invocations->load(std::memory_order_acquire) == 2U);
      BOOST_TEST(implementation->download_calls.load(std::memory_order_acquire) == 1U);

      co_await malformed_session.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(stream_session_keeps_admitted_generation_for_decoder_and_handler) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto registry = forge::api::core::registry{};
      const auto old_decodes = std::make_shared<std::atomic_uint32_t>(0U);
      const auto replaced = std::make_shared<std::atomic_bool>(false);
      auto original = std::make_shared<live_impl>();
      auto replacement = std::make_shared<live_impl>();
      auto replacement_descriptor = std::make_shared<forge::api::core::descriptor>(live_api::describe());
      auto replacement_method = std::find_if(replacement_descriptor->methods.begin(), replacement_descriptor->methods.end(),
                                             [](const auto& value) { return value.name == "download"; });
      BOOST_REQUIRE(replacement_method != replacement_descriptor->methods.end());
      forge::api::core::detail::remove_stream_context(*replacement_method);
      forge::api::core::detail::remove_request_context(*replacement_method);

      auto original_descriptor = live_api::describe();
      auto original_method = std::find_if(original_descriptor.methods.begin(), original_descriptor.methods.end(),
                                          [](const auto& value) { return value.name == "download"; });
      BOOST_REQUIRE(original_method != original_descriptor.methods.end());
      forge::api::core::detail::remove_stream_context(*original_method);
      forge::api::core::detail::remove_request_context(*original_method);
      const auto original_request_decoder = original_method->request_decoder;
      original_method->request_decoder = [old_decodes, replaced, &registry, replacement_descriptor,
                                          replacement, original_request_decoder](const bytes& payload,
                                                                                forge::raw::unpack_limits limits) {
         original_request_decoder(payload, limits);
         old_decodes->fetch_add(1, std::memory_order_release);
         if (!replaced->exchange(true, std::memory_order_acq_rel)) {
            registry.clear();
            registry.install<live_api>(*replacement_descriptor, replacement);
         }
      };
      registry.install<live_api>(std::move(original_descriptor), original);

      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);
      auto service = start_service(
         executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                       forge::api::core::binding().serve(registry).build()));
      auto client = forge::api::stream::session{std::move(client_stream)};
      auto output = forge::api::core::detail::make_local_stream_pair(executor, 4096, 2, 8192);
      auto result = std::make_shared<std::optional<forge::api::core::frame>>();
      const auto run_call =
         [](forge::api::stream::session& session,
            std::shared_ptr<forge::api::core::detail::stream_endpoint> stream_output,
            std::shared_ptr<std::optional<forge::api::core::frame>> call_result)
         -> boost::asio::awaitable<void> {
            *call_result = co_await session.async_stream_call(
               forge::api::core::frame{
                   .kind = forge::api::core::frame_kind::request,
                   .api = live_api::ref(),
                   .method = "download",
                   .codec = {.value = "forge.raw"},
                   .payload = forge::raw::pack(std::uint32_t{1}),
               },
               forge::api::core::method_kind::server_stream, {}, std::move(stream_output));
         };
      auto call = start_service(executor, run_call(client, output.writer, result));

      const auto item = co_await output.reader->async_read();
      BOOST_REQUIRE(item.has_value());
      BOOST_TEST(forge::api::core::unpack_body<transport_live_types::item>(*item).value == 1U);
      BOOST_TEST(!(co_await output.reader->async_read()).has_value());
      co_await wait_service(call);
      BOOST_REQUIRE(result->has_value());
      BOOST_TEST(static_cast<int>(result->value().kind) == static_cast<int>(forge::api::core::frame_kind::response));
      BOOST_TEST(old_decodes->load(std::memory_order_acquire) == 1U);
      BOOST_TEST(replaced->load(std::memory_order_acquire));
      BOOST_TEST(original->download_calls.load(std::memory_order_acquire) == 1U);
      BOOST_TEST(replacement->download_calls.load(std::memory_order_acquire) == 0U);

      co_await client.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(duplex_data_progresses_under_continuous_window_updates) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 1,
          .initial_window_bytes = sizeof(std::uint32_t),
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();
      auto exchange = std::make_shared<exchange_call>(co_await remote.async_open<&live_api::exchange>());

      auto stop = std::make_shared<std::atomic_bool>(false);
      auto observed = std::make_shared<std::atomic_bool>(false);
      auto producer = start_service(executor, produce_until_stopped(exchange, stop));
      auto consumer = start_service(executor, consume_until_closed(exchange, stop, observed));

      co_await wait_until([observed] { return observed->load(std::memory_order_acquire); },
                          std::chrono::milliseconds{500});
      co_await wait_service(producer);
      co_await exchange->async_close();
      co_await wait_service(consumer);
      co_await exchange->async_finish();
      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(active_ingress_drains_are_not_evicted_as_tombstones) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      implementation->return_upload_after_first.store(true, std::memory_order_release);
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 16,
          .initial_window_bytes = 64,
          .disconnect_grace = std::chrono::milliseconds{50},
          .max_tombstones = 1,
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();

      auto calls = std::vector<forge::api::core::client_stream_call<item, total>>{};
      for (auto index = 0; index < 4; ++index) {
         calls.push_back(co_await remote.async_open<&live_api::upload>());
      }
      for (auto& call : calls) {
         co_await call.async_write(item{.value = 1});
         co_await call.async_write(item{.value = 2});
         co_await call.async_write(item{.value = 3});
         co_await call.async_close();
      }
      for (auto& call : calls) {
         BOOST_TEST((co_await call.async_finish()).value == 1U);
      }
      BOOST_TEST((co_await remote->echo(item{.value = 9})).value == 9U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(early_half_close_before_terminal_releases_inflight_slot) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      implementation->return_upload_after_first.store(true, std::memory_order_release);
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_inflight = 1,
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 1,
          .initial_window_bytes = sizeof(std::uint32_t),
          .max_buffered_bytes = sizeof(std::uint32_t),
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();

      BOOST_TEST((co_await remote->echo(item{.value = 1})).value == 1U);
      const auto writes_before = server_model->write_count();
      auto upload = co_await remote.async_open<&live_api::upload>();
      co_await wait_until([server_model, writes_before] { return server_model->write_count() > writes_before; },
                          std::chrono::milliseconds{250});

      server_model->hold_writes(true);
      co_await upload.async_write(item{.value = 7});
      co_await wait_until(
          [implementation] { return implementation->upload_completions.load(std::memory_order_acquire) == 1U; },
          std::chrono::milliseconds{250});

      const auto reads_before_close = server_model->read_count();
      co_await upload.async_close();
      co_await wait_until(
          [server_model, reads_before_close] { return server_model->read_count() >= reads_before_close + 2U; },
          std::chrono::milliseconds{250});
      server_model->hold_writes(false);

      BOOST_TEST((co_await upload.async_finish()).value == 7U);
      BOOST_TEST((co_await remote->echo(item{.value = 2})).value == 2U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(early_terminal_before_half_close_releases_inflight_slot) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      implementation->return_upload_after_first.store(true, std::memory_order_release);
      implementation->hold_upload_completion.store(true, std::memory_order_release);
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_inflight = 1,
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 1,
          .initial_window_bytes = sizeof(std::uint32_t),
          .max_buffered_bytes = 4U * sizeof(std::uint32_t),
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();

      BOOST_TEST((co_await remote->echo(item{.value = 8})).value == 8U);
      auto upload = co_await remote.async_open<&live_api::upload>();
      co_await upload.async_write(item{.value = 1});
      co_await upload.async_write(item{.value = 2});
      co_await upload.async_write(item{.value = 3});
      co_await upload.async_write(item{.value = 4});
      co_await wait_until(
          [implementation] { return implementation->upload_ready_to_complete.load(std::memory_order_acquire); },
          std::chrono::milliseconds{250});
      co_await wait_until(
          [client_model] {
             return count_written_frames(client_model, forge::api::core::frame_kind::stream_item, "upload") == 2U;
          },
          std::chrono::milliseconds{250});
      implementation->hold_upload_completion.store(false, std::memory_order_release);
      BOOST_TEST((co_await upload.async_finish()).value == 1U);

      BOOST_TEST((co_await remote->echo(item{.value = 9})).value == 9U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(early_bidirectional_terminal_before_half_close_releases_inflight_slot) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      implementation->return_exchange_after_first.store(true, std::memory_order_release);
      implementation->hold_exchange_completion.store(true, std::memory_order_release);
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_inflight = 1,
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 1,
          .initial_window_bytes = sizeof(std::uint32_t),
          .max_buffered_bytes = 4U * sizeof(std::uint32_t),
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();

      BOOST_TEST((co_await remote->echo(item{.value = 10})).value == 10U);
      auto exchange = co_await remote.async_open<&live_api::exchange>();
      co_await exchange.async_write(item{.value = 1});
      co_await exchange.async_write(item{.value = 2});
      co_await exchange.async_write(item{.value = 3});
      co_await exchange.async_write(item{.value = 4});
      const auto response = co_await exchange.async_read();
      BOOST_REQUIRE(response.has_value());
      BOOST_TEST(response->value == 2U);
      BOOST_TEST(!(co_await exchange.async_read()).has_value());
      co_await wait_until(
          [implementation] { return implementation->exchange_ready_to_complete.load(std::memory_order_acquire); },
          std::chrono::milliseconds{250});
      co_await wait_until(
          [client_model] {
             return count_written_frames(client_model, forge::api::core::frame_kind::stream_item, "exchange") == 2U;
          },
          std::chrono::milliseconds{250});
      implementation->hold_exchange_completion.store(false, std::memory_order_release);
      co_await exchange.async_finish();

      BOOST_TEST((co_await remote->echo(item{.value = 11})).value == 11U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(pre_request_stream_rejection_releases_inflight_slot) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      constexpr auto unary = static_cast<std::uint64_t>(forge::api::core::capability::unary);
      constexpr auto stream_window = static_cast<std::uint64_t>(forge::api::core::capability::stream_window);
      auto server_options = forge::api::transport::options{
          .capabilities = {.bits = unary | stream_window},
          .max_inflight = 1,
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto client_options = forge::api::transport::options{
          .max_inflight = 1,
          .disconnect_grace = std::chrono::milliseconds{50},
      };
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), std::make_shared<live_impl>());
      auto service = start_service(
          executor, forge::api::transport::serve_stream(
                        std::move(server_stream), forge::api::core::binding().serve(registry).build(), server_options));
      auto connection = forge::api::transport::connection{std::move(client_stream), client_options};
      auto remote = co_await connection.get_remote_api<live_api>();

      BOOST_TEST((co_await remote->echo(item{.value = 3})).value == 3U);
      auto rejected = co_await remote.async_open<&live_api::download>(1U);
      auto incompatible = false;
      try {
         co_await rejected.async_finish();
      } catch (const forge::api::core::exceptions::incompatible_version&) {
         incompatible = true;
      }
      BOOST_TEST(incompatible);
      BOOST_TEST((co_await remote->echo(item{.value = 4})).value == 4U);

      auto rejected_upload = co_await remote.async_open<&live_api::upload>();
      incompatible = false;
      try {
         static_cast<void>(co_await rejected_upload.async_finish());
      } catch (const forge::api::core::exceptions::incompatible_version&) {
         incompatible = true;
      }
      BOOST_TEST(incompatible);
      BOOST_TEST((co_await remote->echo(item{.value = 5})).value == 5U);

      auto rejected_exchange = co_await remote.async_open<&live_api::exchange>();
      incompatible = false;
      try {
         co_await rejected_exchange.async_finish();
      } catch (const forge::api::core::exceptions::incompatible_version&) {
         incompatible = true;
      }
      BOOST_TEST(incompatible);
      BOOST_TEST((co_await remote->echo(item{.value = 6})).value == 6U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(streaming_terminal_errors_and_early_success_preserve_session) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build(),
                                      forge::api::transport::options{
                                          .disconnect_grace = std::chrono::milliseconds{50},
                                      }));
      auto connection = forge::api::transport::connection{std::move(client_stream),
                                                          forge::api::transport::options{
                                                              .disconnect_grace = std::chrono::milliseconds{50},
                                                          }};
      auto remote = co_await connection.get_remote_api<live_api>();

      implementation->fail_upload.store(true, std::memory_order_release);
      auto failed = co_await remote.async_open<&live_api::upload>();
      co_await failed.async_close();
      auto projected = false;
      try {
         static_cast<void>(co_await failed.async_finish());
      } catch (const forge::api::core::exceptions::protocol_error&) {
         projected = true;
      }
      BOOST_TEST(projected);

      implementation->fail_upload.store(false, std::memory_order_release);
      implementation->return_upload_after_first.store(true, std::memory_order_release);
      auto early = co_await remote.async_open<&live_api::upload>();
      co_await early.async_write(item{.value = 1});
      co_await early.async_write(item{.value = 2});
      co_await early.async_write(item{.value = 3});
      co_await early.async_close();
      BOOST_TEST((co_await early.async_finish()).value == 1U);

      BOOST_TEST((co_await remote->echo(item{.value = 17})).value == 17U);
      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(lazy_session_call_snapshots_impl_before_wrapper_move) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), std::make_shared<live_impl>());
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build()));
      auto live = forge::api::stream::session{std::move(client_stream)};
      const auto descriptor = live_api::describe();
      const auto* method = forge::api::core::find_method(descriptor, "echo");
      BOOST_REQUIRE(method != nullptr);
      auto pending = live.async_call(
          forge::api::core::frame{
              .kind = forge::api::core::frame_kind::request,
              .api = live_api::ref(),
              .method = "echo",
              .codec = {.value = "forge.raw"},
              .payload = forge::raw::pack(item{.value = 21}),
          },
          {}, *method);
      auto moved = std::move(live);
      const auto response = co_await std::move(pending);
      BOOST_TEST(forge::raw::unpack_exact<item>(response.payload).value == 21U);
      co_await moved.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(concurrent_initial_calls_share_session_handshake) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), std::make_shared<live_impl>());
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build()));
      auto connection = forge::api::transport::connection{std::move(client_stream)};
      auto remote = co_await connection.get_remote_api<live_api>();

      auto first = boost::asio::co_spawn(executor, remote->echo(item{.value = 31}), boost::asio::use_future);
      auto second = boost::asio::co_spawn(executor, remote->echo(item{.value = 32}), boost::asio::use_future);
      co_await wait_until(
          [&] {
             return first.wait_for(std::chrono::seconds{0}) == std::future_status::ready &&
                    second.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
          },
          std::chrono::milliseconds{250});

      BOOST_TEST(first.get().value == 31U);
      BOOST_TEST(second.get().value == 32U);
      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(session_rejects_reuse_of_retired_custom_call_id) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), std::make_shared<live_impl>());
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build(),
                                      forge::api::transport::options{
                                          .disconnect_grace = std::chrono::milliseconds{50},
                                      }));
      auto live = forge::api::stream::session{std::move(client_stream)};
      const auto descriptor = live_api::describe();
      const auto* method = forge::api::core::find_method(descriptor, "echo");
      BOOST_REQUIRE(method != nullptr);
      const auto request = forge::api::core::frame{
          .kind = forge::api::core::frame_kind::request,
          .api = live_api::ref(),
          .method = "echo",
          .codec = {.value = "forge.raw"},
          .payload = forge::raw::pack(item{.value = 4}),
      };
      const auto first =
          co_await live.async_call(request, forge::api::stream::call_options{.id = {.value = 41}}, *method);
      BOOST_TEST(forge::raw::unpack_exact<item>(first.payload).value == 4U);

      auto rejected = false;
      try {
         static_cast<void>(
             co_await live.async_call(request, forge::api::stream::call_options{.id = {.value = 41}}, *method));
      } catch (const forge::api::core::exceptions::protocol_error&) {
         rejected = true;
      }
      BOOST_TEST(rejected);
      co_await live.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(deadline_cancels_one_call_and_connection_stays_live) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build(),
                                      forge::api::transport::options{
                                          .disconnect_grace = std::chrono::milliseconds{20},
                                      }));
      auto connection = forge::api::transport::connection{std::move(client_stream),
                                                          forge::api::transport::options{
                                                              .disconnect_grace = std::chrono::milliseconds{20},
                                                          }};
      auto remote = co_await connection.get_remote_api<live_api>();
      auto blocked = co_await remote.async_open<&live_api::upload>(forge::api::core::call_options{
          .deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{20},
      });
      co_await wait_until([implementation] { return implementation->upload_started.load(std::memory_order_acquire); },
                          std::chrono::milliseconds{250});

      auto expired = false;
      try {
         static_cast<void>(co_await blocked.async_finish());
      } catch (const forge::api::core::exceptions::deadline_exceeded&) {
         expired = true;
      }
      BOOST_TEST(expired);
      BOOST_TEST((co_await remote->echo(item{.value = 12})).value == 12U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(cancel_unblocks_credit_wait_without_closing_session) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      implementation->hold_upload_reads.store(true, std::memory_order_release);
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      const auto options = forge::api::transport::options{
          .max_frame_size = 512,
          .max_item_size = sizeof(std::uint32_t),
          .initial_window_items = 1,
          .initial_window_bytes = sizeof(std::uint32_t),
          .max_buffered_bytes = 512,
          .disconnect_grace = std::chrono::milliseconds{20},
      };
      auto service = start_service(
          executor, forge::api::transport::serve_stream(std::move(server_stream),
                                                        forge::api::core::binding().serve(registry).build(), options));
      auto connection = forge::api::transport::connection{std::move(client_stream), options};
      auto remote = co_await connection.get_remote_api<live_api>();
      auto upload = co_await remote.async_open<&live_api::upload>(forge::api::core::call_options{
          .max_item_bytes = sizeof(std::uint32_t),
          .max_buffered_items = 1,
          .max_buffered_bytes = sizeof(std::uint32_t),
      });
      co_await wait_until([implementation] { return implementation->upload_started.load(std::memory_order_acquire); },
                          std::chrono::milliseconds{250});

      auto delay = boost::asio::steady_timer{executor};
      co_await upload.async_write(item{.value = 1});
      co_await upload.async_write(item{.value = 2});
      delay.expires_after(std::chrono::milliseconds{10});
      co_await delay.async_wait(boost::asio::use_awaitable);
      co_await upload.async_write(item{.value = 3});
      delay.expires_after(std::chrono::milliseconds{10});
      co_await delay.async_wait(boost::asio::use_awaitable);
      auto blocked = start_service(executor, upload.async_write(item{.value = 4}));
      delay.expires_after(std::chrono::milliseconds{20});
      co_await delay.async_wait(boost::asio::use_awaitable);
      BOOST_TEST(!blocked->done);

      upload.cancel();
      auto write_cancelled = false;
      try {
         co_await wait_service(blocked);
      } catch (const forge::api::core::exceptions::cancelled&) {
         write_cancelled = true;
      }
      BOOST_TEST(write_cancelled);

      auto finish_cancelled = false;
      try {
         static_cast<void>(co_await upload.async_finish());
      } catch (const forge::api::core::exceptions::cancelled&) {
         finish_cancelled = true;
      }
      BOOST_TEST(finish_cancelled);
      BOOST_TEST((co_await remote->echo(item{.value = 13})).value == 13U);

      co_await connection.async_close();
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(close_stops_admission_and_cancels_blocked_calls) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto client_model = std::make_shared<fake_stream>();
      auto server_model = std::make_shared<fake_stream>();
      auto [client_stream, server_stream] = make_stream_pair(client_model, server_model);

      auto implementation = std::make_shared<live_impl>();
      auto registry = forge::api::core::registry{};
      registry.install<live_api>(live_api::describe(), implementation);
      auto service =
          start_service(executor, forge::api::transport::serve_stream(
                                      std::move(server_stream), forge::api::core::binding().serve(registry).build(),
                                      forge::api::transport::options{
                                          .disconnect_grace = std::chrono::milliseconds{10},
                                      }));
      auto connection = forge::api::transport::connection{std::move(client_stream),
                                                          forge::api::transport::options{
                                                              .disconnect_grace = std::chrono::milliseconds{10},
                                                          }};
      auto remote = co_await connection.get_remote_api<live_api>();
      auto blocked = co_await remote.async_open<&live_api::upload>();
      co_await wait_until([implementation] { return implementation->upload_started.load(std::memory_order_acquire); },
                          std::chrono::milliseconds{250});

      co_await connection.async_close();
      auto cancelled = false;
      try {
         static_cast<void>(co_await blocked.async_finish());
      } catch (const forge::api::core::exceptions::cancelled&) {
         cancelled = true;
      }
      BOOST_TEST(cancelled);
      co_await wait_service(service);
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_CASE(graceful_close_waits_for_active_transport_write) {
   auto runtime = forge::asio::runtime{};
   auto scenario = []() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto model = std::make_shared<fake_stream>();
      model->push(pack_api_frame(hello_frame()));
      model->hold_writes(true);
      auto live = forge::api::stream::session{make_stream(model), forge::api::stream::options{
                                                                      .disconnect_grace = std::chrono::milliseconds{10},
                                                                      .control_timeout = std::chrono::milliseconds{250},
                                                                  }};
      const auto descriptor = live_api::describe();
      const auto* method = forge::api::core::find_method(descriptor, "echo");
      BOOST_REQUIRE(method != nullptr);
      auto call = start_service(executor, [&live, method]() -> boost::asio::awaitable<void> {
         try {
            static_cast<void>(co_await live.async_call(
                forge::api::core::frame{
                    .kind = forge::api::core::frame_kind::request,
                    .api = live_api::ref(),
                    .method = "echo",
                    .codec = {.value = "forge.raw"},
                    .payload = forge::raw::pack(item{.value = 1}),
                },
                {}, *method));
         } catch (...) {
            // Closing the session is expected to cancel the pending call.
         }
      }());
      co_await wait_until([model] { return model->write_active(); }, std::chrono::milliseconds{250});
      boost::asio::co_spawn(
          executor,
          [model]() -> boost::asio::awaitable<void> {
             auto delay = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
             delay.expires_after(std::chrono::milliseconds{20});
             co_await delay.async_wait(boost::asio::use_awaitable);
             model->hold_writes(false);
          },
          boost::asio::detached);
      co_await live.async_close();
      co_await wait_service(call);
      BOOST_TEST(!model->close_during_write());
   };
   forge::asio::blocking::run(runtime, scenario());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(api_websocket_stream_suite)

BOOST_AUTO_TEST_CASE(one_websocket_session_supports_all_remote_method_kinds) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto implementation = std::make_shared<live_impl>();
   auto registry = forge::api::core::registry{};
   registry.install<live_api>(live_api::describe(), implementation);

   auto binding = forge::api::websocket::api()
                      .use(forge::api::core::binding().serve(registry).export_api<live_api>(live_api::ref()).build())
                      .backpressure({.max_inflight = 8, .max_buffered_bytes = 4U * 1024U * 1024U})
                      .initial_window(1, 64)
                      .build();
   auto router = forge::net::http::router{};
   router.websocket(
       "/api", [&runtime, binding = std::move(binding)](forge::net::websocket::connection::ptr socket) mutable {
          boost::asio::co_spawn(runtime.context(), binding.accept(std::move(socket)), boost::asio::detached);
       });

   auto server = forge::net::http::server{runtime, forge::net::http::server_config{}, std::move(router)};
   server.start();
   auto websocket_client = forge::net::websocket::client{
       runtime, forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port()))};
   auto socket = websocket_client.connect("/api");
   auto connection =
       forge::api::websocket::connection{std::move(socket), forge::api::stream::options{
                                                                .max_inflight = 8,
                                                                .initial_window_items = 1,
                                                                .initial_window_bytes = 64,
                                                                .max_buffered_bytes = 4U * 1024U * 1024U,
                                                                .disconnect_grace = std::chrono::milliseconds{50},
                                                            }};

   auto scenario = [&connection]() -> boost::asio::awaitable<void> {
      auto remote = co_await connection.get_remote_api<live_api>();
      BOOST_TEST((co_await remote->echo(item{.value = 9})).value == 9U);

      auto download = co_await remote.async_open<&live_api::download>(2U);
      const auto first = co_await download.async_read();
      const auto second = co_await download.async_read();
      BOOST_REQUIRE(first.has_value());
      BOOST_REQUIRE(second.has_value());
      BOOST_TEST(first->value == 1U);
      BOOST_TEST(second->value == 2U);
      BOOST_TEST(!(co_await download.async_read()).has_value());
      co_await download.async_finish();

      auto upload = co_await remote.async_open<&live_api::upload>();
      co_await upload.async_write(item{.value = 3});
      co_await upload.async_write(item{.value = 4});
      co_await upload.async_close();
      BOOST_TEST((co_await upload.async_finish()).value == 7U);

      auto exchange = co_await remote.async_open<&live_api::exchange>();
      co_await exchange.async_write(item{.value = 5});
      const auto doubled = co_await exchange.async_read();
      BOOST_REQUIRE(doubled.has_value());
      BOOST_TEST(doubled->value == 10U);
      co_await exchange.async_close();
      BOOST_TEST(!(co_await exchange.async_read()).has_value());
      co_await exchange.async_finish();

      co_await connection.async_close();
   };
   forge::asio::blocking::run(runtime, scenario());
   server.stop();
}

BOOST_AUTO_TEST_CASE(binding_declares_all_wire_v2_method_capabilities) {
   const auto capabilities = forge::api::websocket::binding_capabilities();
   BOOST_TEST(capabilities.supports(forge::api::core::capability::unary));
   BOOST_TEST(capabilities.supports(forge::api::core::capability::server_stream));
   BOOST_TEST(capabilities.supports(forge::api::core::capability::client_stream));
   BOOST_TEST(capabilities.supports(forge::api::core::capability::bidirectional_stream));
   BOOST_TEST(capabilities.supports(forge::api::core::capability::stream_window));
}

BOOST_AUTO_TEST_CASE(binding_rejects_unsupported_descriptor_before_io) {
   auto registry = forge::api::core::registry{};
   auto plan = forge::api::core::binding().serve(registry).build();
   plan.exports.push_back(forge::api::core::descriptor{
       .id = {.value = "unsupported.websocket"},
       .version = {.major = 1, .revision = 0},
       .methods =
           {
               forge::api::core::method_descriptor{
                   .name = "unsupported",
                   .kind = static_cast<forge::api::core::method_kind>(255),
               },
           },
   });

   BOOST_CHECK_THROW(static_cast<void>(forge::api::websocket::api().use(std::move(plan)).build()),
                     forge::api::core::exceptions::protocol_error);
}

BOOST_AUTO_TEST_SUITE_END()
