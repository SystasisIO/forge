#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <string_view>
#include <utility>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

#include "../../libraries/net/quic/details/quic_engine.hxx"
#include "libp2p_identity_fixture.hxx"

import forge.asio.runtime;
import forge.crypto.core.secret_string;
import forge.net.quic.security;

#include "../../libraries/net/quic/details/engine_client_options.hxx"
#include "../../libraries/net/quic/details/engine_server_options.hxx"

namespace {
namespace asio = boost::asio;
namespace detail = forge::net::quic::detail;
using namespace std::chrono_literals;

detail::engine_server_options one_dial_options() {
   auto options = detail::engine_server_options{};
   options.limits.max_connections = 1;
   return options;
}

void require_closed_udp_fd(asio::io_context& context, const detail::engine_endpoint& local) {
   auto socket = asio::ip::udp::socket{context};
   auto error = boost::system::error_code{};
   socket.open(asio::ip::udp::v4(), error);
   BOOST_REQUIRE(!error);
   // No SO_REUSEPORT: success proves the listener's actual socket was closed.
   socket.bind({asio::ip::make_address(local.host), local.port}, error);
   BOOST_TEST(!error);
}

struct connector_cleanup_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   detail::engine_listener source{runtime.context(), {.host = "127.0.0.1"}, one_dial_options()};
   detail::engine_connector connector{runtime.context(), source, source.local_endpoint()};
   asio::strand<asio::io_context::executor_type> caller = asio::make_strand(runtime.context());
   std::unique_ptr<detail::engine_listener> target;
   std::future<std::shared_ptr<detail::engine_connection>> dial;
   std::future<std::shared_ptr<detail::engine_connection>> accepted;
   std::future<void> stopped;
   std::mutex mutex;
   std::condition_variable changed;
   bool entered = false;
   bool released = false;
   std::atomic_size_t releases{0};
   std::atomic_size_t hook_calls{0};
   std::atomic_bool hook_on_caller{false};
   std::atomic_bool cleanup_on_caller{false};

   ~connector_cleanup_fixture() {
      // Fatal assertions unwind through here as well. Release a held worker,
      // then unconditionally join before any callback's fixture state dies.
      release_worker();
      BOOST_CHECK_NO_THROW(connector.cancel());
      BOOST_CHECK_NO_THROW(source.stop());
      if (target) {
         BOOST_CHECK_NO_THROW(target->stop());
      }
      if (dial.valid()) {
         dial.wait();
      }
      if (accepted.valid()) {
         accepted.wait();
      }
      if (stopped.valid()) {
         stopped.wait();
      }
      BOOST_CHECK_NO_THROW(asio::co_spawn(runtime.context(), source.async_stop(), asio::use_future).get());
      if (target) {
         BOOST_CHECK_NO_THROW(asio::co_spawn(runtime.context(), target->async_stop(), asio::use_future).get());
      }
   }

   void release_worker() {
      auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }

   void connect(detail::engine_endpoint remote, detail::engine_client_options options) {
      auto completion = std::make_shared<std::promise<std::shared_ptr<detail::engine_connection>>>();
      dial = completion->get_future();
      asio::post(caller, [this, completion, remote = std::move(remote), options = std::move(options)]() mutable {
         try {
            // co_spawn dispatch may nest source inside caller: Asio's
            // running_in_this_thread() tests membership anywhere in its call
            // stack, not an exclusive owner. A queued context handoff avoids
            // that invalid negative probe; native guards assert source ownership.
            asio::post(runtime.context(),
                       [this, completion, remote = std::move(remote), options = std::move(options)]() mutable {
               try {
                  asio::co_spawn(runtime.context(), connector.async_connect(std::move(remote), std::move(options)),
                                 [completion](std::exception_ptr error,
                                              std::shared_ptr<detail::engine_connection> connection) {
                     if (error) {
                        completion->set_exception(error);
                     } else {
                        completion->set_value(std::move(connection));
                     }
                  });
               } catch (...) {
                  completion->set_exception(std::current_exception());
               }
            });
         } catch (...) {
            completion->set_exception(std::current_exception());
         }
      });
   }
};

