#include <boost/test/unit_test.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#if defined(__APPLE__) || defined(__linux__)
#include <array>
#include <net/if.h>
#endif

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.transport.buffer;
import forge.net.transport.connector;
import forge.net.transport.endpoint;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.net.transport.listener;
import forge.net.transport.registry;
import forge.net.transport.session;
import forge.net.transport.stream;

namespace {

using bytes = std::vector<std::uint8_t>;

[[nodiscard]] bytes text_bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] forge::net::transport::endpoint endpoint(std::string host,
                                                forge::net::transport::endpoint::protocol_kind protocol,
                                                std::uint16_t port) {
   return forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                   .protocol = protocol,
                                   .host = std::move(host),
                                   .port = port};
}

void check_invalid_literal(const forge::net::transport::endpoint& value) {
   BOOST_CHECK_EXCEPTION(static_cast<void>(value.literal_address()), boost::system::system_error, [](const auto& error) {
      return error.code() == boost::asio::error::invalid_argument;
   });
}

#if defined(__APPLE__) || defined(__linux__)
struct native_interface {
   std::string name;
   unsigned int index = 0;
};

[[nodiscard]] std::optional<native_interface> first_native_interface() {
   auto* interfaces = if_nameindex();
   if (interfaces == nullptr) {
      return std::nullopt;
   }

   auto result = std::optional<native_interface>{};
   for (auto* current = interfaces; current->if_index != 0; ++current) {
      if (current->if_name == nullptr) {
         continue;
      }
      const auto index = if_nametoindex(current->if_name);
      if (index != 0) {
         result = native_interface{.name = current->if_name, .index = index};
         break;
      }
   }
   if_freenameindex(interfaces);
   return result;
}

[[nodiscard]] std::optional<std::string> missing_native_interface_name() {
   for (auto suffix = 0U; suffix < 10000; ++suffix) {
      auto name = "fg" + std::to_string(suffix);
      if (if_nametoindex(name.c_str()) == 0) {
         return name;
      }
   }
   return std::nullopt;
}

[[nodiscard]] std::optional<unsigned int> missing_native_interface_index() {
   for (auto index = 1U; index < 4096; ++index) {
      auto name = std::array<char, IF_NAMESIZE>{};
      if (if_indextoname(index, name.data()) == nullptr) {
         return index;
      }
   }
   return std::nullopt;
}
#endif

class fake_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit fake_stream(std::int64_t stream_id) : id_{stream_id} {}

   [[nodiscard]] bool valid() const noexcept override {
      return open;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return id_;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      ++span_write_count;
      last_write_data = value.data();
      writes.push_back({value.begin(), value.end()});
      co_return;
   }

   boost::asio::awaitable<void> async_write_chunk(forge::net::transport::chunk value) override {
      ++chunk_write_count;
      last_write_data = value.bytes().data();
      writes.push_back(value.to_vector());
      co_return;
   }

   boost::asio::awaitable<bytes> async_read() override {
      ++reads_started;
      BOOST_REQUIRE(!reads.empty());
      auto out = std::move(reads.front());
      reads.pop_front();
      co_return out;
   }

   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk() override {
      ++chunk_reads_started;
      BOOST_REQUIRE(!reads.empty());
      auto out = std::move(reads.front());
      reads.pop_front();
      co_return forge::net::transport::chunk{std::move(out)};
   }

   boost::asio::awaitable<void> async_close() override {
      if (close_entered) {
         close_entered->arrive_and_wait();
      }
      if (close_release) {
         close_release->arrive_and_wait();
      }
      open = false;
      ++close_count;
      if (throw_after_close) {
         throw std::runtime_error{"injected terminal close failure"};
      }
      co_return;
   }

   void cancel() override {
      open = false;
      ++cancel_count;
      if (throw_on_cancel) {
         throw std::runtime_error{"injected legacy cancel failure"};
      }
   }

   std::deque<bytes> reads;
   std::vector<bytes> writes;
   const std::uint8_t* last_write_data = nullptr;
   std::uint64_t reads_started = 0;
   std::uint64_t chunk_reads_started = 0;
   std::uint64_t span_write_count = 0;
   std::uint64_t chunk_write_count = 0;
   std::uint64_t close_count = 0;
   std::uint64_t cancel_count = 0;
   bool throw_on_cancel = false;
   bool throw_after_close = false;
   bool open = true;
   std::barrier<>* close_entered = nullptr;
   std::barrier<>* close_release = nullptr;

 private:
   std::int64_t id_ = 0;
};

class fake_session final : public forge::net::transport::detail::session_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override {
      return open;
   }

   boost::asio::awaitable<forge::net::transport::stream> async_open_stream() override {
      ++open_stream_count;
      co_return forge::net::transport::detail::stream_access::make(std::make_shared<fake_stream>(100 + open_stream_count));
   }

   boost::asio::awaitable<forge::net::transport::stream> async_accept_stream() override {
      ++accept_stream_count;
      BOOST_REQUIRE(!accepted.empty());
      auto out = std::move(accepted.front());
      accepted.pop_front();
      co_return out;
   }

   boost::asio::awaitable<void> async_close() override {
      open = false;
      ++close_count;
      co_return;
   }

   void cancel() override {
      open = false;
      ++cancel_count;
   }

   std::deque<forge::net::transport::stream> accepted;
   std::uint64_t open_stream_count = 0;
   std::uint64_t accept_stream_count = 0;
   std::uint64_t close_count = 0;
   std::uint64_t cancel_count = 0;
   bool open = true;
};

class fake_stream_connector final : public forge::net::transport::detail::stream_connector_concept {
 public:
   explicit fake_stream_connector(forge::net::transport::endpoint local_value) : local{std::move(local_value)} {}

