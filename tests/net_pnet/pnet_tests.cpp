#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.symmetric.xsalsa20;
import forge.net.pnet.network_fingerprint;
import forge.net.pnet.protector;
import forge.net.transport.connector;
import forge.net.transport.exceptions;
import forge.net.transport.stream;

namespace {

namespace pnet = forge::net::pnet;
namespace xsalsa20 = forge::crypto::symmetric::xsalsa20;
using bytes = std::vector<std::uint8_t>;

[[nodiscard]] std::array<std::uint8_t, pnet::pre_shared_key_size> fixture_key_bytes() {
   auto value = std::array<std::uint8_t, pnet::pre_shared_key_size>{};
   for (auto index = std::size_t{}; index < value.size(); ++index) {
      value[index] = static_cast<std::uint8_t>(index);
   }
   return value;
}

[[nodiscard]] pnet::protector fixture_protector() {
   return pnet::protector{pnet::pre_shared_key{fixture_key_bytes()}};
}

[[nodiscard]] bytes text_bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] bytes concatenate(std::span<const std::uint8_t> first, std::span<const std::uint8_t> second) {
   auto value = bytes{};
   value.reserve(first.size() + second.size());
   value.insert(value.end(), first.begin(), first.end());
   value.insert(value.end(), second.begin(), second.end());
   return value;
}

[[nodiscard]] bytes transform_with_nonce(const std::array<std::uint8_t, 32>& key_bytes,
                                         const std::array<std::uint8_t, 24>& nonce, bytes value) {
   auto key = xsalsa20::key{key_bytes};
   auto cipher = xsalsa20::stream{key, xsalsa20::nonce{.bytes = nonce}};
   cipher.transform(value);
   return value;
}

class scripted_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override {
      return open;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return 71;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      writes.emplace_back(value.begin(), value.end());
      co_return;
   }

   boost::asio::awaitable<bytes> async_read() override {
      BOOST_REQUIRE(!reads.empty());
      auto value = std::move(reads.front());
      reads.pop_front();
      ++read_calls;
      co_return value;
   }

   boost::asio::awaitable<void> async_close() override {
      open = false;
      ++close_calls;
      co_return;
   }

   void cancel() override {
      open = false;
      ++cancel_calls;
   }

   std::deque<bytes> reads;
   std::vector<bytes> writes;
   std::size_t read_calls = 0;
   std::size_t close_calls = 0;
   std::size_t cancel_calls = 0;
   bool open = true;
};

template <typename value_type> struct spawned_result {
   std::mutex mutex;
   forge::asio::notification completion;
   std::optional<value_type> value;
   std::exception_ptr error;
   bool done = false;
};

template <typename value_type>
[[nodiscard]] std::shared_ptr<spawned_result<value_type>>
spawn_result(boost::asio::any_io_executor executor, boost::asio::awaitable<value_type> operation) {
   auto state = std::make_shared<spawned_result<value_type>>();
   boost::asio::co_spawn(
       std::move(executor),
       [state, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
          try {
             state->value.emplace(co_await std::move(operation));
          } catch (...) {
             state->error = std::current_exception();
          }
          {
             const auto lock = std::scoped_lock{state->mutex};
             state->done = true;
          }
          state->completion.notify();
       },
       boost::asio::detached);
   return state;
}

template <> struct spawned_result<void> {
   std::mutex mutex;
   forge::asio::notification completion;
   std::exception_ptr error;
   bool done = false;
};

[[nodiscard]] std::shared_ptr<spawned_result<void>>
spawn_result(boost::asio::any_io_executor executor, boost::asio::awaitable<void> operation) {
   auto state = std::make_shared<spawned_result<void>>();
   boost::asio::co_spawn(
       std::move(executor),
       [state, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
          try {
             co_await std::move(operation);
          } catch (...) {
             state->error = std::current_exception();
          }
          {
             const auto lock = std::scoped_lock{state->mutex};
             state->done = true;
          }
          state->completion.notify();
       },
       boost::asio::detached);
   return state;
}

template <typename value_type>
boost::asio::awaitable<value_type> take_result(const std::shared_ptr<spawned_result<value_type>>& state) {
   while (true) {
      const auto observed = state->completion.epoch();
      {
         const auto lock = std::scoped_lock{state->mutex};
         if (state->done) {
            if (state->error) {
               std::rethrow_exception(state->error);
            }
            co_return std::move(*state->value);
         }
      }
      static_cast<void>(co_await state->completion.async_wait(observed));
   }
}

template <> boost::asio::awaitable<void> take_result(const std::shared_ptr<spawned_result<void>>& state) {
   while (true) {
      const auto observed = state->completion.epoch();
      {
         const auto lock = std::scoped_lock{state->mutex};
         if (state->done) {
            if (state->error) {
               std::rethrow_exception(state->error);
            }
            co_return;
         }
      }
      static_cast<void>(co_await state->completion.async_wait(observed));
   }
}

