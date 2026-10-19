#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

import forge.swarm.api.peer;
import forge.swarm.protocol.metadata_reader;
import forge.swarm.protocol.metadata_assembler;
import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.core.call_options;
import forge.api.core.error_projection;
import forge.api.core.trusted_invocation;
import forge.api.transport.connection;
import forge.api.stream.server;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.asio.task;
import forge.asio.notification;
import forge.net.tcp.listener;
import forge.net.tcp.connector;
import forge.raw.raw;

namespace swarm_api_test {
namespace core = forge::swarm::core;
namespace wire = forge::swarm::protocol;
namespace api = forge::api::core;
using peer = forge::swarm::api::peer;
using caller = forge::api::auth::authenticated_caller;
using namespace std::chrono_literals;

wire::metadata fixture() {
   const auto hash = core::hash_file(std::array<std::uint8_t, 1>{42U}, core::block_size).root;
   auto manifest = core::descriptor{core::descriptor_version::v1, core::block_size, {}};
   for (unsigned i = 0; i < 600U; ++i) {
      manifest.files.push_back({{"file" + std::to_string(i + 1000U)}, 1U, hash, {}});
   }
   return {core::calculate_swarm_id(manifest), std::move(manifest)};
}

caller principal() {
   return {forge::api::auth::caller_source::p2p_peer, core::sha256::hash(std::string{"server-authenticated-peer"})};
}

api::trusted_invocation trusted() {
   return api::trusted_invocation_builder{}.set(principal()).build();
}

api::call_options limits() {
   return {.max_item_bytes = 65536U,
           .max_buffered_items = 1U,
           .max_buffered_bytes = 65536U,
           .deadline = std::chrono::steady_clock::now() + 5s};
}

// A test service exercises the real contract and transport. The production
// storage, admission/quota and request ownership implementation belongs to PR6.
class service final : public peer {
 public:
   wire::metadata content = fixture();
   std::atomic_uint metadata_calls{0};
   std::atomic_uint exchange_calls{0};
   std::atomic_uint active_exchanges{0};
   std::atomic_uint received{0};
   std::atomic_uint delivered_to_handler{0};
   std::atomic_uint emitted{0};
   std::optional<caller> observed;
   bool deny = false;
   bool close_output_first = false;
   bool fail_after_first = false;
   std::optional<wire::exceptions::code> handler_error;
   forge::asio::notification release_error;

   void admit(const caller& value) {
      if (!value.transport_authenticated() || deny) {
         FORGE_THROW_EXCEPTION(api::exceptions::invocation_denied, "fixture admission denied");
      }
      observed = value;
   }

   void fail_if_requested() {
      if (!handler_error)
         return;
      switch (*handler_error) {
      case wire::exceptions::code::invalid_metadata:
         FORGE_THROW_EXCEPTION(wire::exceptions::invalid_metadata, "fixture handler failure");
      case wire::exceptions::code::invalid_message:
         FORGE_THROW_EXCEPTION(wire::exceptions::invalid_message, "fixture handler failure");
      case wire::exceptions::code::invalid_encoding:
         FORGE_THROW_EXCEPTION(wire::exceptions::invalid_encoding, "fixture handler failure");
      case wire::exceptions::code::unsupported_version:
         FORGE_THROW_EXCEPTION(wire::exceptions::unsupported_version, "fixture handler failure");
      }
   }

   boost::asio::awaitable<void> metadata(wire::metadata_request request, caller value,
                                         api::stream_writer<wire::metadata_chunk> output) override {
      wire::validate(request);
      admit(value);
      metadata_calls.fetch_add(1U);
      fail_if_requested();
      if (request.swarm != content.swarm) {
         FORGE_THROW_EXCEPTION(wire::exceptions::invalid_metadata, "fixture content unavailable");
      }
      auto source = wire::metadata_reader{content};
      while (auto chunk = source.next()) {
         co_await output.async_write(std::move(*chunk));
         emitted.fetch_add(1U);
         if (fail_after_first) {
            co_await release_error.async_wait_until(0U, std::chrono::steady_clock::now() + 2s);
            FORGE_THROW_EXCEPTION(api::exceptions::invocation_denied, "fixture permission revoked");
         }
      }
      co_await output.async_close();
   }