   [[nodiscard]] bool valid() const noexcept override {
      return active;
   }

   boost::asio::awaitable<forge::net::transport::stream_connection>
   async_connect(forge::net::transport::endpoint remote, forge::net::transport::connect_options options) override {
      ++connect_count;
      last_remote = remote;
      last_limits = options.limits;
      co_return forge::net::transport::stream_connection{
          .local_endpoint = local,
          .remote_endpoint = std::move(remote),
          .stream = forge::net::transport::detail::stream_access::make(std::make_shared<fake_stream>(700)),
      };
   }

   void cancel() override {
      active = false;
      ++cancel_count;
   }

   forge::net::transport::endpoint local;
   forge::net::transport::endpoint last_remote;
   forge::net::transport::limits last_limits;
   std::uint64_t connect_count = 0;
   std::uint64_t cancel_count = 0;
   bool active = true;
};

class fake_session_connector final : public forge::net::transport::detail::session_connector_concept {
 public:
   explicit fake_session_connector(forge::net::transport::endpoint local_value) : local{std::move(local_value)} {}

   [[nodiscard]] bool valid() const noexcept override {
      return active;
   }

   boost::asio::awaitable<forge::net::transport::session_connection>
   async_connect(forge::net::transport::endpoint remote, forge::net::transport::connect_options options) override {
      ++connect_count;
      last_remote = remote;
      last_limits = options.limits;
      co_return forge::net::transport::session_connection{
          .local_endpoint = local,
          .remote_endpoint = std::move(remote),
          .session = forge::net::transport::detail::session_access::make(std::make_shared<fake_session>()),
      };
   }

   void cancel() override {
      active = false;
      ++cancel_count;
   }

   forge::net::transport::endpoint local;
   forge::net::transport::endpoint last_remote;
   forge::net::transport::limits last_limits;
   std::uint64_t connect_count = 0;
   std::uint64_t cancel_count = 0;
   bool active = true;
};

class fake_stream_listener final : public forge::net::transport::detail::stream_listener_concept {
 public:
   explicit fake_stream_listener(forge::net::transport::endpoint local_value) : local{std::move(local_value)} {}

   [[nodiscard]] bool valid() const noexcept override {
      return active;
   }

   [[nodiscard]] forge::net::transport::endpoint local_endpoint() const override {
      return local;
   }

   boost::asio::awaitable<forge::net::transport::stream_connection> async_accept() override {
      ++accept_count;
      BOOST_REQUIRE(!accepted.empty());
      auto out = std::move(accepted.front());
      accepted.pop_front();
      co_return out;
   }

   boost::asio::awaitable<void> async_close() override {
      active = false;
      ++close_count;
      co_return;
   }

   void cancel() override {
      active = false;
      ++cancel_count;
   }

   forge::net::transport::endpoint local;
   std::deque<forge::net::transport::stream_connection> accepted;
   std::uint64_t accept_count = 0;
   std::uint64_t close_count = 0;
   std::uint64_t cancel_count = 0;
   bool active = true;
};

class fake_session_listener final : public forge::net::transport::detail::session_listener_concept {
 public:
   explicit fake_session_listener(forge::net::transport::endpoint local_value) : local{std::move(local_value)} {}

   [[nodiscard]] bool valid() const noexcept override {
      return active;
   }

   [[nodiscard]] forge::net::transport::endpoint local_endpoint() const override {
      return local;
   }

   boost::asio::awaitable<forge::net::transport::session_connection> async_accept() override {
      ++accept_count;
      BOOST_REQUIRE(!accepted.empty());
      auto out = std::move(accepted.front());
      accepted.pop_front();
      co_return out;
   }

   boost::asio::awaitable<void> async_close() override {
      active = false;
      ++close_count;
      co_return;
   }

   void cancel() override {
      active = false;
      ++cancel_count;
   }

   forge::net::transport::endpoint local;
   std::deque<forge::net::transport::session_connection> accepted;
   std::uint64_t accept_count = 0;
   std::uint64_t close_count = 0;
   std::uint64_t cancel_count = 0;
   bool active = true;
};

[[nodiscard]] forge::net::transport::stream make_stream(std::shared_ptr<fake_stream> model) {
   return forge::net::transport::detail::stream_access::make(std::move(model));
}

[[nodiscard]] forge::net::transport::session make_session(std::shared_ptr<fake_session> model) {
   return forge::net::transport::detail::session_access::make(std::move(model));
}

[[nodiscard]] forge::net::transport::stream_connector make_stream_connector(std::shared_ptr<fake_stream_connector> model) {
   return forge::net::transport::detail::stream_connector_access::make(std::move(model));
}

[[nodiscard]] forge::net::transport::session_connector make_session_connector(std::shared_ptr<fake_session_connector> model) {
   return forge::net::transport::detail::session_connector_access::make(std::move(model));
}

[[nodiscard]] forge::net::transport::stream_listener make_stream_listener(std::shared_ptr<fake_stream_listener> model) {
   return forge::net::transport::detail::stream_listener_access::make(std::move(model));
}

[[nodiscard]] forge::net::transport::session_listener make_session_listener(std::shared_ptr<fake_session_listener> model) {
   return forge::net::transport::detail::session_listener_access::make(std::move(model));
}

} // namespace

BOOST_AUTO_TEST_SUITE(transport)