BOOST_AUTO_TEST_CASE(quic_borrowed_connector_bad_alloc_releases_capacity_on_listener_strand) {
   auto state = connector_cleanup_fixture{};
   const auto local = state.source.local_endpoint();

   // Repeat with capacity one: every exceptional owner must return the same
   // admission slot, not merely decrement active_operations during stop.
   for (auto attempt = std::size_t{0}; attempt < 4; ++attempt) {
      auto options = detail::engine_client_options{};
      options.connect_timeout = 5s;
      options.connection_lifetime = std::shared_ptr<void>{
          new int{0}, [&state](int* value) noexcept {
             state.cleanup_on_caller.store(state.caller.running_in_this_thread(), std::memory_order_release);
             delete value;
             state.releases.fetch_add(1, std::memory_order_release);
          }};
      const auto lifetime = std::weak_ptr<void>{options.connection_lifetime};
      options.test_failpoint = [&state](std::string_view name) -> bool {
         if (name != "after_resolution_completion") {
            return false;
         }
         state.hook_on_caller.store(state.caller.running_in_this_thread(), std::memory_order_release);
         state.hook_calls.fetch_add(1, std::memory_order_release);
         throw std::bad_alloc{};
      };
      state.connect(local, std::move(options));
      BOOST_REQUIRE(state.dial.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(state.dial.get()), std::bad_alloc);
      BOOST_TEST(lifetime.expired());
      BOOST_TEST(state.releases.load(std::memory_order_acquire) == attempt + 1);
   }
   BOOST_TEST(state.hook_calls.load(std::memory_order_acquire) == 4U);
   BOOST_TEST(!state.hook_on_caller.load(std::memory_order_acquire));
   BOOST_TEST(!state.cleanup_on_caller.load(std::memory_order_acquire));

   state.stopped = asio::co_spawn(state.caller, state.source.async_stop(), asio::use_future);
   BOOST_REQUIRE(state.stopped.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(state.stopped.get());
   require_closed_udp_fd(state.runtime.context(), local);
}

BOOST_AUTO_TEST_CASE(quic_borrowed_connector_cancel_and_listener_stop_join_admitted_owner) {
   auto state = connector_cleanup_fixture{};
   const auto local = state.source.local_endpoint();
   auto blackhole = asio::ip::udp::socket{state.runtime.context(), {asio::ip::udp::v4(), 0}};
   const auto remote = detail::engine_endpoint{.host = "127.0.0.1", .port = blackhole.local_endpoint().port()};
   auto options = detail::engine_client_options{};
   options.connect_timeout = 5s;
   options.security.verify_peer = false;
   options.connection_lifetime = std::shared_ptr<void>{new int{0}, [&state](int* value) noexcept {
      state.cleanup_on_caller.store(state.caller.running_in_this_thread(), std::memory_order_release);
      delete value;
   }};
   const auto lifetime = std::weak_ptr<void>{options.connection_lifetime};
   options.test_failpoint = [&state](std::string_view name) {
      if (name != "drain_after_owner_claim_before_native_write") {
         return false;
      }
      // The real connection is CID-registered and owns a native background
      // drain on its own strand. Listener stop can install its waiter while
      // this worker is held; it must not publish completion or drop admission.
      auto lock = std::unique_lock{state.mutex};
      state.entered = true;
      state.changed.notify_all();
      state.changed.wait(lock, [&] { return state.released; });
      return false;
   };
   state.connect(remote, std::move(options));
   {
      auto lock = std::unique_lock{state.mutex};
      BOOST_REQUIRE(state.changed.wait_for(lock, 2s, [&] { return state.entered; }));
   }
   BOOST_TEST(!lifetime.expired());
   state.stopped = asio::co_spawn(state.caller, state.source.async_stop(), asio::use_future);
   BOOST_CHECK(state.stopped.wait_for(20ms) != std::future_status::ready);
   BOOST_CHECK(state.dial.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(!lifetime.expired());
   state.connector.cancel();
   state.release_worker();

   BOOST_REQUIRE(state.dial.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(state.dial.get()), detail::engine_failure, [](const auto& error) {
      return error.kind() == detail::engine_error_kind::canceled;
   });
   BOOST_REQUIRE(state.stopped.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(state.stopped.get());
   BOOST_TEST(lifetime.expired());
   BOOST_TEST(!state.cleanup_on_caller.load(std::memory_order_acquire));
   require_closed_udp_fd(state.runtime.context(), local);
}

BOOST_AUTO_TEST_CASE(quic_borrowed_connector_late_bad_alloc_joins_native_connection_and_returns_capacity) {
   auto state = connector_cleanup_fixture{};
   const auto client_identity = forge::tests::p2p::make_identity_fixture("cleanup-client");
   const auto server_identity = forge::tests::p2p::make_identity_fixture("cleanup-server");
   auto server_options = detail::engine_server_options{};
   server_options.certificate_pem = server_identity.certificate_pem;
   server_options.private_key_pem = server_identity.private_key_pem;
   server_options.security.expected_sha256_fingerprint =
       forge::net::quic::certificate_sha256_fingerprint_from_pem(client_identity.certificate_pem);
   state.target = std::make_unique<detail::engine_listener>(
       state.runtime.context(), detail::engine_endpoint{.host = "127.0.0.1"}, std::move(server_options));

   for (auto attempt = 0; attempt < 2; ++attempt) {
      auto options = detail::engine_client_options{};
      options.connect_timeout = 3s;
      options.handshake_timeout = 2s;
      options.certificate_pem = client_identity.certificate_pem;
      options.private_key_pem = client_identity.private_key_pem;
      options.security.expected_sha256_fingerprint =
          forge::net::quic::certificate_sha256_fingerprint_from_pem(server_identity.certificate_pem);
      options.connection_lifetime = std::shared_ptr<void>{new int{0}, [&state](int* value) noexcept {
         delete value;
         state.releases.fetch_add(1, std::memory_order_release);
      }};
      const auto lifetime = std::weak_ptr<void>{options.connection_lifetime};
      if (attempt == 0) {
         options.test_failpoint = [&state](std::string_view name) -> bool {
            // Existing finish seam also runs after authenticated handshake,
            // CID registration and native workers, not only pre-connect errors.
            if (name == "timeout_before_pre_connection_error_finish") {
               state.hook_calls.fetch_add(1, std::memory_order_release);
               throw std::bad_alloc{};
            }
            return false;
         };
      }
      state.accepted = asio::co_spawn(state.runtime.context(), state.target->async_accept(), asio::use_future);
      state.connect(state.target->local_endpoint(), std::move(options));
      BOOST_REQUIRE(state.dial.wait_for(5s) == std::future_status::ready);
      BOOST_REQUIRE(state.accepted.wait_for(5s) == std::future_status::ready);
      auto inbound = state.accepted.get();
      BOOST_TEST(inbound->metrics().handshakes_completed == 1U);
      BOOST_REQUIRE(inbound->peer_certificate().has_value());
      if (attempt == 0) {
         BOOST_CHECK_THROW(static_cast<void>(state.dial.get()), std::bad_alloc);
         BOOST_TEST(state.hook_calls.load(std::memory_order_acquire) == 1U);
         BOOST_TEST(lifetime.expired());
      } else {
         // Capacity one must admit a genuine second native dial after failure.
         auto outbound = state.dial.get();
         BOOST_TEST(outbound->metrics().handshakes_completed == 1U);
         BOOST_TEST(!lifetime.expired());
         auto closed = asio::co_spawn(state.runtime.context(), outbound->async_close(), asio::use_future);
         BOOST_REQUIRE(closed.wait_for(2s) == std::future_status::ready);
         BOOST_CHECK_NO_THROW(closed.get());
      }
      auto closed = asio::co_spawn(state.runtime.context(), inbound->async_close(), asio::use_future);
      BOOST_REQUIRE(closed.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_NO_THROW(closed.get());
      BOOST_TEST(lifetime.expired());
      BOOST_TEST(state.releases.load(std::memory_order_acquire) == static_cast<std::size_t>(attempt + 1));
   }
   const auto local = state.source.local_endpoint();
   state.stopped = asio::co_spawn(state.runtime.context(), state.source.async_stop(), asio::use_future);
   BOOST_REQUIRE(state.stopped.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_NO_THROW(state.stopped.get());
   require_closed_udp_fd(state.runtime.context(), local);
}

} // namespace