   boost::asio::awaitable<void> exchange(wire::exchange_request request, caller value,
                                         api::duplex_stream<wire::peer_message, wire::peer_message> stream) override {
      wire::validate(request);
      admit(value);
      exchange_calls.fetch_add(1U);
      fail_if_requested();
      active_exchanges.fetch_add(1U);
      try {
         auto first = true;
         while (auto message = co_await stream.async_read()) {
            delivered_to_handler.fetch_add(1U);
            wire::validate(*message, content.manifest);
            if (first) {
               const auto* hello = std::get_if<wire::handshake>(&message->body);
               if (!hello || hello->swarm != request.swarm || hello->realm != request.realm) {
                  FORGE_THROW_EXCEPTION(wire::exceptions::invalid_message, "fixture requires bound first handshake");
               }
               first = false;
            }
            received.fetch_add(1U);
            if (!close_output_first || received.load() == 1U) {
               co_await stream.async_write(std::move(*message));
               if (close_output_first)
                  co_await stream.async_close();
            }
         }
         if (first) {
            FORGE_THROW_EXCEPTION(wire::exceptions::invalid_message, "fixture handshake missing");
         }
         co_await stream.async_close();
      } catch (...) {
         active_exchanges.fetch_sub(1U);
         throw;
      }
      active_exchanges.fetch_sub(1U);
   }
};

// Same contract ID and DTOs, deliberately without Swarm's descriptor customization.
// It is a hostile client probe, never a production compatibility path.
class unchecked_peer : public api::contract<unchecked_peer, api::surface::remote> {
 public:
   virtual ~unchecked_peer() = default;
   virtual boost::asio::awaitable<void> exchange(wire::exchange_request, caller,
                                                 api::duplex_stream<wire::peer_message, wire::peer_message>) = 0;
};

template <typename Interface = peer, typename Scenario>
void run(const std::shared_ptr<service>& implementation, Scenario exercise,
         api::trusted_invocation authority = trusted()) {
   auto runtime = forge::asio::runtime{};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto body = [&]() -> boost::asio::awaitable<void> {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto registry = api::registry{};
      registry.install<peer>(implementation);
      auto plan = api::binding().serve(registry).build();
      auto listener = forge::net::tcp::listener{
          executor, {.protocol = forge::net::transport::endpoint::protocol_kind::tcp, .host = "127.0.0.1", .port = 0U}};
      auto options = forge::api::stream::options{};
      options.max_frame_size = 131072U;
      options.max_item_size = 65536U;
      options.initial_window_items = 1U;
      options.initial_window_bytes = 65536U;
      options.max_buffered_bytes = 262144U;
      options.control_timeout = 2s;
      options.idle_timeout = 3s;
      options.disconnect_grace = 500ms;
      auto server = scheduler.submit(forge::asio::task::awaitable{
          .name = "swarm-api-test-server", .work = [&](forge::asio::task::context&) -> boost::asio::awaitable<void> {
             auto accepted = co_await listener.async_accept();
             co_await forge::api::stream::serve_stream(std::move(accepted.stream), plan, options, {}, authority);
          }});
      auto connector = forge::net::tcp::connector{executor};
      auto connection = forge::api::transport::connection{};
      auto failure = std::exception_ptr{};
      try {
         auto connected = co_await connector.async_connect(listener.local_endpoint());
         connection = forge::api::transport::connection{std::move(connected.stream), options};
         auto remote = co_await connection.get_remote_api<Interface>();
         co_await exercise(std::move(remote), *implementation);
      } catch (...) {
         failure = std::current_exception();
         connection.cancel();
         server.cancel();
      }
      try {
         co_await connection.async_close();
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
      }
      co_await listener.async_close();
      co_await connector.async_stop();
      try {
         co_await server.wait();
      } catch (...) {
         if (!failure)
            failure = std::current_exception();
      }
      co_await scheduler.shutdown();
      if (failure)
         std::rethrow_exception(failure);
   };
   forge::asio::blocking::run(runtime, body());
}

wire::peer_message hello(const service& state, std::string realm = "test") {
   return {wire::handshake{1U, state.content.swarm, std::move(realm), 0U}};
}

boost::asio::awaitable<void> read_metadata(api::handle<peer> remote, service& implementation) {
   auto call = co_await remote.async_open<&peer::metadata>(
       limits(), wire::metadata_request{implementation.content.swarm, "test"}, caller{});
   auto assembler = wire::metadata_assembler{implementation.content.swarm};
   while (auto chunk = co_await call.async_read())
      assembler.append(*chunk);
   co_await call.async_finish();
   BOOST_REQUIRE(assembler.complete());
   BOOST_CHECK(std::move(assembler).finish().manifest == implementation.content.manifest);
}

template <typename Exception, typename Call> boost::asio::awaitable<void> expect_terminal_error(Call& call) {
   auto failed = false;
   try {
      static_cast<void>(co_await call.async_read());
   } catch (const Exception&) {
      failed = true;
   }
   BOOST_CHECK(failed);
   failed = false;
   try {
      co_await call.async_finish();
   } catch (const Exception&) {
      failed = true;
   }
   BOOST_CHECK(failed);
}

template <typename Exception>
boost::asio::awaitable<void> check_handler_error(api::handle<peer> remote, service& state) {
   state.handler_error = Exception::value;
   auto metadata = co_await remote.async_open<&peer::metadata>(
       limits(), wire::metadata_request{state.content.swarm, "test"}, caller{});
   co_await expect_terminal_error<Exception>(metadata);
   auto exchange = co_await remote.async_open<&peer::exchange>(
       limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
   co_await expect_terminal_error<Exception>(exchange);
   state.handler_error.reset();
}

template <typename Exception>
void check_error_mapping(const api::method_descriptor& method, const char* name, api::status status) {
   try {
      FORGE_THROW_EXCEPTION(Exception, "fixture handler failure");
   } catch (const Exception& error) {
      const auto payload = api::project_error(method, error);
      BOOST_TEST(payload.error == name);
      BOOST_TEST(payload.message == "fixture handler failure");
      BOOST_CHECK(payload.status_code == status);
      BOOST_CHECK(!payload.retryable);
      BOOST_CHECK(payload.identity == api::exception_identity<Exception>());
      BOOST_CHECK(!payload.details_codec);
      BOOST_CHECK(!payload.details);
      const auto restored = forge::raw::unpack_exact<api::error_payload>(forge::raw::pack(payload));
      BOOST_CHECK(restored == payload);
      BOOST_CHECK_THROW(api::raise_remote_error(restored, &method), Exception);
   }
}

} // namespace swarm_api_test

FORGE_API(::swarm_api_test::unchecked_peer, FORGE_API_CONTRACT("forge.swarm.peer", 1, 0),
          FORGE_API_METHOD(exchange, request, caller))

BOOST_AUTO_TEST_SUITE(swarm_peer_api)

BOOST_AUTO_TEST_CASE(contract_uses_existing_stream_kinds_and_bounded_decoders) {
   using namespace swarm_api_test;
   const auto descriptor = peer::describe();
   BOOST_REQUIRE_EQUAL(descriptor.methods.size(), 2U);
   BOOST_CHECK(descriptor.methods[0].kind == api::method_kind::server_stream);
   BOOST_CHECK(descriptor.methods[1].kind == api::method_kind::bidirectional_stream);
   const auto valid = wire::encode_message(wire::peer_message{wire::choke{true}});
   descriptor.methods[1].input_decoder(valid, {});
   auto invalid = valid;
   invalid.push_back(0U);
   BOOST_CHECK_THROW(descriptor.methods[1].input_decoder(invalid, {}), wire::exceptions::invalid_encoding);
   BOOST_CHECK_THROW(descriptor.methods[1].output_decoder(invalid, {}), wire::exceptions::invalid_encoding);
   const auto bomb = std::vector<std::uint8_t>(65537U);
   BOOST_CHECK_THROW(descriptor.methods[1].input_decoder(bomb, {}), wire::exceptions::invalid_encoding);
}

BOOST_AUTO_TEST_CASE(metadata_is_incremental_and_wire_caller_values_are_replaced) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      const auto forged =
          caller{forge::api::auth::caller_source::tls_certificate, core::sha256::hash(std::string{"client-forged"})};
      auto call = co_await remote.async_open<&peer::metadata>(
          limits(), wire::metadata_request{state.content.swarm, "test"}, forged);
      auto assembler = wire::metadata_assembler{state.content.swarm};
      auto chunks = 0U;
      while (auto chunk = co_await call.async_read()) {
         BOOST_REQUIRE_EQUAL(chunk->offset, assembler.received_size());
         assembler.append(*chunk);
         ++chunks;
      }
      co_await call.async_finish();
      BOOST_TEST(chunks > 1U);
      BOOST_REQUIRE(state.observed);
      BOOST_CHECK(state.observed->source == principal().source);
      BOOST_CHECK(state.observed->fingerprint == principal().fingerprint);
      BOOST_CHECK(state.observed->transport_authenticated());
      BOOST_CHECK(!forged.transport_authenticated());
      BOOST_CHECK(forged.fingerprint == core::sha256::hash(std::string{"client-forged"}));
      BOOST_CHECK(std::move(assembler).finish().manifest == state.content.manifest);
   });
}