boost::asio::awaitable<void> wait_for_count(forge::asio::notification& notification,
                                             const std::atomic_size_t& count, std::size_t expected) {
   while (count.load(std::memory_order_acquire) < expected) {
      const auto observed = notification.epoch();
      if (count.load(std::memory_order_acquire) < expected) {
         static_cast<void>(co_await notification.async_wait(observed));
      }
   }
}

enum class blocked_operation : std::uint8_t {
   read,
   write,
   close,
};

struct blocking_lower_state {
   mutable std::mutex mutex;
   forge::asio::notification read_started;
   forge::asio::notification write_started;
   forge::asio::notification close_started;
   std::atomic_size_t reads_started = 0;
   std::atomic_size_t writes_started = 0;
   std::atomic_size_t closes_started = 0;
   std::atomic_size_t cancel_calls = 0;
   std::deque<bytes> queued_reads;
   std::deque<bytes> released_reads;
   std::vector<bytes> writes;
   std::vector<std::shared_ptr<boost::asio::steady_timer>> blocked_reads;
   std::vector<std::shared_ptr<boost::asio::steady_timer>> blocked_writes;
   std::vector<std::shared_ptr<boost::asio::steady_timer>> blocked_closes;
   bool block_reads = false;
   bool block_writes = false;
   bool block_closes = true;
   bool canceled = false;
   bool fail_close = false;
   bool write_failure_pending = false;

   void enqueue_read(bytes value) {
      const auto lock = std::scoped_lock{mutex};
      queued_reads.push_back(std::move(value));
   }

   [[nodiscard]] std::optional<bytes> take_queued_read() {
      const auto lock = std::scoped_lock{mutex};
      if (queued_reads.empty()) {
         return std::nullopt;
      }
      auto value = std::move(queued_reads.front());
      queued_reads.pop_front();
      return value;
   }

   void set_released_read(bytes value) {
      const auto lock = std::scoped_lock{mutex};
      released_reads.clear();
      released_reads.push_back(std::move(value));
   }

   void enqueue_released_read(bytes value) {
      const auto lock = std::scoped_lock{mutex};
      released_reads.push_back(std::move(value));
   }

   [[nodiscard]] bytes take_released_read() {
      const auto lock = std::scoped_lock{mutex};
      if (released_reads.empty()) {
         return {};
      }
      auto value = std::move(released_reads.front());
      released_reads.pop_front();
      return value;
   }

   void fail_next_write() {
      const auto lock = std::scoped_lock{mutex};
      write_failure_pending = true;
   }

   [[nodiscard]] bool take_write_failure() {
      const auto lock = std::scoped_lock{mutex};
      return std::exchange(write_failure_pending, false);
   }

   [[nodiscard]] bool blocks(blocked_operation operation) const {
      const auto lock = std::scoped_lock{mutex};
      switch (operation) {
      case blocked_operation::read:
         return block_reads;
      case blocked_operation::write:
         return block_writes;
      case blocked_operation::close:
         return block_closes;
      }
      return false;
   }

   void set_blocked(blocked_operation operation, bool value) {
      const auto lock = std::scoped_lock{mutex};
      switch (operation) {
      case blocked_operation::read:
         block_reads = value;
         break;
      case blocked_operation::write:
         block_writes = value;
         break;
      case blocked_operation::close:
         block_closes = value;
         break;
      }
   }

   void add_timer(blocked_operation operation, std::shared_ptr<boost::asio::steady_timer> timer) {
      const auto lock = std::scoped_lock{mutex};
      switch (operation) {
      case blocked_operation::read:
         blocked_reads.push_back(std::move(timer));
         break;
      case blocked_operation::write:
         blocked_writes.push_back(std::move(timer));
         break;
      case blocked_operation::close:
         blocked_closes.push_back(std::move(timer));
         break;
      }
   }

   void release(blocked_operation operation) {
      auto pending = std::vector<std::shared_ptr<boost::asio::steady_timer>>{};
      {
         const auto lock = std::scoped_lock{mutex};
         switch (operation) {
         case blocked_operation::read:
            pending.swap(blocked_reads);
            break;
         case blocked_operation::write:
            pending.swap(blocked_writes);
            break;
         case blocked_operation::close:
            pending.swap(blocked_closes);
            break;
         }
      }
      for (const auto& timer : pending) {
         timer->cancel();
      }
   }

   void request_cancel() {
      {
         const auto lock = std::scoped_lock{mutex};
         canceled = true;
      }
      cancel_calls.fetch_add(1U, std::memory_order_release);
      release(blocked_operation::read);
      release(blocked_operation::write);
   }

   [[nodiscard]] bool cancel_requested() const {
      const auto lock = std::scoped_lock{mutex};
      return canceled;
   }

   [[nodiscard]] bool close_should_fail() const {
      const auto lock = std::scoped_lock{mutex};
      return fail_close;
   }
};