BOOST_AUTO_TEST_CASE(transport_chunk_and_pool_reuse_bounded_storage) {
   auto pool = forge::net::transport::buffer_pool{
       forge::net::transport::buffer_pool_options{.default_capacity = 8, .max_cached_buffers = 1, .max_cached_bytes = 32}};

   const std::uint8_t* released_storage = nullptr;
   {
      auto builder = pool.acquire(8);
      auto writable = builder.writable();
      BOOST_REQUIRE_GE(writable.size(), 8U);
      released_storage = writable.data();
      const auto payload = text_bytes("abc");
      std::copy(payload.begin(), payload.end(), writable.begin());

      auto value = builder.commit(payload.size());
      BOOST_CHECK_EQUAL(value.size(), payload.size());
      BOOST_CHECK_EQUAL_COLLECTIONS(value.bytes().begin(), value.bytes().end(), payload.begin(), payload.end());
      const auto copied = value.to_vector();
      BOOST_CHECK_EQUAL_COLLECTIONS(copied.begin(), copied.end(), payload.begin(), payload.end());
      BOOST_CHECK_THROW((void)builder.commit(0), forge::net::transport::exceptions::invalid_buffer);
   }

   auto cached = pool.cached();
   BOOST_CHECK_EQUAL(cached.buffers, 1U);
   BOOST_CHECK_GE(cached.bytes, 8U);

   auto reused = pool.acquire(8);
   BOOST_CHECK_EQUAL(reused.writable().data(), released_storage);
   BOOST_CHECK_THROW((void)reused.commit(reused.writable().size() + 1), forge::net::transport::exceptions::invalid_buffer);

   {
      auto a = pool.copy(text_bytes("one"));
      auto b = pool.copy(text_bytes("two"));
      (void)a;
      (void)b;
   }
   cached = pool.cached();
   BOOST_CHECK_EQUAL(cached.buffers, 1U);
   BOOST_CHECK_LE(cached.bytes, 32U);
}

BOOST_AUTO_TEST_CASE(transport_buffer_pool_drops_oversized_returned_storage) {
   auto pool = forge::net::transport::buffer_pool{forge::net::transport::buffer_pool_options{
       .default_capacity = 8,
       .max_cached_buffers = 4,
       .max_cached_bytes = 64,
       .max_cached_buffer_capacity = 16,
   }};

   {
      auto builder = pool.acquire(32);
      auto writable = builder.writable();
      BOOST_REQUIRE_GE(writable.size(), 32U);
   }
   auto cached = pool.cached();
   BOOST_TEST(cached.buffers == 0U);
   BOOST_TEST(cached.bytes == 0U);

   const std::uint8_t* released_storage = nullptr;
   {
      auto builder = pool.acquire(12);
      released_storage = builder.writable().data();
   }
   cached = pool.cached();
   BOOST_TEST(cached.buffers == 1U);
   BOOST_TEST(cached.bytes <= 64U);

   auto reused = pool.acquire(12);
   BOOST_TEST(reused.writable().data() == released_storage);
}

BOOST_AUTO_TEST_CASE(transport_stream_delegates_and_preserves_buffered_frames) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(42);
   auto combined = forge::net::transport::encode_frame(text_bytes("one"));
   auto second = forge::net::transport::encode_frame(text_bytes("two"));
   combined.insert(combined.end(), second.begin(), second.end());
   model->reads.push_back(combined);

   auto value = make_stream(model);
   BOOST_CHECK(value.valid());
   BOOST_CHECK_EQUAL(value.id(), 42);

   const auto expected_one = text_bytes("one");
   const auto expected_two = text_bytes("two");
   auto first_payload = forge::asio::blocking::run(runtime, value.async_read_frame());
   BOOST_CHECK_EQUAL_COLLECTIONS(first_payload.begin(), first_payload.end(), expected_one.begin(), expected_one.end());
   auto second_payload = forge::asio::blocking::run(runtime, value.async_read_frame());
   BOOST_CHECK_EQUAL_COLLECTIONS(second_payload.begin(), second_payload.end(), expected_two.begin(), expected_two.end());
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 1U);
   BOOST_CHECK_EQUAL(model->reads_started, 0U);

   forge::asio::blocking::run(runtime, value.async_write_frame(text_bytes("out")));
   const auto expected_write = forge::net::transport::encode_frame(text_bytes("out"));
   BOOST_REQUIRE_EQUAL(model->writes.size(), 1U);
   BOOST_CHECK_EQUAL_COLLECTIONS(
       model->writes.front().begin(), model->writes.front().end(), expected_write.begin(), expected_write.end());

   forge::asio::blocking::run(runtime, value.async_close());
   BOOST_CHECK_EQUAL(model->close_count, 1U);
   BOOST_CHECK(!value.valid());
}

BOOST_AUTO_TEST_CASE(transport_stream_delegates_chunk_read_write_without_forcing_vector_path) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(46);
   model->reads.push_back(text_bytes("chunk read"));
   auto value = make_stream(model);

   auto written = forge::net::transport::chunk{text_bytes("chunk write")};
   const auto* written_data = written.bytes().data();
   forge::asio::blocking::run(runtime, value.async_write(std::move(written)));
   BOOST_CHECK_EQUAL(model->chunk_write_count, 1U);
   BOOST_CHECK_EQUAL(model->span_write_count, 0U);
   BOOST_CHECK_EQUAL(model->last_write_data, written_data);
   const auto expected_write = text_bytes("chunk write");
   BOOST_REQUIRE_EQUAL(model->writes.size(), 1U);
   BOOST_CHECK_EQUAL_COLLECTIONS(
       model->writes.front().begin(), model->writes.front().end(), expected_write.begin(), expected_write.end());

   auto read = forge::asio::blocking::run(runtime, value.async_read_chunk());
   const auto expected_read = text_bytes("chunk read");
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 1U);
   BOOST_CHECK_EQUAL(model->reads_started, 0U);
   BOOST_CHECK_EQUAL_COLLECTIONS(read.bytes().begin(), read.bytes().end(), expected_read.begin(), expected_read.end());
}