BOOST_AUTO_TEST_CASE(exchange_wire_caller_values_are_replaced) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      const auto forged =
          caller{forge::api::auth::caller_source::tls_certificate, core::sha256::hash(std::string{"client-forged"})};
      auto call = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, forged);
      co_await call.async_write(hello(state));
      const auto echoed = co_await call.async_read();
      BOOST_REQUIRE(echoed);
      BOOST_CHECK(std::holds_alternative<wire::handshake>(echoed->body));
      co_await call.async_close();
      BOOST_CHECK(!(co_await call.async_read()));
      co_await call.async_finish();
      BOOST_REQUIRE(state.observed);
      BOOST_CHECK(state.observed->source == principal().source);
      BOOST_CHECK(state.observed->fingerprint == principal().fingerprint);
      BOOST_CHECK(state.observed->transport_authenticated());
      BOOST_CHECK(!forged.transport_authenticated());
      BOOST_CHECK(forged.fingerprint == core::sha256::hash(std::string{"client-forged"}));
      BOOST_TEST(state.exchange_calls.load() == 1U);
   });
}

BOOST_AUTO_TEST_CASE(trusted_caller_does_not_bypass_denial) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   implementation->deny = true;
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto metadata = co_await remote.async_open<&peer::metadata>(
          limits(), wire::metadata_request{state.content.swarm, "test"}, principal());
      co_await expect_terminal_error<api::exceptions::invocation_denied>(metadata);
      auto exchange = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, principal());
      co_await expect_terminal_error<api::exceptions::invocation_denied>(exchange);
      BOOST_TEST(state.metadata_calls.load() == 0U);
      BOOST_TEST(state.exchange_calls.load() == 0U);
      BOOST_TEST(state.delivered_to_handler.load() == 0U);
      BOOST_TEST(state.emitted.load() == 0U);
      state.deny = false;
      co_await read_metadata(remote, state);
      BOOST_REQUIRE(state.observed);
      BOOST_CHECK(state.observed->transport_authenticated());
   });
}