class blocking_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   explicit blocking_transport_stream(std::shared_ptr<blocking_lower_state> state)
       : state_{std::move(state)}, lifetime_{std::make_shared<std::uint8_t>()} {}

   [[nodiscard]] std::weak_ptr<void> lifetime() const noexcept {
      return lifetime_;
   }

   [[nodiscard]] bool valid() const noexcept override {
      return !state_->cancel_requested();
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return 72;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      if (state_->take_write_failure()) {
         throw forge::net::transport::exceptions::closed{"blocked lower write failure"};
      }
      if (state_->blocks(blocked_operation::write)) {
         co_await wait_until_released(blocked_operation::write);
         throw_if_canceled();
      }
      const auto lock = std::scoped_lock{state_->mutex};
      state_->writes.emplace_back(value.begin(), value.end());
   }

   boost::asio::awaitable<bytes> async_read() override {
      if (auto value = state_->take_queued_read()) {
         co_return std::move(*value);
      }
      if (!state_->blocks(blocked_operation::read)) {
         co_return bytes{};
      }
      co_await wait_until_released(blocked_operation::read);
      throw_if_canceled();
      co_return state_->take_released_read();
   }

   boost::asio::awaitable<void> async_close() override {
      if (state_->blocks(blocked_operation::close)) {
         co_await wait_until_released(blocked_operation::close);
      }
      if (state_->close_should_fail()) {
         throw forge::net::transport::exceptions::closed{"blocked lower close failure"};
      }
   }

   void cancel() override {
      state_->request_cancel();
   }

 private:
   boost::asio::awaitable<void> wait_until_released(blocked_operation operation) {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto timer = std::make_shared<boost::asio::steady_timer>(executor);
      timer->expires_at((boost::asio::steady_timer::time_point::max)());
      state_->add_timer(operation, timer);
      switch (operation) {
      case blocked_operation::read:
         state_->reads_started.fetch_add(1U, std::memory_order_release);
         state_->read_started.notify();
         break;
      case blocked_operation::write:
         state_->writes_started.fetch_add(1U, std::memory_order_release);
         state_->write_started.notify();
         break;
      case blocked_operation::close:
         state_->closes_started.fetch_add(1U, std::memory_order_release);
         state_->close_started.notify();
         break;
      }

      auto error = boost::system::error_code{};
      co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      if (error && error != boost::asio::error::operation_aborted) {
         throw boost::system::system_error{error};
      }
   }

   void throw_if_canceled() const {
      if (state_->cancel_requested()) {
         throw forge::net::transport::exceptions::canceled{"blocked lower transport was canceled"};
      }
   }

   std::shared_ptr<blocking_lower_state> state_;
   std::shared_ptr<void> lifetime_;
};

[[nodiscard]] forge::net::transport::stream_connection raw_connection(std::shared_ptr<scripted_transport_stream> backing) {
   return {.local_endpoint = {},
           .remote_endpoint = {},
           .stream = forge::net::transport::detail::stream_access::make(std::move(backing))};
}

[[nodiscard]] forge::net::transport::stream_connection raw_connection(std::shared_ptr<blocking_transport_stream> backing) {
   return {.local_endpoint = {},
           .remote_endpoint = {},
           .stream = forge::net::transport::detail::stream_access::make(std::move(backing))};
}

} // namespace

BOOST_AUTO_TEST_SUITE(net_pnet)