BOOST_AUTO_TEST_CASE(transport_stream_reads_framed_chunks_and_preserves_trailing_bytes_without_extra_reads) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(47);
   auto combined = forge::net::transport::encode_frame(text_bytes("one"));
   auto second = forge::net::transport::encode_frame(text_bytes("two"));
   combined.insert(combined.end(), second.begin(), second.end());
   model->reads.push_back(combined);

   auto value = make_stream(model);
   auto first = forge::asio::blocking::run(runtime, value.async_read_frame_chunk());
   auto second_value = forge::asio::blocking::run(runtime, value.async_read_frame_chunk());

   const auto expected_one = text_bytes("one");
   const auto expected_two = text_bytes("two");
   BOOST_CHECK_EQUAL_COLLECTIONS(first.bytes().begin(), first.bytes().end(), expected_one.begin(), expected_one.end());
   BOOST_CHECK_EQUAL_COLLECTIONS(
       second_value.bytes().begin(), second_value.bytes().end(), expected_two.begin(), expected_two.end());
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 1U);
   BOOST_CHECK_EQUAL(model->reads_started, 0U);
}

BOOST_AUTO_TEST_CASE(transport_stream_rejects_oversized_headers_before_reading_a_body) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(48);
   model->reads.push_back({0x00, 0x00, 0x00, 0x11});
   model->reads.push_back(text_bytes("unread body"));
   auto value = make_stream(model);

   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(
                         runtime, value.async_read_frame(forge::net::transport::frame_options{.max_size = 16}))),
                     forge::net::transport::exceptions::frame_too_large);
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 1U);
   BOOST_REQUIRE_EQUAL(model->reads.size(), 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_bounds_retained_coalesced_input_without_dropping_valid_frames) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(49);
   const auto options = forge::net::transport::frame_options{.max_size = 3};
   auto combined = forge::net::transport::encode_frame(text_bytes("one"), options);
   const auto second = forge::net::transport::encode_frame(text_bytes("two"), options);
   combined.insert(combined.end(), second.begin(), second.end());
   BOOST_REQUIRE_EQUAL(combined.size(), forge::net::transport::frame_buffer_limit(options));
   model->reads.push_back(std::move(combined));
   auto value = make_stream(model);

   const auto first = forge::asio::blocking::run(runtime, value.async_read_frame(options));
   const auto next = forge::asio::blocking::run(runtime, value.async_read_frame(options));
   const auto expected_first = text_bytes("one");
   const auto expected_next = text_bytes("two");

   BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(), expected_first.begin(), expected_first.end());
   BOOST_CHECK_EQUAL_COLLECTIONS(next.begin(), next.end(), expected_next.begin(), expected_next.end());
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_rejects_oversized_backend_chunks_without_retaining_them) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(50);
   const auto options = forge::net::transport::frame_options{.max_size = 16, .max_buffered_size = 40};
   auto oversized = forge::net::transport::encode_frame(text_bytes("one"), options);
   oversized.resize(options.max_buffered_size + 1, 0);
   model->reads.push_back(std::move(oversized));
   model->reads.push_back(forge::net::transport::encode_frame(text_bytes("next"), options));
   auto value = make_stream(model);

   BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, value.async_read_frame(options))),
                     forge::net::transport::exceptions::frame_too_large);
   const auto next = forge::asio::blocking::run(runtime, value.async_read_frame(options));
   const auto expected = text_bytes("next");

   BOOST_CHECK_EQUAL_COLLECTIONS(next.begin(), next.end(), expected.begin(), expected.end());
   BOOST_CHECK_EQUAL(model->chunk_reads_started, 2U);
}

BOOST_AUTO_TEST_CASE(transport_stream_write_owns_caller_buffer_across_await) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(43);
   auto value = make_stream(model);
   const auto payload = text_bytes("owned write payload");

   forge::asio::blocking::run(runtime, value.async_write(payload));

   BOOST_REQUIRE_EQUAL(model->writes.size(), 1U);
   BOOST_CHECK_EQUAL_COLLECTIONS(model->writes.front().begin(), model->writes.front().end(), payload.begin(), payload.end());
   BOOST_CHECK(model->last_write_data != payload.data());
}

BOOST_AUTO_TEST_CASE(transport_stream_write_frame_owns_encoded_buffer_across_await) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(44);
   auto value = make_stream(model);
   const auto payload = text_bytes("framed write payload");
   const auto expected_write = forge::net::transport::encode_frame(payload);

   forge::asio::blocking::run(runtime, value.async_write_frame(payload));

   BOOST_REQUIRE_EQUAL(model->writes.size(), 1U);
   BOOST_CHECK_EQUAL_COLLECTIONS(
       model->writes.front().begin(), model->writes.front().end(), expected_write.begin(), expected_write.end());
   BOOST_CHECK(model->last_write_data != payload.data());
   BOOST_CHECK(model->last_write_data != expected_write.data());
}