BOOST_AUTO_TEST_CASE(declared_terminal_errors_preserve_status_and_typed_raw_roundtrip) {
   using namespace swarm_api_test;
   const auto descriptor = peer::describe();
   BOOST_REQUIRE_EQUAL(descriptor.methods.size(), 2U);
   for (const auto& method : descriptor.methods) {
      BOOST_REQUIRE_EQUAL(method.errors.size(), 6U);
      check_error_mapping<api::exceptions::server_supplied_unavailable>(method, "caller_unavailable",
                                                                        api::status::unauthenticated);
      check_error_mapping<api::exceptions::invocation_denied>(method, "admission_denied",
                                                              api::status::permission_denied);
      check_error_mapping<wire::exceptions::invalid_metadata>(method, "invalid_metadata",
                                                              api::status::invalid_argument);
      check_error_mapping<wire::exceptions::invalid_message>(method, "invalid_message", api::status::invalid_argument);
      check_error_mapping<wire::exceptions::invalid_encoding>(method, "invalid_encoding",
                                                              api::status::invalid_argument);
      check_error_mapping<wire::exceptions::unsupported_version>(method, "unsupported_version",
                                                                 api::status::failed_precondition);
   }
}

BOOST_AUTO_TEST_CASE(handler_protocol_errors_roundtrip_for_both_stream_methods) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      co_await check_handler_error<wire::exceptions::invalid_metadata>(remote, state);
      co_await check_handler_error<wire::exceptions::invalid_message>(remote, state);
      co_await check_handler_error<wire::exceptions::invalid_encoding>(remote, state);
      co_await check_handler_error<wire::exceptions::unsupported_version>(remote, state);
      BOOST_TEST(state.metadata_calls.load() == 4U);
      BOOST_TEST(state.exchange_calls.load() == 4U);
      BOOST_TEST(state.active_exchanges.load() == 0U);
      BOOST_TEST(state.delivered_to_handler.load() == 0U);
      BOOST_TEST(state.emitted.load() == 0U);
      co_await read_metadata(remote, state);
   });
}