BOOST_AUTO_TEST_CASE(key_parses_canonical_swarm_key_terminal_forms_and_fingerprints_without_secret_access) {
   static_assert(!std::is_copy_constructible_v<pnet::pre_shared_key>);
   static_assert(!std::is_copy_assignable_v<pnet::pre_shared_key>);
   static_assert(std::is_move_constructible_v<pnet::pre_shared_key>);
   static_assert(!std::is_copy_constructible_v<pnet::protector>);

   const auto swarm_key = std::string{
       "/key/swarm/psk/1.0.0/\n/base16/\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
   const auto crlf_swarm_key = std::string{
       "/key/swarm/psk/1.0.0/\r\n/base16/\r\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"};
   const auto direct = pnet::pre_shared_key{fixture_key_bytes()};
   const auto expected = std::array<std::uint8_t, 32>{
       0x7c, 0x29, 0x1e, 0xf5, 0xc4, 0x53, 0xde, 0x49, 0x1f, 0x0a, 0x6a, 0x21, 0x9e, 0xe8, 0xe3, 0x20,
       0x77, 0x67, 0x44, 0x6d, 0xa3, 0x73, 0xdf, 0xbb, 0xbb, 0x91, 0xce, 0x7d, 0x55, 0x41, 0x85, 0x83,
   };

   const auto valid = std::array<std::string, 6>{swarm_key, swarm_key + "\n", swarm_key + "\r\n", crlf_swarm_key,
                                                  crlf_swarm_key + "\n", crlf_swarm_key + "\r\n"};
   for (const auto& value : valid) {
      const auto parsed = pnet::pre_shared_key::parse_swarm_key(value);
      const auto fingerprint = parsed.fingerprint();
      BOOST_CHECK(fingerprint == direct.fingerprint());
      BOOST_CHECK_EQUAL_COLLECTIONS(fingerprint.bytes.begin(), fingerprint.bytes.end(), expected.begin(), expected.end());
   }
   BOOST_CHECK(fixture_protector().fingerprint() == direct.fingerprint());

   const auto malformed = std::array<std::string, 10>{
       "/key/swarm/psk/2.0.0/\n/base16/\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
       "/key/swarm/psk/1.0.0/\n/base64/\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
       "/key/swarm/psk/1.0.0/\n/base16/\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e",
       "/key/swarm/psk/1.0.0/\n/base16/\n000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1g",
       swarm_key + "\r",
       swarm_key + "\n\n",
       swarm_key + "\r\n\n",
       swarm_key + "extra",
       swarm_key + "\n/key/swarm/psk/1.0.0/",
       crlf_swarm_key + "\r\nextra",
   };
   for (const auto& value : malformed) {
      BOOST_CHECK_THROW(static_cast<void>(pnet::pre_shared_key::parse_swarm_key(value)),
                        pnet::exceptions::invalid_options);
   }

   const auto short_key = std::array<std::uint8_t, 31>{};
   BOOST_CHECK_THROW(pnet::pre_shared_key{short_key}, pnet::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(network_fingerprint_matches_rust_go_donor_and_survives_moves) {
   static_assert(pnet::network_fingerprint::byte_size == 16);
   static_assert(!std::is_same_v<pnet::network_fingerprint, pnet::operational_fingerprint>);
   const auto expected = pnet::network_fingerprint{.bytes = {
       0x45, 0xfc, 0x98, 0x6b, 0xbc, 0x93, 0x88, 0xa1, 0x1d, 0x93, 0x9d, 0xf2, 0x6f, 0x73, 0x0f, 0x0c}};
   auto source = pnet::pre_shared_key::parse_swarm_key(
       "/key/swarm/psk/1.0.0/\n/base16/\n6189c5cf0b87fb800c1a9feeda73c6ab5e998db48fb9e6a978575c770ceef683");
   const auto operational = source.fingerprint();
   BOOST_CHECK(source.network_fingerprint() == expected);
   BOOST_CHECK(source.fingerprint() == operational);
   auto moved = std::move(source);
   BOOST_CHECK_THROW(static_cast<void>(source.network_fingerprint()), pnet::exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(source.fingerprint()), pnet::exceptions::invalid_options);
   auto assigned = pnet::pre_shared_key{fixture_key_bytes()};
   assigned = std::move(moved);
   BOOST_CHECK_THROW(static_cast<void>(moved.network_fingerprint()), pnet::exceptions::invalid_options);
   BOOST_CHECK(assigned.network_fingerprint() == expected);
   BOOST_CHECK(assigned.fingerprint() == operational);
   auto original = pnet::protector{std::move(assigned)};
   BOOST_CHECK_THROW(static_cast<void>(assigned.network_fingerprint()), pnet::exceptions::invalid_options);
   BOOST_CHECK(original.network_fingerprint() == expected);
   auto protector = std::move(original);
   BOOST_CHECK_THROW(static_cast<void>(original.network_fingerprint()), pnet::exceptions::invalid_options);
   auto destination = fixture_protector();
   destination = std::move(protector);
   BOOST_CHECK_THROW(static_cast<void>(protector.network_fingerprint()), pnet::exceptions::invalid_options);
   BOOST_CHECK(destination.network_fingerprint() == expected);
   BOOST_CHECK(destination.fingerprint() == operational);
   BOOST_CHECK(fixture_protector().network_fingerprint() != expected);

   const auto long_key = std::array<std::uint8_t, 33>{};
   BOOST_CHECK_THROW(pnet::pre_shared_key{long_key}, pnet::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(protector_eagerly_writes_one_local_nonce_before_returning_the_protected_stream) {
   auto runtime = forge::asio::runtime{};
   auto backing = std::make_shared<scripted_transport_stream>();
   auto protector = fixture_protector();

   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));

   BOOST_REQUIRE_EQUAL(backing->writes.size(), 1U);
   BOOST_CHECK_EQUAL(backing->writes.front().size(), xsalsa20::nonce_size);

   const auto plaintext = text_bytes("pnet-eager-local-nonce");
   forge::asio::blocking::run(runtime, connection.stream.async_write(plaintext));
   BOOST_REQUIRE_EQUAL(backing->writes.size(), 2U);
   auto nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{};
   std::copy(backing->writes.front().begin(), backing->writes.front().end(), nonce.begin());
   const auto decrypted = transform_with_nonce(fixture_key_bytes(), nonce, backing->writes[1]);
   BOOST_CHECK_EQUAL_COLLECTIONS(decrypted.begin(), decrypted.end(), plaintext.begin(), plaintext.end());
}

BOOST_AUTO_TEST_CASE(protector_awaitable_owns_the_key_when_its_owner_is_destroyed_before_await) {
   auto runtime = forge::asio::runtime{};
   auto backing = std::make_shared<scripted_transport_stream>();
   auto operation = [&backing] {
      auto owner = fixture_protector();
      return owner.async_protect(raw_connection(backing));
   }();

   auto connection = forge::asio::blocking::run(runtime, std::move(operation));

   BOOST_REQUIRE_EQUAL(backing->writes.size(), 1U);
   BOOST_CHECK_EQUAL(backing->writes.front().size(), xsalsa20::nonce_size);
   BOOST_CHECK(connection.stream.valid());
   forge::asio::blocking::run(runtime, connection.stream.async_close());
}

BOOST_AUTO_TEST_CASE(protected_stream_rejects_eof_before_within_and_after_the_peer_nonce) {
   const auto nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{};
   const auto expect_closed = [](std::deque<bytes> reads) {
      auto runtime = forge::asio::runtime{};
      auto backing = std::make_shared<scripted_transport_stream>();
      backing->reads = std::move(reads);
      auto protector = fixture_protector();
      auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));

      BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime, connection.stream.async_read())),
                        pnet::exceptions::closed);
      BOOST_TEST(backing->cancel_calls == 1U);
   };

   expect_closed({bytes{}});
   expect_closed({bytes{nonce.begin(), nonce.begin() + 7}, bytes{}});
   expect_closed({bytes{nonce.begin(), nonce.end()}, bytes{}});
}