BOOST_AUTO_TEST_CASE(transport_stream_cancel_delegates_to_backend) {
   auto model = std::make_shared<fake_stream>(45);
   auto value = make_stream(model);

   BOOST_TEST(value.valid());
   value.cancel();

   BOOST_TEST(!value.valid());
   BOOST_CHECK_EQUAL(model->cancel_count, 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_request_cancel_contains_throwing_legacy_backend) {
   auto model = std::make_shared<fake_stream>(46);
   model->throw_on_cancel = true;
   auto value = make_stream(model);

   BOOST_CHECK_NO_THROW(value.request_cancel());
   BOOST_TEST(!value.valid());
   BOOST_CHECK_EQUAL(model->cancel_count, 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_foreign_request_cancel_interrupts_incomplete_close_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto close_entered = std::barrier{2};
   auto close_release = std::barrier{2};
   auto model = std::make_shared<fake_stream>(47);
   model->close_entered = &close_entered;
   model->close_release = &close_release;
   auto cancel_requests = std::atomic_size_t{};
   auto value = forge::net::transport::detail::stream_access::make_cancelable(
       model, [&cancel_requests]() noexcept { cancel_requests.fetch_add(1, std::memory_order_relaxed); });

   auto closed = boost::asio::co_spawn(runtime.context(), value.async_close(), boost::asio::use_future);
   close_entered.arrive_and_wait();
   auto requester = std::thread{[&value] { value.request_cancel(); }};
   requester.join();

   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 1U);
   close_release.arrive_and_wait();
   BOOST_CHECK_NO_THROW(closed.get());
   value.request_cancel();
   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(model->close_count == 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_close_exception_is_a_shared_terminal_barrier) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto close_entered = std::barrier{2};
   auto close_release = std::barrier{2};
   auto model = std::make_shared<fake_stream>(48);
   model->close_entered = &close_entered;
   model->close_release = &close_release;
   model->throw_after_close = true;
   auto value = make_stream(model);

   auto first = boost::asio::co_spawn(runtime.context(), value.async_close(), boost::asio::use_future);
   close_entered.arrive_and_wait();
   auto second = boost::asio::co_spawn(runtime.context(), value.async_close(), boost::asio::use_future);

   BOOST_TEST(static_cast<int>(first.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(static_cast<int>(second.wait_for(std::chrono::milliseconds{0})) ==
              static_cast<int>(std::future_status::timeout));
   BOOST_TEST(model->open);
   BOOST_TEST(model->close_count == 0U);

   close_release.arrive_and_wait();
   BOOST_CHECK_THROW(first.get(), std::runtime_error);
   BOOST_CHECK_THROW(second.get(), std::runtime_error);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, value.async_close()), std::runtime_error);
   BOOST_TEST(!model->open);
   BOOST_TEST(model->close_count == 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_graceful_close_is_not_reset_by_facade_destruction) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(49);
   auto cancel_requests = std::atomic_size_t{};
   {
      auto value = forge::net::transport::detail::stream_access::make_cancelable(
          model, [&cancel_requests]() noexcept { cancel_requests.fetch_add(1, std::memory_order_relaxed); });
      forge::asio::blocking::run(runtime, value.async_close());
   }

   BOOST_TEST(model->close_count == 1U);
   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 0U);
}

BOOST_AUTO_TEST_CASE(transport_stream_distinguishes_explicit_cancel_from_managed_abandonment) {
   auto model = std::make_shared<fake_stream>(50);
   auto cancel_requests = std::atomic_size_t{};
   auto abandon_requests = std::atomic_size_t{};
   {
      auto value = forge::net::transport::detail::stream_access::make_cancelable(
          model,
          [&cancel_requests]() noexcept { cancel_requests.fetch_add(1, std::memory_order_relaxed); },
          [&abandon_requests]() noexcept { abandon_requests.fetch_add(1, std::memory_order_relaxed); });
   }

   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 0U);
   BOOST_TEST(abandon_requests.load(std::memory_order_relaxed) == 1U);

   auto value = forge::net::transport::detail::stream_access::make_cancelable(
       model,
       [&cancel_requests]() noexcept { cancel_requests.fetch_add(1, std::memory_order_relaxed); },
       [&abandon_requests]() noexcept { abandon_requests.fetch_add(1, std::memory_order_relaxed); });
   value.request_cancel();
   value.request_cancel();
   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(abandon_requests.load(std::memory_order_relaxed) == 1U);
}

BOOST_AUTO_TEST_CASE(transport_stream_concurrent_request_cancel_invokes_immutable_callback_once) {
   constexpr auto requester_count = std::size_t{4};
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_stream>(48);
   auto cancel_requests = std::atomic_size_t{};
   auto value = forge::net::transport::detail::stream_access::make_cancelable(
       model, [model, &cancel_requests]() noexcept {
          cancel_requests.fetch_add(1, std::memory_order_relaxed);
          model->cancel();
       });
   auto start = std::barrier{static_cast<std::ptrdiff_t>(requester_count + 1)};
   auto requesters = std::vector<std::thread>{};
   requesters.reserve(requester_count);
   for (auto index = std::size_t{}; index < requester_count; ++index) {
      requesters.emplace_back([&] {
         start.arrive_and_wait();
         value.request_cancel();
      });
   }
   start.arrive_and_wait();
   for (auto& requester : requesters) {
      requester.join();
   }

   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(model->cancel_count == 1U);
   forge::asio::blocking::run(runtime, value.async_close());
   BOOST_TEST(model->close_count == 1U);
   BOOST_TEST(cancel_requests.load(std::memory_order_relaxed) == 1U);
}

BOOST_AUTO_TEST_CASE(transport_session_delegates_open_accept_close_cancel) {
   auto runtime = forge::asio::runtime{};
   auto model = std::make_shared<fake_session>();
   model->accepted.push_back(make_stream(std::make_shared<fake_stream>(501)));

   auto value = make_session(model);
   BOOST_CHECK(value.valid());

   auto opened = forge::asio::blocking::run(runtime, value.async_open_stream());
   BOOST_CHECK_EQUAL(opened.id(), 101);
   auto accepted = forge::asio::blocking::run(runtime, value.async_accept_stream());
   BOOST_CHECK_EQUAL(accepted.id(), 501);
   BOOST_CHECK_EQUAL(model->open_stream_count, 1U);
   BOOST_CHECK_EQUAL(model->accept_stream_count, 1U);

   value.cancel();
   BOOST_CHECK_EQUAL(model->cancel_count, 1U);
   BOOST_CHECK(!value.valid());
}