BOOST_AUTO_TEST_CASE(missing_trusted_caller_fails_before_service_admission) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation,
       [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
          auto call = co_await remote.async_open<&peer::metadata>(
              limits(), wire::metadata_request{state.content.swarm, "test"}, principal());
          auto failed = false;
          try {
             co_await call.async_finish();
          } catch (const api::exceptions::server_supplied_unavailable&) {
             failed = true;
          }
          BOOST_CHECK(failed);
          BOOST_TEST(state.metadata_calls.load() == 0U);
          auto duplex = co_await remote.async_open<&peer::exchange>(
              limits(), wire::exchange_request{state.content.swarm, "test"}, principal());
          failed = false;
          try {
             co_await duplex.async_finish();
          } catch (const api::exceptions::server_supplied_unavailable&) {
             failed = true;
          }
          BOOST_CHECK(failed);
          BOOST_TEST(state.exchange_calls.load() == 0U);
       },
       {});
}

BOOST_AUTO_TEST_CASE(duplex_makes_progress_under_single_item_windows_and_client_half_close) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto call = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
      auto send = [&]() -> boost::asio::awaitable<void> {
         co_await call.async_write(hello(state));
         for (unsigned i = 0; i < 64U; ++i)
            co_await call.async_write(wire::peer_message{wire::have{{i, 0U}}});
         co_await call.async_close();
      };
      auto receive = [&]() -> boost::asio::awaitable<void> {
         unsigned count = 0;
         while (auto item = co_await call.async_read()) {
            if (count == 0U)
               BOOST_REQUIRE(std::holds_alternative<wire::handshake>(item->body));
            else
               BOOST_CHECK(std::get<wire::have>(item->body).piece == (core::piece_id{count - 1U, 0U}));
            ++count;
         }
         BOOST_TEST(count == 65U);
      };
      using namespace boost::asio::experimental::awaitable_operators;
      co_await (send() && receive());
      co_await call.async_finish();
      BOOST_TEST(state.received.load() == 65U);
      BOOST_TEST(state.active_exchanges.load() == 0U);
   });
}

BOOST_AUTO_TEST_CASE(server_half_close_keeps_client_input_open) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   implementation->close_output_first = true;
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto call = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
      co_await call.async_write(hello(state));
      BOOST_REQUIRE(co_await call.async_read());
      BOOST_CHECK(!(co_await call.async_read()));
      for (unsigned i = 0; i < 4U; ++i)
         co_await call.async_write(wire::peer_message{wire::have{{i, 0U}}});
      co_await call.async_close();
      co_await call.async_finish();
      BOOST_TEST(state.received.load() == 5U);
   });
}

BOOST_AUTO_TEST_CASE(cancel_and_local_outbound_rejection_leave_neighboring_calls_usable) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto held = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
      co_await held.async_write(hello(state));
      BOOST_REQUIRE(co_await held.async_read());
      auto neighbor = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
      co_await neighbor.async_write(hello(state));
      BOOST_REQUIRE(co_await neighbor.async_read());
      BOOST_TEST(state.active_exchanges.load() == 2U);
      held.cancel();
      auto canceled = false;
      try {
         co_await held.async_finish();
      } catch (const api::exceptions::cancelled&) {
         canceled = true;
      }
      BOOST_CHECK(canceled);
      co_await neighbor.async_write(wire::peer_message{wire::have{{0U, 0U}}});
      const auto echoed = co_await neighbor.async_read();
      BOOST_REQUIRE(echoed);
      BOOST_CHECK(std::get<wire::have>(echoed->body).piece == (core::piece_id{0U, 0U}));
      co_await neighbor.async_close();
      BOOST_CHECK(!(co_await neighbor.async_read()));
      co_await neighbor.async_finish();
      co_await read_metadata(remote, state);

      auto malformed = co_await remote.async_open<&peer::exchange>(
          limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
      co_await malformed.async_write(hello(state));
      BOOST_REQUIRE(co_await malformed.async_read());
      // Bypass the protocol encoder, but not the generated proxy's outbound
      // validator: this failure must occur locally before the item is sent.
      auto invalid = wire::block{1U, {0U, 0U}, 0U, std::vector<std::uint8_t>(core::block_size + 1U)};
      auto failed = false;
      try {
         co_await malformed.async_write(wire::peer_message{std::move(invalid)});
         co_await malformed.async_close();
         co_await malformed.async_finish();
      } catch (const std::exception&) {
         failed = true;
      }
      BOOST_CHECK(failed);
      co_await read_metadata(remote, state);
      BOOST_TEST(state.received.load() == 4U);
   });
}