BOOST_AUTO_TEST_CASE(protector_lazily_reads_fragmented_peer_nonce_and_preserves_a_coalesced_tail) {
   auto runtime = forge::asio::runtime{};
   const auto peer_nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{
       0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b,
       0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
   };
   const auto plaintext = text_bytes("coalesced peer payload");
   const auto ciphertext = transform_with_nonce(fixture_key_bytes(), peer_nonce, plaintext);
   auto backing = std::make_shared<scripted_transport_stream>();
   backing->reads.push_back(bytes{peer_nonce.begin(), peer_nonce.begin() + 7});
   backing->reads.push_back(concatenate(std::span{peer_nonce}.subspan(7), ciphertext));
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));

   const auto received = forge::asio::blocking::run(runtime, connection.stream.async_read());
   BOOST_CHECK_EQUAL(backing->read_calls, 2U);
   BOOST_REQUIRE_EQUAL(backing->writes.size(), 1U);
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), plaintext.begin(), plaintext.end());
}

BOOST_AUTO_TEST_CASE(protected_stream_keeps_independent_xsalsa20_direction_state_across_chunks) {
   auto runtime = forge::asio::runtime{};
   const auto first = text_bytes("chunk-");
   const auto second = text_bytes("boundaries-");
   const auto third = text_bytes("remain-transparent");
   const auto plaintext = concatenate(concatenate(first, second), third);

   auto writer_backing = std::make_shared<scripted_transport_stream>();
   auto writer_protector = fixture_protector();
   auto writer = forge::asio::blocking::run(runtime, writer_protector.async_protect(raw_connection(writer_backing)));
   forge::asio::blocking::run(runtime, writer.stream.async_write(first));
   forge::asio::blocking::run(runtime, writer.stream.async_write(second));
   forge::asio::blocking::run(runtime, writer.stream.async_write(third));

   BOOST_REQUIRE_EQUAL(writer_backing->writes.size(), 4U);
   auto nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{};
   std::copy(writer_backing->writes[0].begin(), writer_backing->writes[0].end(), nonce.begin());
   const auto wire_ciphertext = concatenate(concatenate(writer_backing->writes[1], writer_backing->writes[2]),
                                            writer_backing->writes[3]);
   const auto one_shot = transform_with_nonce(fixture_key_bytes(), nonce, plaintext);
   BOOST_CHECK_EQUAL_COLLECTIONS(wire_ciphertext.begin(), wire_ciphertext.end(), one_shot.begin(), one_shot.end());

   auto reader_backing = std::make_shared<scripted_transport_stream>();
   reader_backing->reads.push_back(bytes{nonce.begin(), nonce.begin() + 5});
   reader_backing->reads.push_back(concatenate(std::span{nonce}.subspan(5),
                                               std::span<const std::uint8_t>{wire_ciphertext}.subspan(0, 9)));
   reader_backing->reads.push_back(
       bytes{wire_ciphertext.begin() + 9, wire_ciphertext.begin() + static_cast<std::ptrdiff_t>(9 + second.size())});
   reader_backing->reads.push_back(bytes{wire_ciphertext.begin() + static_cast<std::ptrdiff_t>(9 + second.size()),
                                         wire_ciphertext.end()});
   auto reader_protector = fixture_protector();
   auto reader = forge::asio::blocking::run(runtime, reader_protector.async_protect(raw_connection(reader_backing)));

   auto recovered = bytes{};
   for (auto read = std::size_t{}; read < 3; ++read) {
      const auto chunk = forge::asio::blocking::run(runtime, reader.stream.async_read());
      recovered.insert(recovered.end(), chunk.begin(), chunk.end());
   }
   BOOST_CHECK_EQUAL_COLLECTIONS(recovered.begin(), recovered.end(), plaintext.begin(), plaintext.end());
}