BOOST_AUTO_TEST_CASE(transport_frame_handles_partial_multiple_and_limit) {
   const auto encoded_a = forge::net::transport::encode_frame(text_bytes("a"));
   auto partial = bytes{encoded_a.begin(), encoded_a.begin() + 2};
   auto decoded_partial = forge::net::transport::decode_frame(partial);
   BOOST_CHECK(decoded_partial.status == forge::net::transport::frame_decode_status::need_more_data);

   auto encoded_b = forge::net::transport::encode_frame(text_bytes("b"));
   auto combined = encoded_a;
   combined.insert(combined.end(), encoded_b.begin(), encoded_b.end());
   auto first = forge::net::transport::decode_frame(combined);
   BOOST_REQUIRE(first.status == forge::net::transport::frame_decode_status::complete);
   BOOST_CHECK_EQUAL(first.consumed, encoded_a.size());
   const auto expected_a = text_bytes("a");
   const auto expected_b = text_bytes("b");
   BOOST_CHECK_EQUAL_COLLECTIONS(first.payload.begin(), first.payload.end(), expected_a.begin(), expected_a.end());
   auto second = forge::net::transport::decode_frame({combined.data() + first.consumed, combined.size() - first.consumed});
   BOOST_REQUIRE(second.status == forge::net::transport::frame_decode_status::complete);
   BOOST_CHECK_EQUAL_COLLECTIONS(second.payload.begin(), second.payload.end(), expected_b.begin(), expected_b.end());

   BOOST_CHECK_THROW((void)forge::net::transport::encode_frame(text_bytes("toolong"), forge::net::transport::frame_options{.max_size = 3}),
                     forge::net::transport::exceptions::frame_too_large);
   const auto over_limit = forge::net::transport::encode_frame(text_bytes("toolong"));
   BOOST_CHECK_THROW((void)forge::net::transport::decode_frame(over_limit, forge::net::transport::frame_options{.max_size = 3}),
                     forge::net::transport::exceptions::frame_too_large);
}

BOOST_AUTO_TEST_CASE(transport_frame_view_decodes_without_payload_copy) {
   const auto encoded_a = forge::net::transport::encode_frame(text_bytes("a"));
   auto partial = bytes{encoded_a.begin(), encoded_a.begin() + 2};
   auto decoded_partial = forge::net::transport::decode_frame_view(partial);
   BOOST_CHECK(decoded_partial.status == forge::net::transport::frame_decode_status::need_more_data);

   auto encoded_b = forge::net::transport::encode_frame(text_bytes("b"));
   auto combined = encoded_a;
   combined.insert(combined.end(), encoded_b.begin(), encoded_b.end());
   auto first = forge::net::transport::decode_frame_view(combined);
   BOOST_REQUIRE(first.status == forge::net::transport::frame_decode_status::complete);
   BOOST_CHECK_EQUAL(first.consumed, encoded_a.size());
   BOOST_CHECK_EQUAL(first.payload.data(), combined.data() + 4);
   BOOST_CHECK_EQUAL(first.payload.size(), 1U);
   BOOST_CHECK_EQUAL(first.payload.front(), static_cast<std::uint8_t>('a'));

   auto second = forge::net::transport::decode_frame_view({combined.data() + first.consumed, combined.size() - first.consumed});
   BOOST_REQUIRE(second.status == forge::net::transport::frame_decode_status::complete);
   BOOST_CHECK_EQUAL(second.payload.data(), combined.data() + first.consumed + 4);
   BOOST_CHECK_EQUAL(second.payload.front(), static_cast<std::uint8_t>('b'));

   auto encoded = bytes{};
   forge::net::transport::encode_frame_to(encoded, text_bytes("view"));
   const auto expected = forge::net::transport::encode_frame(text_bytes("view"));
   BOOST_CHECK_EQUAL_COLLECTIONS(encoded.begin(), encoded.end(), expected.begin(), expected.end());

   const auto over_limit = forge::net::transport::encode_frame(text_bytes("toolong"));
   BOOST_CHECK_THROW((void)forge::net::transport::decode_frame_view(over_limit, forge::net::transport::frame_options{.max_size = 3}),
                     forge::net::transport::exceptions::frame_too_large);
}

BOOST_AUTO_TEST_CASE(transport_endpoint_formats_authority_by_host_kind) {
   auto ip4 = endpoint("127.0.0.1", forge::net::transport::endpoint::protocol_kind::tcp, 443);
   BOOST_CHECK_EQUAL(ip4.authority(), "127.0.0.1:443");

   auto dns = endpoint("example.com", forge::net::transport::endpoint::protocol_kind::tcp, 8443);
   dns.host_type = forge::net::transport::endpoint::host_kind::dns;
   BOOST_CHECK_EQUAL(dns.authority(), "example.com:8443");

   auto ip6 = endpoint("2001:db8::1", forge::net::transport::endpoint::protocol_kind::tcp, 443);
   ip6.host_type = forge::net::transport::endpoint::host_kind::ip6;
   BOOST_CHECK_EQUAL(ip6.authority(), "[2001:db8::1]:443");
}