BOOST_AUTO_TEST_CASE(hostile_wire_item_fails_the_session_before_service_decode) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run<unchecked_peer>(
       implementation, [](api::handle<unchecked_peer> remote, service& state) -> boost::asio::awaitable<void> {
          auto call = co_await remote.async_open<&unchecked_peer::exchange>(
              limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
          co_await call.async_write(hello(state));
          const auto first = co_await call.async_read();
          BOOST_REQUIRE(first);
          const auto invalid =
              wire::peer_message{wire::block{1U, {0U, 0U}, 0U, std::vector<std::uint8_t>(core::block_size + 1U)}};
          const auto unchecked = api::api_traits<unchecked_peer>::describe();
          BOOST_REQUIRE_EQUAL(unchecked.methods.size(), 1U);
          BOOST_REQUIRE_NO_THROW(unchecked.methods[0].input_decoder(forge::raw::pack(invalid), {}));
          auto failed = false;
          try {
             co_await call.async_write(invalid);
             co_await call.async_close();
             co_await call.async_finish();
          } catch (const std::exception&) {
             failed = true;
          }
          BOOST_CHECK(failed);
          // If only the fixture's own semantic validation rejected the block,
          // this would be two. The descriptor must reject it before typed delivery.
          BOOST_TEST(state.delivered_to_handler.load() == 1U);
          failed = false;
          try {
             auto next = co_await remote.async_open<&unchecked_peer::exchange>(
                 limits(), wire::exchange_request{state.content.swarm, "test"}, caller{});
             co_await next.async_finish();
          } catch (const std::exception&) {
             failed = true;
          }
          BOOST_CHECK(failed);
       });
}

BOOST_AUTO_TEST_CASE(deadline_wakes_blocked_read_without_closing_neighboring_calls) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto options = limits();
      options.deadline = std::chrono::steady_clock::now() + 1s;
      auto call = co_await remote.async_open<&peer::exchange>(
          options, wire::exchange_request{state.content.swarm, "test"}, caller{});
      co_await call.async_write(hello(state));
      const auto first = co_await call.async_read();
      BOOST_REQUIRE(first);
      auto expired = false;
      try {
         static_cast<void>(co_await call.async_read());
      } catch (const api::exceptions::deadline_exceeded&) {
         expired = true;
      }
      BOOST_CHECK(expired);
      expired = false;
      try {
         co_await call.async_finish();
      } catch (const api::exceptions::deadline_exceeded&) {
         expired = true;
      }
      BOOST_CHECK(expired);
      co_await read_metadata(remote, state);
   });
}

BOOST_AUTO_TEST_CASE(typed_terminal_denial_cannot_turn_partial_metadata_into_success) {
   using namespace swarm_api_test;
   auto implementation = std::make_shared<service>();
   implementation->fail_after_first = true;
   run(implementation, [](api::handle<peer> remote, service& state) -> boost::asio::awaitable<void> {
      auto call = co_await remote.async_open<&peer::metadata>(
          limits(), wire::metadata_request{state.content.swarm, "test"}, caller{});
      auto assembler = wire::metadata_assembler{state.content.swarm};
      const auto first = co_await call.async_read();
      BOOST_REQUIRE(first);
      assembler.append(*first);
      BOOST_CHECK(!assembler.complete());
      const auto received = assembler.received_size();
      state.release_error.notify();
      co_await expect_terminal_error<api::exceptions::invocation_denied>(call);
      BOOST_TEST(assembler.received_size() == received);
      BOOST_TEST(state.emitted.load() == 1U);
      BOOST_CHECK_THROW(static_cast<void>(std::move(assembler).finish()), wire::exceptions::invalid_metadata);
   });
}

BOOST_AUTO_TEST_SUITE_END()