BOOST_AUTO_TEST_CASE(protected_stream_has_one_underlying_cancel_for_explicit_request_and_abandon) {
   auto runtime = forge::asio::runtime{};
   auto explicit_backing = std::make_shared<scripted_transport_stream>();
   auto explicit_protector = fixture_protector();
   auto explicit_connection =
       forge::asio::blocking::run(runtime, explicit_protector.async_protect(raw_connection(explicit_backing)));
   explicit_connection.stream.request_cancel();
   explicit_connection.stream.cancel();
   BOOST_CHECK_EQUAL(explicit_backing->cancel_calls, 1U);

   auto abandon_backing = std::make_shared<scripted_transport_stream>();
   {
      auto abandon_protector = fixture_protector();
      auto abandoned = forge::asio::blocking::run(runtime, abandon_protector.async_protect(raw_connection(abandon_backing)));
      static_cast<void>(abandoned);
   }
   BOOST_CHECK_EQUAL(abandon_backing->cancel_calls, 1U);
}

BOOST_AUTO_TEST_CASE(protected_stream_forwards_close_and_wrong_keys_remain_unauthenticated) {
   auto runtime = forge::asio::runtime{};
   auto close_backing = std::make_shared<scripted_transport_stream>();
   auto close_protector = fixture_protector();
   auto closed = forge::asio::blocking::run(runtime, close_protector.async_protect(raw_connection(close_backing)));
   forge::asio::blocking::run(runtime, closed.stream.async_close());
   BOOST_CHECK_EQUAL(close_backing->close_calls, 1U);

   const auto nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{
       0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b,
       0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
   };
   const auto plaintext = text_bytes("wrong keys are unauthenticated");
   const auto ciphertext = transform_with_nonce(fixture_key_bytes(), nonce, plaintext);
   auto wrong_key = fixture_key_bytes();
   wrong_key.back() ^= 0x01U;
   auto wrong_backing = std::make_shared<scripted_transport_stream>();
   wrong_backing->reads.push_back(concatenate(nonce, ciphertext));
   auto wrong_protector = pnet::protector{pnet::pre_shared_key{wrong_key}};
   auto wrong_connection = forge::asio::blocking::run(runtime, wrong_protector.async_protect(raw_connection(wrong_backing)));

   const auto received = forge::asio::blocking::run(runtime, wrong_connection.stream.async_read());
   BOOST_CHECK(received != plaintext);
}

BOOST_AUTO_TEST_CASE(protected_stream_serializes_blocked_same_direction_operations_for_the_entire_lower_await) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<blocking_lower_state>();
   auto backing = std::make_shared<blocking_transport_stream>(state);
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();
   state->set_blocked(blocked_operation::write, true);

   forge::asio::blocking::run(runtime, [&connection, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto first = spawn_result(executor, connection.stream.async_write(text_bytes("first")));
      co_await wait_for_count(state->write_started, state->writes_started, 1U);

      auto second = spawn_result(executor, connection.stream.async_write(text_bytes("second")));
      auto settle = boost::asio::steady_timer{executor};
      settle.expires_after(std::chrono::milliseconds{10});
      co_await settle.async_wait(boost::asio::use_awaitable);
      BOOST_CHECK_EQUAL(state->writes_started.load(std::memory_order_acquire), 1U);

      state->release(blocked_operation::write);
      co_await wait_for_count(state->write_started, state->writes_started, 2U);
      state->release(blocked_operation::write);
      co_await take_result(first);
      co_await take_result(second);

      state->set_blocked(blocked_operation::close, false);
      co_await connection.stream.async_close();
   }());
}

BOOST_AUTO_TEST_CASE(protected_stream_serializes_blocked_readers_and_preserves_xsalsa20_keystream_order) {
   auto runtime = forge::asio::runtime{};
   const auto peer_nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{
       0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b,
       0x6c, 0x6d, 0x6e, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
   };
   const auto first_plaintext = text_bytes("first protected read");
   const auto second_plaintext = text_bytes("second protected read");
   const auto ciphertext = transform_with_nonce(fixture_key_bytes(), peer_nonce,
                                                concatenate(first_plaintext, second_plaintext));
   auto state = std::make_shared<blocking_lower_state>();
   state->enqueue_read(bytes{peer_nonce.begin(), peer_nonce.end()});
   state->enqueue_released_read(
       bytes{ciphertext.begin(), ciphertext.begin() + static_cast<std::ptrdiff_t>(first_plaintext.size())});
   state->enqueue_released_read(
       bytes{ciphertext.begin() + static_cast<std::ptrdiff_t>(first_plaintext.size()), ciphertext.end()});
   auto backing = std::make_shared<blocking_transport_stream>(state);
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();
   state->set_blocked(blocked_operation::read, true);

   forge::asio::blocking::run(runtime, [&connection, first_plaintext, second_plaintext, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto first = spawn_result(executor, connection.stream.async_read());
      co_await wait_for_count(state->read_started, state->reads_started, 1U);

      auto second = spawn_result(executor, connection.stream.async_read());
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
      BOOST_CHECK_EQUAL(state->reads_started.load(std::memory_order_acquire), 1U);

      state->release(blocked_operation::read);
      const auto first_received = co_await take_result(first);
      BOOST_CHECK_EQUAL_COLLECTIONS(first_received.begin(), first_received.end(), first_plaintext.begin(), first_plaintext.end());

      co_await wait_for_count(state->read_started, state->reads_started, 2U);
      state->release(blocked_operation::read);
      const auto second_received = co_await take_result(second);
      BOOST_CHECK_EQUAL_COLLECTIONS(second_received.begin(), second_received.end(), second_plaintext.begin(),
                                    second_plaintext.end());

      state->set_blocked(blocked_operation::close, false);
      co_await connection.stream.async_close();
   }());
}