BOOST_AUTO_TEST_CASE(transport_endpoint_requires_strict_native_literals) {
   using endpoint_type = forge::net::transport::endpoint;

   const auto ip4 = endpoint_type{.host_type = endpoint_type::host_kind::ip4,
                                  .protocol = endpoint_type::protocol_kind::tcp,
                                  .host = "127.0.0.1",
                                  .port = 4001};
   BOOST_CHECK(ip4.literal_address().is_v4());

   auto embedded_nul = ip4;
   embedded_nul.host = std::string{"127.0.0.1\0suffix", 16};
   check_invalid_literal(embedded_nul);

   auto scoped_ip4 = ip4;
   scoped_ip4.zone = "1";
   check_invalid_literal(scoped_ip4);

   const auto dns = endpoint_type{.host_type = endpoint_type::host_kind::dns,
                                  .protocol = endpoint_type::protocol_kind::tcp,
                                  .host = "localhost",
                                  .port = 4001,
                                  .zone = "1"};
   check_invalid_literal(dns);

   const auto link_local = endpoint_type{.host_type = endpoint_type::host_kind::ip6,
                                         .protocol = endpoint_type::protocol_kind::tcp,
                                         .host = "fe80::1",
                                         .port = 4001};
   check_invalid_literal(link_local);

   const auto empty_legacy_zone = endpoint_type{.host_type = endpoint_type::host_kind::ip6,
                                                .protocol = endpoint_type::protocol_kind::tcp,
                                                .host = "::1%",
                                                .port = 4001};
   check_invalid_literal(empty_legacy_zone);

   auto nul_zone = endpoint_type{.host_type = endpoint_type::host_kind::ip6,
                                 .protocol = endpoint_type::protocol_kind::tcp,
                                 .host = "::1",
                                 .port = 4001};
   nul_zone.zone = std::string{"lo\0suffix", 9};
   check_invalid_literal(nul_zone);

#if defined(__APPLE__) || defined(__linux__)
   const auto scope = first_native_interface();
   if (!scope) {
      BOOST_TEST_MESSAGE("no native interface available for scoped literal validation");
      return;
   }

   const auto scoped = endpoint_type{.host_type = endpoint_type::host_kind::ip6,
                                     .protocol = endpoint_type::protocol_kind::tcp,
                                     .host = "::1",
                                     .port = 4001,
                                     .zone = scope->name};
   BOOST_CHECK_EQUAL(scoped.literal_address().to_v6().scope_id(), scope->index);

   auto numeric_scope = scoped;
   numeric_scope.zone = std::to_string(scope->index);
   BOOST_CHECK_EQUAL(numeric_scope.literal_address().to_v6().scope_id(), scope->index);

   auto legacy_scope = scoped;
   legacy_scope.host += "%" + scope->name;
   legacy_scope.zone.clear();
   BOOST_CHECK_EQUAL(legacy_scope.literal_address().to_v6().scope_id(), scope->index);

   auto ambiguous_scope = scoped;
   ambiguous_scope.host += "%" + scope->name;
   check_invalid_literal(ambiguous_scope);

   auto unknown_scope = scoped;
   const auto unknown_name = missing_native_interface_name();
   BOOST_REQUIRE(unknown_name.has_value());
   unknown_scope.zone = *unknown_name;
   check_invalid_literal(unknown_scope);

   const auto unknown_index = missing_native_interface_index();
   BOOST_REQUIRE(unknown_index.has_value());
   auto unknown_numeric_scope = scoped;
   unknown_numeric_scope.zone = std::to_string(*unknown_index);
   check_invalid_literal(unknown_numeric_scope);

   auto zero_scope = scoped;
   zero_scope.zone = "0";
   check_invalid_literal(zero_scope);

   auto overflow_scope = scoped;
   overflow_scope.zone = std::to_string(std::numeric_limits<unsigned int>::max()) + "0";
   check_invalid_literal(overflow_scope);
#else
   const auto unsupported_scope = endpoint_type{.host_type = endpoint_type::host_kind::ip6,
                                                .protocol = endpoint_type::protocol_kind::tcp,
                                                .host = "::1",
                                                .port = 4001,
                                                .zone = "scope"};
   BOOST_CHECK_EXCEPTION(static_cast<void>(unsupported_scope.literal_address()), boost::system::system_error,
                         [](const auto& error) { return error.code() == boost::asio::error::operation_not_supported; });
#endif
}

BOOST_AUTO_TEST_CASE(transport_endpoint_from_address_preserves_numeric_scope_without_lookup) {
   using endpoint_type = forge::net::transport::endpoint;
   const auto scope = std::numeric_limits<boost::asio::ip::scope_id_type>::max();
   const auto scoped_loopback = boost::asio::ip::address_v6{boost::asio::ip::address_v6::loopback().to_bytes(), scope};

   const auto endpoint = endpoint_type::from_address(scoped_loopback, 4001, endpoint_type::protocol_kind::tcp);
   BOOST_CHECK(endpoint.host_type == endpoint_type::host_kind::ip6);
   BOOST_CHECK_EQUAL(endpoint.host, "::1");
   BOOST_CHECK_EQUAL(endpoint.zone, std::to_string(scope));
   BOOST_CHECK_EQUAL(endpoint.port, 4001U);
   BOOST_CHECK(endpoint.protocol == endpoint_type::protocol_kind::tcp);

   const auto ip4 = endpoint_type::from_address(boost::asio::ip::make_address_v4("192.0.2.1"), 4002,
                                                endpoint_type::protocol_kind::quic_v1);
   BOOST_CHECK(ip4.host_type == endpoint_type::host_kind::ip4);
   BOOST_CHECK_EQUAL(ip4.host, "192.0.2.1");
   BOOST_CHECK(ip4.zone.empty());
}

