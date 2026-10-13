#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

import forge.asio.runtime;
import forge.exceptions;
import forge.net.quic.connection;
import forge.net.quic.connector;
import forge.net.quic.exceptions;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.quic.stream;
import forge.net.quic.transport;
import forge.net.transport.buffer;
import forge.net.transport.exceptions;
import forge.net.transport.stream;

namespace {
namespace quic = forge::net::quic;
namespace transport = forge::net::transport;
using namespace std::chrono_literals;

template<class T> T joined(std::future<T>& result) {
   BOOST_REQUIRE(result.wait_for(5s) == std::future_status::ready);
   return result.get();
}

struct authenticated_pair {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 2}};
   std::unique_ptr<quic::listener> listener;
   quic::connection client;
   quic::connection server;
   bool finished = false;

   authenticated_pair() {
      const auto client_identity = forge::tests::p2p::make_identity_fixture("eof-client");
      const auto server_identity = forge::tests::p2p::make_identity_fixture("eof-server");
      auto accepting = quic::server_options{};
      accepting.certificate_pem = server_identity.certificate_pem;
      accepting.private_key_pem = server_identity.private_key_pem;
      accepting.security.verify_peer = true;
      accepting.security.expected_sha256_fingerprint =
          quic::certificate_sha256_fingerprint_from_pem(client_identity.certificate_pem);
      listener = std::make_unique<quic::listener>(runtime, quic::endpoint{.host = "127.0.0.1"}, accepting);
      auto accept = boost::asio::co_spawn(runtime.context(), listener->async_accept(), boost::asio::use_future);
      auto connecting = quic::client_options{};
      connecting.connect_timeout = 3s;
      connecting.handshake_timeout = 2s;
      connecting.certificate_pem = client_identity.certificate_pem;
      connecting.private_key_pem = client_identity.private_key_pem;
      connecting.security.expected_sha256_fingerprint =
          quic::certificate_sha256_fingerprint_from_pem(server_identity.certificate_pem);
      auto connector = quic::connector{runtime};
      client = run(connector.async_connect(listener->local_endpoint(), connecting));
      server = joined(accept);
      BOOST_REQUIRE(client.peer_certificate());
      BOOST_REQUIRE(server.peer_certificate());
   }

   void finish() {
      auto failure = std::exception_ptr{};
      for (auto* connection : {&client, &server}) {
         connection->request_cancel();
         try { run(connection->async_close()); }
         catch (const forge::exceptions::base& error) {
            if (!quic::exceptions::is(error, quic::exceptions::code::canceled) &&
                !quic::exceptions::is(error, quic::exceptions::code::connection_closed) && !failure) {
               failure = std::current_exception();
            }
         }
         catch (...) { if (!failure) { failure = std::current_exception(); } }
      }
      try { if (listener) { run(listener->async_stop()); } }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
      finished = !failure;
      if (failure) { std::rethrow_exception(failure); }
   }

   ~authenticated_pair() noexcept {
      if (finished) { return; }
      // Failure cleanup must not replace the test's original exception.
      for (auto* connection : {&client, &server}) {
         connection->request_cancel();
         try {
            auto close = boost::asio::co_spawn(runtime.context(), connection->async_close(), boost::asio::use_future);
            if (close.wait_for(5s) == std::future_status::ready) { close.get(); }
         } catch (...) {}
      }
      if (listener) {
         try {
            listener->stop();
            auto stop = boost::asio::co_spawn(runtime.context(), listener->async_stop(), boost::asio::use_future);
            if (stop.wait_for(5s) == std::future_status::ready) { stop.get(); }
         } catch (...) {}
      }
   }

   template<class T> T run(boost::asio::awaitable<T> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return joined(result);
   }

   std::pair<quic::stream, quic::stream> streams() {
      auto incoming = boost::asio::co_spawn(runtime.context(), server.async_accept_stream(), boost::asio::use_future);
      auto sent = run(client.async_open_stream());
      const auto marker = std::array<std::uint8_t, 1>{0x41};
      run(sent.async_write(marker));
      auto received = joined(incoming);
      BOOST_TEST(run(received.async_read()) == std::vector<std::uint8_t>{0x41}, boost::test_tools::per_element());
      return {std::move(sent), std::move(received)};
   }
};

bool native_eof(const quic::exceptions::stream_closed& error) {
   return dynamic_cast<const quic::exceptions::end_of_stream*>(&error) && error.code().value() == 14 &&
       quic::exceptions::code_of(error) == quic::exceptions::code::stream_closed;
}

bool adapted_eof(const transport::exceptions::closed& error) {
   return dynamic_cast<const transport::exceptions::end_of_stream*>(&error) && error.code().value() == 2 &&
       transport::exceptions::code_of(error) == transport::exceptions::code::closed;
}

void join_reset(authenticated_pair& fixture, quic::stream& stream) {
   stream.request_cancel();
   try { fixture.run(stream.async_close()); }
   catch (const forge::exceptions::base& error) {
      if (quic::exceptions::code_of(error) != quic::exceptions::code::canceled) { throw; }
   }
}
} // namespace