BOOST_AUTO_TEST_CASE(protected_stream_allows_one_blocked_read_and_one_blocked_write_to_progress_concurrently) {
   auto runtime = forge::asio::runtime{};
   const auto peer_nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{
       0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
       0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
   };
   const auto expected_read = text_bytes("read and write overlap");
   auto state = std::make_shared<blocking_lower_state>();
   state->enqueue_read(bytes{peer_nonce.begin(), peer_nonce.end()});
   state->set_released_read(transform_with_nonce(fixture_key_bytes(), peer_nonce, expected_read));
   auto backing = std::make_shared<blocking_transport_stream>(state);
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();
   state->set_blocked(blocked_operation::read, true);
   state->set_blocked(blocked_operation::write, true);

   forge::asio::blocking::run(runtime, [&connection, expected_read, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto read = spawn_result(executor, connection.stream.async_read());
      co_await wait_for_count(state->read_started, state->reads_started, 1U);

      auto write = spawn_result(executor, connection.stream.async_write(text_bytes("write while read waits")));
      co_await wait_for_count(state->write_started, state->writes_started, 1U);
      BOOST_CHECK_EQUAL(state->reads_started.load(std::memory_order_acquire), 1U);
      BOOST_CHECK_EQUAL(state->writes_started.load(std::memory_order_acquire), 1U);

      state->release(blocked_operation::read);
      state->release(blocked_operation::write);
      const auto received = co_await take_result(read);
      BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), expected_read.begin(), expected_read.end());
      co_await take_result(write);

      state->set_blocked(blocked_operation::close, false);
      co_await connection.stream.async_close();
   }());
}

BOOST_AUTO_TEST_CASE(protected_stream_close_cancels_blocked_read_and_write_before_joining_lower_close) {
   auto runtime = forge::asio::runtime{};
   const auto peer_nonce = std::array<std::uint8_t, xsalsa20::nonce_size>{
       0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b,
       0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
   };
   auto state = std::make_shared<blocking_lower_state>();
   state->enqueue_read(bytes{peer_nonce.begin(), peer_nonce.end()});
   auto backing = std::make_shared<blocking_transport_stream>(state);
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();
   state->set_blocked(blocked_operation::read, true);
   state->set_blocked(blocked_operation::write, true);

   forge::asio::blocking::run(runtime, [&connection, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto read = spawn_result(executor, connection.stream.async_read());
      co_await wait_for_count(state->read_started, state->reads_started, 1U);
      auto write = spawn_result(executor, connection.stream.async_write(text_bytes("blocked protected write")));
      co_await wait_for_count(state->write_started, state->writes_started, 1U);

      auto close = spawn_result(executor, connection.stream.async_close());
      co_await wait_for_count(state->close_started, state->closes_started, 1U);

      auto read_canceled = false;
      try {
         static_cast<void>(co_await take_result(read));
      } catch (const forge::net::transport::exceptions::canceled&) {
         read_canceled = true;
      }
      BOOST_CHECK(read_canceled);

      auto write_canceled = false;
      try {
         co_await take_result(write);
      } catch (const forge::net::transport::exceptions::canceled&) {
         write_canceled = true;
      }
      BOOST_CHECK(write_canceled);
      BOOST_CHECK_EQUAL(state->cancel_calls.load(std::memory_order_acquire), 1U);

      state->release(blocked_operation::close);
      co_await take_result(close);
   }());
}

BOOST_AUTO_TEST_CASE(protected_stream_cancel_then_close_joins_lower_teardown_and_releases_the_lower_lifetime) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<blocking_lower_state>();
   auto backing = std::make_shared<blocking_transport_stream>(state);
   const auto lifetime = backing->lifetime();
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();

   forge::asio::blocking::run(runtime, [&connection, lifetime, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      connection.stream.request_cancel();
      co_await wait_for_count(state->close_started, state->closes_started, 1U);
      auto close = spawn_result(executor, connection.stream.async_close());

      auto settle = boost::asio::steady_timer{executor};
      settle.expires_after(std::chrono::milliseconds{10});
      co_await settle.async_wait(boost::asio::use_awaitable);
      {
         const auto lock = std::scoped_lock{close->mutex};
         BOOST_CHECK(!close->done);
      }
      BOOST_CHECK(!lifetime.expired());
      BOOST_CHECK_EQUAL(state->cancel_calls.load(std::memory_order_acquire), 1U);

      state->release(blocked_operation::close);
      co_await take_result(close);
      BOOST_CHECK(lifetime.expired());
   }());
}