BOOST_AUTO_TEST_CASE(transport_connector_listener_wrappers_preserve_endpoints) {
   auto runtime = forge::asio::runtime{};
   const auto local = endpoint("127.0.0.1", forge::net::transport::endpoint::protocol_kind::tcp, 7001);
   const auto remote = endpoint("127.0.0.2", forge::net::transport::endpoint::protocol_kind::tcp, 7002);
   const auto connection_limits = forge::net::transport::limits{.max_connections = 7};

   auto stream_connector_model = std::make_shared<fake_stream_connector>(local);
   auto stream_connector = make_stream_connector(stream_connector_model);
   auto stream_conn = forge::asio::blocking::run(
       runtime, stream_connector.async_connect(remote, forge::net::transport::connect_options{.limits = connection_limits}));
   BOOST_CHECK_EQUAL(stream_conn.local_endpoint.port, local.port);
   BOOST_CHECK_EQUAL(stream_conn.remote_endpoint.host, remote.host);
   BOOST_CHECK_EQUAL(stream_conn.stream.id(), 700);
   BOOST_CHECK_EQUAL(stream_connector_model->last_limits.max_connections, connection_limits.max_connections);
   stream_connector.cancel();
   BOOST_CHECK_EQUAL(stream_connector_model->cancel_count, 1U);
   BOOST_CHECK(!stream_connector.valid());

   auto session_connector_model = std::make_shared<fake_session_connector>(local);
   auto session_connector = make_session_connector(session_connector_model);
   auto session_conn = forge::asio::blocking::run(
       runtime, session_connector.async_connect(remote, forge::net::transport::connect_options{.limits = connection_limits}));
   BOOST_CHECK_EQUAL(session_conn.local_endpoint.port, local.port);
   BOOST_CHECK_EQUAL(session_conn.remote_endpoint.host, remote.host);
   BOOST_CHECK(session_conn.session.valid());
   BOOST_CHECK_EQUAL(session_connector_model->last_limits.max_connections, connection_limits.max_connections);

   auto stream_listener_model = std::make_shared<fake_stream_listener>(local);
   stream_listener_model->accepted.push_back(forge::net::transport::stream_connection{
       .local_endpoint = local,
       .remote_endpoint = remote,
       .stream = make_stream(std::make_shared<fake_stream>(800)),
   });
   auto stream_listener = make_stream_listener(stream_listener_model);
   BOOST_CHECK_EQUAL(stream_listener.local_endpoint().port, local.port);
   auto accepted_stream = forge::asio::blocking::run(runtime, stream_listener.async_accept());
   BOOST_CHECK_EQUAL(accepted_stream.stream.id(), 800);
   forge::asio::blocking::run(runtime, stream_listener.async_close());
   BOOST_CHECK(!stream_listener.valid());

   auto session_listener_model = std::make_shared<fake_session_listener>(local);
   session_listener_model->accepted.push_back(forge::net::transport::session_connection{
       .local_endpoint = local,
       .remote_endpoint = remote,
       .session = make_session(std::make_shared<fake_session>()),
   });
   auto session_listener = make_session_listener(session_listener_model);
   auto accepted_session = forge::asio::blocking::run(runtime, session_listener.async_accept());
   BOOST_CHECK(accepted_session.session.valid());
   session_listener.cancel();
   BOOST_CHECK_EQUAL(session_listener_model->cancel_count, 1U);
}

BOOST_AUTO_TEST_CASE(transport_registry_routes_and_rejects_missing_or_duplicate) {
   auto runtime = forge::asio::runtime{};
   auto registry = forge::net::transport::registry{};
   const auto local = endpoint("127.0.0.1", forge::net::transport::endpoint::protocol_kind::tcp, 9001);
   const auto remote = endpoint("127.0.0.2", forge::net::transport::endpoint::protocol_kind::tcp, 9002);

   auto connector_model = std::make_shared<fake_stream_connector>(local);
   auto listener_model = std::make_shared<fake_stream_listener>(local);
   auto received_listen_limits = forge::net::transport::limits{};
   registry.register_stream(
       forge::net::transport::endpoint::protocol_kind::tcp,
       [connector_model] { return make_stream_connector(connector_model); },
       [listener_model, &received_listen_limits](forge::net::transport::endpoint, forge::net::transport::listen_options options)
           -> boost::asio::awaitable<forge::net::transport::stream_listener> {
          received_listen_limits = options.limits;
          co_return make_stream_listener(listener_model);
       });
   BOOST_CHECK(registry.has_stream(forge::net::transport::endpoint::protocol_kind::tcp));
   BOOST_CHECK(!registry.has_session(forge::net::transport::endpoint::protocol_kind::tcp));

   auto connected = forge::asio::blocking::run(
       runtime, registry.async_connect_stream(remote, forge::net::transport::connect_options{.limits = {.max_connections = 11}}));
   BOOST_CHECK_EQUAL(connected.local_endpoint.port, local.port);
   BOOST_CHECK_EQUAL(connected.remote_endpoint.port, remote.port);
   BOOST_CHECK_EQUAL(connector_model->connect_count, 1U);
   BOOST_CHECK_EQUAL(connector_model->last_limits.max_connections, 11U);

   auto listener = forge::asio::blocking::run(
       runtime, registry.async_listen_stream(local, forge::net::transport::listen_options{.limits = {.max_connections = 13}}));
   BOOST_CHECK_EQUAL(listener.local_endpoint().port, local.port);
   BOOST_CHECK_EQUAL(received_listen_limits.max_connections, 13U);

   BOOST_CHECK_THROW(registry.register_stream(
                         forge::net::transport::endpoint::protocol_kind::tcp,
                         [] { return forge::net::transport::stream_connector{}; },
                         [](forge::net::transport::endpoint, forge::net::transport::listen_options)
                             -> boost::asio::awaitable<forge::net::transport::stream_listener> {
                            co_return forge::net::transport::stream_listener{};
                         }),
                     forge::net::transport::exceptions::duplicate_registration);

   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, registry.async_connect_session(remote, {})),
                     forge::net::transport::exceptions::unsupported_protocol);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, registry.async_listen_session(remote, {})),
                     forge::net::transport::exceptions::unsupported_protocol);
}

BOOST_AUTO_TEST_SUITE_END()