BOOST_AUTO_TEST_CASE(quic_remote_fin_drains_bytes_and_preserves_native_and_adapter_closed_catches) {
   for (const auto adapter : {false, true}) {
      BOOST_TEST_CONTEXT("adapter=" << adapter) {
         auto fixture = authenticated_pair{};
         auto [sent, received] = fixture.streams();
         auto adapted = adapter ? quic::as_transport_stream(std::move(sent)) : transport::stream{};
         const auto read = [&]() { return adapter ? adapted.async_read() : sent.async_read(); };
         const auto payload = std::vector<std::uint8_t>(32769, 0x52);
         fixture.run(received.async_write(payload));
         fixture.run(received.async_close());
         auto drained = std::vector<std::uint8_t>{};
         while (drained.size() < payload.size()) {
            const auto next = fixture.run(read());
            BOOST_REQUIRE(!next.empty());
            drained.insert(drained.end(), next.begin(), next.end());
         }
         BOOST_TEST(drained == payload, boost::test_tools::per_element());
         if (adapter) {
            BOOST_CHECK_EXCEPTION(fixture.run(read()), transport::exceptions::closed, adapted_eof);
            BOOST_CHECK_EXCEPTION(fixture.run(adapted.async_read_chunk()), transport::exceptions::closed, adapted_eof);
            fixture.run(adapted.async_close());
         } else {
            BOOST_CHECK_EXCEPTION(fixture.run(read()), quic::exceptions::stream_closed, native_eof);
            fixture.run(sent.async_close());
         }
         fixture.finish();
      }
   }
}

BOOST_AUTO_TEST_CASE(quic_reset_or_connection_loss_is_never_native_or_adapter_eof) {
   for (const auto adapter : {false, true}) {
      for (const auto reset : {false, true}) {
         BOOST_TEST_CONTEXT("adapter=" << adapter << " reset=" << reset) {
            auto fixture = authenticated_pair{};
            auto [sent, received] = fixture.streams();
            auto adapted = adapter ? quic::as_transport_stream(std::move(sent)) : transport::stream{};
            auto pending = boost::asio::co_spawn(fixture.runtime.context(),
                adapter ? adapted.async_read() : sent.async_read(), boost::asio::use_future);
            if (reset) {
               join_reset(fixture, received);
            } else {
               fixture.run(fixture.server.async_close());
            }
            BOOST_CHECK_EXCEPTION(joined(pending), forge::exceptions::base, ([adapter, reset](const auto& error) {
               if (dynamic_cast<const quic::exceptions::end_of_stream*>(&error) ||
                   dynamic_cast<const transport::exceptions::end_of_stream*>(&error)) { return false; }
               if (reset) { return quic::exceptions::code_of(error) == quic::exceptions::code::stream_reset; }
               return adapter ? transport::exceptions::code_of(error) == transport::exceptions::code::closed
                              : quic::exceptions::code_of(error) == quic::exceptions::code::connection_closed;
            }));
            fixture.finish();
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(quic_invalid_stream_closed_code_does_not_grant_eof) {
   auto fixture = authenticated_pair{};
   auto invalid = quic::stream{};
   BOOST_CHECK_EXCEPTION(fixture.run(invalid.async_read()), quic::exceptions::stream_closed, [](const auto& error) {
      return !dynamic_cast<const quic::exceptions::end_of_stream*>(&error) && error.code().value() == 14;
   });
   auto adapted = quic::as_transport_stream(std::move(invalid));
   BOOST_CHECK_EXCEPTION(fixture.run(adapted.async_read()), transport::exceptions::closed, [](const auto& error) {
      return !dynamic_cast<const transport::exceptions::end_of_stream*>(&error) && error.code().value() == 2;
   });
   fixture.finish();
}

BOOST_AUTO_TEST_CASE(quic_received_fin_never_masks_later_reset_or_connection_close) {
   for (const auto adapter : {false, true}) {
      for (const auto reset : {false, true}) {
         BOOST_TEST_CONTEXT("adapter=" << adapter << " reset=" << reset) {
            auto fixture = authenticated_pair{};
            auto [sent, received] = fixture.streams();
            auto adapted = adapter ? quic::as_transport_stream(std::move(sent)) : transport::stream{};
            const auto read = [&]() { return adapter ? adapted.async_read() : sent.async_read(); };
            fixture.run(received.async_close());
            if (adapter) { BOOST_CHECK_EXCEPTION(fixture.run(read()), transport::exceptions::closed, adapted_eof); }
            else { BOOST_CHECK_EXCEPTION(fixture.run(read()), quic::exceptions::stream_closed, native_eof); }
            if (!reset) { fixture.run(fixture.client.async_close()); }
            else if (!adapter) { join_reset(fixture, sent); }
            else {
               adapted.request_cancel();
               try { fixture.run(adapted.async_close()); }
               catch (const forge::exceptions::base& error) {
                  if (transport::exceptions::code_of(error) != transport::exceptions::code::canceled) { throw; }
               }
            }
            BOOST_CHECK_EXCEPTION(fixture.run(read()), forge::exceptions::base, ([adapter, reset](const auto& error) {
               if (dynamic_cast<const quic::exceptions::end_of_stream*>(&error) ||
                   dynamic_cast<const transport::exceptions::end_of_stream*>(&error)) { return false; }
               if (reset) { return quic::exceptions::code_of(error) == quic::exceptions::code::stream_reset; }
               return adapter ? transport::exceptions::code_of(error) == transport::exceptions::code::closed
                              : quic::exceptions::code_of(error) == quic::exceptions::code::connection_closed;
            }));
            fixture.finish();
         }
      }
   }
}