BOOST_AUTO_TEST_CASE(protector_cancel_interrupts_the_initial_nonce_write_exactly_once) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<blocking_lower_state>();
   state->set_blocked(blocked_operation::write, true);
   auto backing = std::make_shared<blocking_transport_stream>(state);
   auto protector = fixture_protector();
   auto stop = std::stop_source{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto operation =
          spawn_result(executor, protector.async_protect(raw_connection(backing), stop.get_token()));
      co_await wait_for_count(state->write_started, state->writes_started, 1U);
      BOOST_CHECK(stop.request_stop());

      auto canceled = false;
      try {
         static_cast<void>(co_await take_result(operation));
      } catch (const pnet::exceptions::canceled&) {
         canceled = true;
      }
      BOOST_CHECK(canceled);
      BOOST_CHECK_EQUAL(state->cancel_calls.load(std::memory_order_acquire), 1U);
      BOOST_CHECK(!stop.request_stop());
      BOOST_CHECK_EQUAL(state->cancel_calls.load(std::memory_order_acquire), 1U);
   }());
}

BOOST_AUTO_TEST_CASE(protector_rejects_a_pre_canceled_nonce_exchange_before_lower_write) {
   auto runtime = forge::asio::runtime{};
   auto backing = std::make_shared<scripted_transport_stream>();
   auto protector = fixture_protector();
   auto stop = std::stop_source{};
   BOOST_REQUIRE(stop.request_stop());

   BOOST_CHECK_THROW(
       static_cast<void>(forge::asio::blocking::run(
           runtime, protector.async_protect(raw_connection(backing), stop.get_token()))),
       pnet::exceptions::canceled);
   BOOST_CHECK_EQUAL(backing->cancel_calls, 1U);
   BOOST_CHECK(backing->writes.empty());
}

BOOST_AUTO_TEST_CASE(protected_stream_write_failure_terminalizes_and_joins_lower_close) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<blocking_lower_state>();
   auto backing = std::make_shared<blocking_transport_stream>(state);
   const auto lifetime = backing->lifetime();
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();
   state->fail_next_write();

   forge::asio::blocking::run(runtime, [&connection, lifetime, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto failed_write = spawn_result(executor, connection.stream.async_write(text_bytes("write that must not retry")));

      auto write_failed = false;
      try {
         co_await take_result(failed_write);
      } catch (const forge::net::transport::exceptions::closed&) {
         write_failed = true;
      }
      BOOST_CHECK(write_failed);
      co_await wait_for_count(state->close_started, state->closes_started, 1U);

      auto retry_rejected = false;
      try {
         co_await connection.stream.async_write(text_bytes("unsafe retry"));
      } catch (const pnet::exceptions::closed&) {
         retry_rejected = true;
      }
      BOOST_CHECK(retry_rejected);
      {
         const auto lock = std::scoped_lock{state->mutex};
         BOOST_CHECK_EQUAL(state->writes.size(), 1U);
      }
      BOOST_CHECK_EQUAL(state->cancel_calls.load(std::memory_order_acquire), 1U);

      auto close = spawn_result(executor, connection.stream.async_close());
      state->release(blocked_operation::close);
      co_await take_result(close);
      BOOST_CHECK(lifetime.expired());
   }());
}

BOOST_AUTO_TEST_CASE(protected_stream_preserves_a_single_close_failure_for_all_terminal_waiters) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<blocking_lower_state>();
   state->fail_close = true;
   auto backing = std::make_shared<blocking_transport_stream>(state);
   const auto lifetime = backing->lifetime();
   auto protector = fixture_protector();
   auto connection = forge::asio::blocking::run(runtime, protector.async_protect(raw_connection(backing)));
   backing.reset();

   forge::asio::blocking::run(runtime, [&connection, lifetime, state]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto first = spawn_result(executor, connection.stream.async_close());
      co_await wait_for_count(state->close_started, state->closes_started, 1U);
      state->release(blocked_operation::close);

      auto first_failed = false;
      try {
         co_await take_result(first);
      } catch (const forge::net::transport::exceptions::closed&) {
         first_failed = true;
      }
      BOOST_CHECK(first_failed);

      auto second_failed = false;
      try {
         co_await connection.stream.async_close();
      } catch (const forge::net::transport::exceptions::closed&) {
         second_failed = true;
      }
      BOOST_CHECK(second_failed);
      BOOST_CHECK_EQUAL(state->closes_started.load(std::memory_order_acquire), 1U);
      BOOST_CHECK(lifetime.expired());
   }());
}

BOOST_AUTO_TEST_SUITE_END()
