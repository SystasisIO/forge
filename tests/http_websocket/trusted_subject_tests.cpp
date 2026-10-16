#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

import forge.api.auth.authenticated_caller;
import forge.api.auth.authenticated_subject;
import forge.api.core.binding;
import forge.api.core.invocation_context;
import forge.api.core.registry;
import forge.api.core.stream_reader;
import forge.api.core.stream_writer;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.error_renderer;
import forge.api.websocket.binding;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.auth.oauth2.token_verifier;
import forge.codec.json;
import forge.crypto.digest.sha256;
import forge.net.http.body;
import forge.net.http.base_url;
import forge.net.http.middleware;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.transport.frame;
import forge.net.websocket.client;
import forge.net.websocket.connection;
import forge.raw.raw;

namespace f3_test {

namespace core = forge::api::core;
namespace http = forge::net::http;
using subject = forge::api::auth::authenticated_subject;

// Evidence double only: these tests do not qualify a real issuer/backend.
class verifier final : public forge::auth::oauth2::token_verifier {
 public:
   static forge::auth::oauth2::verified_token issue(std::string name,
                                                   std::chrono::milliseconds validity = std::chrono::seconds{30}) {
      const auto now = std::chrono::system_clock::now();
      return make_verified_token({
         .issuer = "https://issuer.example.test",
         .subject = std::move(name),
         .audiences = {"resource"},
         .scopes = {"read"},
         .expires_at = now + std::chrono::minutes{5},
         .verified_at = now - std::chrono::seconds{1},
         .valid_until = now + validity,
         .policy_version = "v1",
      });
   }

   boost::asio::awaitable<forge::auth::oauth2::verified_token>
   async_verify(forge::crypto::core::secret_string, forge::auth::oauth2::verification_policy) override {
      co_return issue("Alice");
   }
   void request_stop() noexcept override {}
   boost::asio::awaitable<void> async_drain() override { co_return; }
};

core::invocation_context admitted(std::string name) {
   return {.trusted = core::trusted_invocation_builder{}.set(subject{verifier::issue(std::move(name))}).build()};
}

struct input {
   subject user;
   std::string claimed;
};

struct output {
   std::string user;
};

class api : public core::contract<api, core::surface::local | core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<output> who(input request) = 0;
   virtual boost::asio::awaitable<void> watch(input request, core::stream_writer<output> writer) = 0;
};

BOOST_DESCRIBE_STRUCT(input, (), (user, claimed))
BOOST_DESCRIBE_STRUCT(output, (), (user))

} // namespace f3_test

FORGE_API(::f3_test::api, FORGE_API_CONTRACT("test.f3.subject", 1, 0),
          FORGE_API_METHOD(who, request), FORGE_API_METHOD(watch, request))

namespace f3_test {

class service final : public api {
 public:
   std::atomic_uint calls{0};
   std::promise<void> handler_started;
   std::atomic_bool handler_exited{false};

   boost::asio::awaitable<output> who(input request) override {
      calls.fetch_add(1);
      BOOST_REQUIRE(request.user.authenticated());
      BOOST_REQUIRE(request.user.token() != nullptr);
      if (request.claimed == "hold") {
         co_await hold_until_cancelled();
      }
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      timer.expires_after(std::chrono::milliseconds{5});
      co_await timer.async_wait(boost::asio::use_awaitable);
      co_return output{request.user.token()->subject()};
   }

   boost::asio::awaitable<void> watch(input request, core::stream_writer<output> writer) override {
      calls.fetch_add(1);
      BOOST_REQUIRE(request.user.authenticated());
      if (request.claimed == "flood") {
         handler_started.set_value();
         auto error = std::exception_ptr{};
         try {
            for (auto index = 0U; index < 64U; ++index) {
               co_await writer.async_write({std::string(512U * 1024U, 'x')});
            }
         } catch (...) {
            error = std::current_exception();
         }
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
         auto cleanup = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         cleanup.expires_after(std::chrono::milliseconds{50});
         co_await cleanup.async_wait(boost::asio::use_awaitable);
         handler_exited.store(true);
         if (error) {
            std::rethrow_exception(error);
         }
         co_return;
      }
      co_await writer.async_write({request.user.token()->subject()});
      if (request.claimed == "hold") {
         co_await hold_until_cancelled();
      }
   }

 private:
   boost::asio::awaitable<void> hold_until_cancelled() {
      handler_started.set_value();
      auto error = std::exception_ptr{};
      try {
         co_await wait_for_cancel();
      } catch (...) {
         error = std::current_exception();
      }
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      auto cleanup = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      cleanup.expires_after(std::chrono::milliseconds{50});
      co_await cleanup.async_wait(boost::asio::use_awaitable);
      handler_exited.store(true);
      if (error) {
         std::rethrow_exception(error);
      }
   }

   static boost::asio::awaitable<void> wait_for_cancel() {
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      timer.expires_after(std::chrono::seconds{2});
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
};

boost::asio::awaitable<int> wait_operation(std::shared_ptr<std::atomic_bool> exited,
                                          std::chrono::milliseconds duration = std::chrono::seconds{5}) {
   struct completion {
      std::shared_ptr<std::atomic_bool> exited;
      ~completion() { exited->store(true); }
   } done{std::move(exited)};
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(duration);
   co_await timer.async_wait(boost::asio::use_awaitable);
   co_return 7;
}

http::request request(std::string name) {
   auto value = http::request{http::method::post, "/who/" + name, 11};
   value.set(http::field::content_type, "application/json");
   value.body() = R"({"user":{},"claimed":"spoofed-Bob"})";
   value.prepare_payload();
   return value;
}

class buffered_body final : public http::body_reader::source {
 public:
   explicit buffered_body(std::string text) : _text(std::move(text)) {}

   boost::asio::awaitable<std::optional<http::body_chunk>> async_read() override {
      if (_text.empty()) {
         co_return std::nullopt;
      }
      auto chunk = http::body_chunk{std::vector<std::byte>(_text.size())};
      std::memcpy(chunk.bytes.data(), _text.data(), _text.size());
      _read += _text.size();
      _text.clear();
      co_return chunk;
   }

   std::uint64_t bytes_read() const noexcept override {
      return _read;
   }

 private:
   std::string _text;
   std::uint64_t _read = 0;
};

boost::asio::awaitable<http::response> handle(http::router& router, http::request value) {
   auto context = http::make_route_context(value);
   if (router.can_handle_stream(context)) {
      auto source = std::make_shared<buffered_body>(std::exchange(value.body(), {}));
      auto stream = http::stream_request{context, http::body_reader{std::move(source)}};
      auto output = co_await router.handle_stream(stream);
      co_return std::move(output.head);
   }
   co_return co_await router.handle(context);
}

boost::beast::http::request<boost::beast::http::string_body> upgrade_request(std::string target) {
   auto value = boost::beast::http::request<boost::beast::http::string_body>{
      boost::beast::http::verb::get, std::move(target), 11};
   value.set(boost::beast::http::field::host, "127.0.0.1");
   value.set(boost::beast::http::field::connection, "Upgrade");
   value.set(boost::beast::http::field::upgrade, "websocket");
   value.set(boost::beast::http::field::sec_websocket_version, "13");
   value.set(boost::beast::http::field::sec_websocket_key, "dGhlIHNhbXBsZSBub25jZQ==");
   return value;
}

template <typename Ready>
boost::asio::awaitable<bool> wait_for_connection_metrics(forge::net::websocket::connection::ptr connection,
                                                        Ready ready) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   for (;;) {
      const auto metrics = connection->metrics();
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline || metrics.failed_writes != 0U || metrics.close_count != 0U) {
         co_return false;
      }
      if (ready(metrics, now)) {
         co_return true;
      }
      timer.expires_at(std::min(deadline, now + std::chrono::milliseconds{5}));
      co_await timer.async_wait(boost::asio::use_awaitable);
   }
}

struct write_backpressure_observation {
   forge::net::websocket::connection_metrics before;
   forge::net::websocket::connection_metrics stalled;
};

boost::asio::awaitable<std::optional<write_backpressure_observation>>
wait_for_write_backpressure(forge::net::websocket::connection::ptr connection,
                            std::uint64_t minimum_received_messages = 0) {
   auto before = std::optional<forge::net::websocket::connection_metrics>{};
   auto stable_since = std::chrono::steady_clock::time_point{};
   auto observation = std::optional<write_backpressure_observation>{};
   const auto ready = co_await wait_for_connection_metrics(std::move(connection),
      [&](const forge::net::websocket::connection_metrics& metrics, std::chrono::steady_clock::time_point now) {
         if (metrics.queued_writes == 0U || metrics.received_messages < minimum_received_messages) {
            before.reset();
            return false;
         }
         // Restart the original observation window whenever native writes make progress.
         if (!before || metrics.sent_messages != before->sent_messages) {
            before = metrics;
            stable_since = now;
         }
         if (now - stable_since < std::chrono::milliseconds{100}) {
            return false;
         }
         observation = write_backpressure_observation{*before, metrics};
         return true;
      });
   if (!ready) {
      co_return std::nullopt;
   }
   co_return observation;
}

void check_websocket_backpressure(bool revoke) {
   constexpr auto window_bytes = 8U * 1024U * 1024U;
   constexpr auto frame_bytes = 2U * 1024U * 1024U;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   auto started = implementation->handler_started.get_future();
   registry.install<api>(implementation);
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build())
      .max_frame_size(frame_bytes).initial_window(16, window_bytes).build();
   const auto item = forge::raw::pack(output{std::string(512U * 1024U, 'x')});
   BOOST_REQUIRE(item.size() <= binding.options().max_item_size);
   BOOST_REQUIRE(forge::raw::pack(core::frame{
      .kind = core::frame_kind::stream_item, .id = {.value = 1}, .api = api::ref(),
      .method = "watch", .codec = {.value = "forge.raw"}, .payload = item,
   }).size() <= frame_bytes);
   auto revoked = std::make_shared<std::atomic_bool>(false);
   auto socket_ready = std::make_shared<std::promise<forge::net::websocket::connection::ptr>>();
   auto accepted = socket_ready->get_future();
   auto session_done = std::make_shared<std::promise<void>>();
   auto completed = session_done->get_future();
   auto guard = binding.guarded(
      [revoked](const http::route_context&) -> boost::asio::awaitable<core::invocation_context> {
         auto context = admitted("Alice");
         context.lifetime.recheck_interval = std::chrono::milliseconds{10};
         context.lifetime.recheck_timeout = std::chrono::milliseconds{50};
         context.lifetime.recheck = [revoked](core::trusted_invocation) -> boost::asio::awaitable<bool> {
            co_return !revoked->load();
         };
         co_return context;
      });
   auto router = http::router{};
   router.websocket_guarded("/backpressure",
      [guard, socket_ready, session_done](http::route_context& route)
         -> boost::asio::awaitable<http::websocket_upgrade_result> {
         auto result = co_await guard(route);
         result.session = [session = std::move(result.session), socket_ready, session_done]
            (forge::net::websocket::connection::ptr socket) -> boost::asio::awaitable<void> {
            socket_ready->set_value(socket);
            auto error = std::exception_ptr{};
            try {
               co_await session(std::move(socket));
            } catch (...) {
               error = std::current_exception();
            }
            session_done->set_value();
            if (error) {
               std::rethrow_exception(error);
            }
         };
         co_return result;
      });
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto io = boost::asio::io_context{};
   auto peer = boost::beast::websocket::stream<boost::beast::tcp_stream>{io};
   auto& socket = boost::beast::get_lowest_layer(peer);
   socket.socket().open(boost::asio::ip::tcp::v4());
   socket.socket().set_option(boost::asio::socket_base::receive_buffer_size{1024});
   socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
   peer.handshake("127.0.0.1", "/backpressure");
   peer.binary(true);
   auto send_frame = [&peer](const core::frame& value) {
      const auto payload = forge::raw::pack(value);
      const auto wire = forge::net::transport::encode_frame(payload);
      peer.write(boost::asio::buffer(wire));
   };
   auto hello = core::session_hello{};
   hello.capabilities = forge::api::websocket::binding_capabilities();
   hello.limits.max_frame_bytes = frame_bytes;
   hello.limits.max_item_bytes = binding.options().max_item_size;
   hello.limits.initial_window_bytes = window_bytes;
   send_frame({.kind = core::frame_kind::session_hello, .codec = {.value = "forge.raw"},
               .payload = forge::raw::pack(hello)});
   send_frame({.kind = core::frame_kind::request, .id = {.value = 1}, .api = api::ref(),
               .method = "watch", .codec = {.value = "forge.raw"},
               .payload = forge::raw::pack(input{{}, "flood"})});
   // Hello negotiates bounds; the receiver must separately grant this call its output credit.
   send_frame({.kind = core::frame_kind::stream_window, .id = {.value = 1}, .api = api::ref(),
               .method = "watch", .codec = {.value = "forge.raw"},
               .payload = forge::raw::pack(core::stream_window{
                  .direction = core::stream_direction::output, .max_items = 16, .max_bytes = window_bytes,
               })});
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   auto server_socket = accepted.get();
   BOOST_REQUIRE(started.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   // No read loop: the peer remains connected and cannot relieve native write backpressure.
   auto observation = forge::asio::blocking::run(runtime, wait_for_write_backpressure(server_socket, 3U));
   const auto latest = server_socket->metrics();
   BOOST_REQUIRE_MESSAGE(observation.has_value(),
      "WebSocket did not reach pending-write backpressure: queued=" << latest.queued_writes
      << ", sent=" << latest.sent_messages << ", received=" << latest.received_messages
      << ", failed=" << latest.failed_writes << ", closed=" << latest.close_count);
   const auto before = observation->before;
   const auto stalled = observation->stalled;
   BOOST_REQUIRE(before.queued_writes > 0U);
   BOOST_REQUIRE(stalled.queued_writes > 0U);
   BOOST_REQUIRE(stalled.sent_messages == before.sent_messages);
   BOOST_REQUIRE(!implementation->handler_exited.load());

   auto stopped = std::future<void>{};
   if (revoke) {
      revoked->store(true);
   } else {
      stopped = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   }
   BOOST_REQUIRE(completed.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   completed.get();
   BOOST_TEST(implementation->handler_exited.load());
   const auto drained = server_socket->metrics();
   BOOST_TEST(drained.queued_writes == 0U);
   BOOST_TEST(drained.failed_writes > 0U);
   BOOST_TEST(implementation->calls.load() == 1U);
   if (revoke) {
      stopped = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   }
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   stopped.get();
}

BOOST_AUTO_TEST_SUITE(f3_trusted_subject_suite)

BOOST_AUTO_TEST_CASE(subject_has_no_wire_claims_and_missing_context_resets_spoof) {
   auto value = subject{verifier::issue("canary-verified-user")};
   BOOST_TEST(!value.authenticated());
   BOOST_TEST(forge::raw::pack(value).empty());
   const auto encoded = forge::codec::json::write(value);
   BOOST_REQUIRE(encoded.ok());
   BOOST_TEST(encoded.text == "{}");
   auto decoded = forge::raw::unpack_exact<subject>(forge::raw::pack(value));
   BOOST_TEST(decoded.token() == nullptr);
   core::reset_server_supplied(value);
   BOOST_CHECK_THROW(core::apply_server_supplied(value, {}), core::exceptions::server_supplied_unavailable);
   BOOST_TEST(value.token() == nullptr);
   core::apply_server_supplied(value, admitted("Alice").trusted);
   BOOST_TEST(value.authenticated());
   BOOST_TEST(value.token()->subject() == "Alice");
}

BOOST_AUTO_TEST_CASE(protected_merge_rejects_same_type_and_preserves_builder_on_conflict) {
   auto context = admitted("Alice");
   auto builder = core::trusted_invocation_builder{};
   builder.merge(context.trusted);
   BOOST_CHECK_THROW(builder.merge(admitted("Bob").trusted), core::exceptions::protocol_error);
   BOOST_TEST(std::move(builder).build().find<subject>()->token()->subject() == "Alice");
   BOOST_TEST(context.trusted.find<subject>()->token()->subject() == "Alice");
}

BOOST_AUTO_TEST_CASE(raw_unary_and_stream_open_never_accept_payload_subject) {
   auto runtime = forge::asio::runtime{};
   auto implementation = std::make_shared<service>();
   auto registry = core::registry{};
   registry.install<api>(implementation);
   auto value = core::frame{
      .kind = core::frame_kind::request, .id = {.value = 1}, .api = api::ref(),
      .method = "who", .codec = {.value = "forge.raw"},
      .payload = forge::raw::pack(input{subject{verifier::issue("spoofed-Bob")}, "spoofed-Bob"}),
   };
   auto denied = forge::asio::blocking::run(runtime, registry.dispatch_contextual(value, {}));
   BOOST_CHECK(denied.kind == core::frame_kind::error);
   BOOST_TEST(implementation->calls.load() == 0U);
   auto accepted = forge::asio::blocking::run(runtime, registry.dispatch_contextual(value, admitted("Alice").trusted));
   BOOST_TEST(forge::raw::unpack_exact<output>(accepted.payload).user == "Alice");
   value.method = "watch";
   auto pair = core::detail::make_local_stream_pair(runtime.context().get_executor(), 1024, 4, 4096);
   auto stream_denied = forge::asio::blocking::run(
      runtime, registry.dispatch_stream_contextual(value, {}, pair.writer, {}));
   BOOST_CHECK(stream_denied.kind == core::frame_kind::error);
   BOOST_TEST(implementation->calls.load() == 1U);
}

BOOST_AUTO_TEST_CASE(http_provider_runs_after_route_selection_and_isolates_concurrent_users) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   registry.install<api>(std::make_shared<service>());
   auto provider_calls = std::make_shared<std::atomic_uint>(0);
   auto router = http::router{};
   router.mount(forge::api::http::binding().use(core::binding().serve(registry).build())
      .invocation([provider_calls](const http::route_context& route, const core::method_descriptor& method)
         -> boost::asio::awaitable<core::invocation_context> {
         BOOST_TEST(method.name == "who");
         provider_calls->fetch_add(1);
         co_return admitted(route.route_params.at("name"));
      }).post<&api::who, input, output>("/who/:name").build());
   auto alice = boost::asio::co_spawn(runtime.context(), handle(router, request("Alice")), boost::asio::use_future);
   auto bob = boost::asio::co_spawn(runtime.context(), handle(router, request("Bob")), boost::asio::use_future);
   auto alice_response = alice.get();
   auto bob_response = bob.get();
   BOOST_TEST(provider_calls->load() == 2U);
   BOOST_REQUIRE(alice_response.result_int() == 200U);
   BOOST_REQUIRE(bob_response.result_int() == 200U);
   auto a = forge::codec::json::read<output>(alice_response.body());
   auto b = forge::codec::json::read<output>(bob_response.body());
   BOOST_REQUIRE(a.ok());
   BOOST_REQUIRE(b.ok());
   BOOST_TEST(a.value.user == "Alice");
   BOOST_TEST(b.value.user == "Bob");
}

BOOST_AUTO_TEST_CASE(http_missing_subject_and_tls_override_are_rejected_before_method) {
   auto runtime = forge::asio::runtime{};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   registry.install<api>(implementation);
   auto router = http::router{};
   router.mount(forge::api::http::binding().use(core::binding().serve(registry).build())
      .post<&api::who, input, output>("/who/:name").build());
   auto denied = forge::asio::blocking::run(runtime, handle(router, request("Alice")));
   BOOST_TEST(denied.result_int() >= 400U);
   BOOST_TEST(denied.result_int() != 404U);
   BOOST_TEST(denied.result_int() != 405U);
   BOOST_TEST(implementation->calls.load() == 0U);

   auto value = request("Alice");
   auto route = http::make_route_context(value);
   route.client_certificate_fingerprint = forge::crypto::digest::sha256::hash(std::string{"TLS"});
   const auto descriptor = api::describe();
   const auto method = *core::find_method(descriptor, "who");
   auto conflicting = [](const http::route_context&, const core::method_descriptor&)
      -> boost::asio::awaitable<core::invocation_context> {
      auto ctx = admitted("Alice");
      ctx.trusted = core::trusted_invocation_builder{}.merge(ctx.trusted)
         .set(forge::api::auth::authenticated_caller{}).build();
      co_return ctx;
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime,
      forge::api::http::detail::provide_invocation(conflicting, route, method)), core::exceptions::protocol_error);
   auto valid = forge::asio::blocking::run(runtime, forge::api::http::detail::provide_invocation(
      [](const http::route_context&, const core::method_descriptor&) -> boost::asio::awaitable<core::invocation_context> {
         co_return admitted("Alice");
      }, route, method));
   BOOST_TEST(valid.trusted.contains<forge::api::auth::authenticated_caller>());
   BOOST_TEST(valid.trusted.find<subject>()->token()->subject() == "Alice");
}

BOOST_AUTO_TEST_CASE(lifetime_cancels_and_joins_on_revoke_and_expiry) {
   auto runtime = forge::asio::runtime{};
   for (const auto revoke : {false, true}) {
      auto exited = std::make_shared<std::atomic_bool>(false);
      auto denials = std::make_shared<std::atomic_uint>(0);
      auto context = admitted("Alice");
      if (revoke) {
         context.lifetime.recheck_interval = std::chrono::milliseconds{5};
         context.lifetime.recheck_timeout = std::chrono::milliseconds{50};
         context.lifetime.recheck = [](core::trusted_invocation trusted) -> boost::asio::awaitable<bool> {
            BOOST_TEST(trusted.find<subject>()->token()->subject() == "Alice");
            co_return false;
         };
      } else {
         context.lifetime.expires_at = std::chrono::system_clock::now() + std::chrono::milliseconds{20};
      }
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, core::enforce_invocation(
         wait_operation(exited), context, [denials](std::exception_ptr reason) {
            BOOST_TEST(static_cast<bool>(reason));
            denials->fetch_add(1);
         })), core::exceptions::invocation_denied);
      BOOST_TEST(exited->load());
      BOOST_TEST(denials->load() == 1U);
   }
}

BOOST_AUTO_TEST_CASE(inactive_and_completed_invocations_have_no_denial_watchdog) {
   auto runtime = forge::asio::runtime{};
   auto exited = std::make_shared<std::atomic_bool>(false);
   auto denied = std::make_shared<std::atomic_uint>(0);
   BOOST_TEST(forge::asio::blocking::run(runtime, core::enforce_invocation(
      wait_operation(exited, std::chrono::milliseconds{1}), {},
      [denied](std::exception_ptr) { denied->fetch_add(1); })) == 7);
   auto context = core::invocation_context{};
   context.lifetime.recheck_interval = std::chrono::seconds{1};
   context.lifetime.recheck_timeout = std::chrono::seconds{1};
   context.lifetime.recheck = [](core::trusted_invocation) -> boost::asio::awaitable<bool> { co_return false; };
   BOOST_TEST(forge::asio::blocking::run(runtime, core::enforce_invocation(
      wait_operation(exited, std::chrono::milliseconds{1}), context,
      [denied](std::exception_ptr) { denied->fetch_add(1); })) == 7);
   BOOST_TEST(denied->load() == 0U);
}

BOOST_AUTO_TEST_CASE(lifetime_rejects_overflow_and_does_not_extend_bound_expiry) {
   auto runtime = forge::asio::runtime{};
   auto lifetime = core::invocation_lifetime{};
   lifetime.recheck = [](core::trusted_invocation) -> boost::asio::awaitable<bool> { co_return true; };
   lifetime.recheck_interval = std::chrono::milliseconds::max();
   lifetime.recheck_timeout = std::chrono::milliseconds{1};
   BOOST_CHECK_THROW(lifetime.check(), core::exceptions::protocol_error);
   lifetime = {};
   lifetime.limit_for(std::chrono::milliseconds{5});
   // Even removing the wall-clock expiry cannot remove the monotonic ceiling.
   lifetime.expires_at = std::chrono::system_clock::now() + std::chrono::hours{1};
   lifetime.bind_expiry();
   lifetime.expires_at.reset();
   auto context = core::invocation_context{{}, lifetime};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime,
      core::enforce_invocation(wait_operation(std::make_shared<std::atomic_bool>(false)), context)),
      core::exceptions::invocation_denied);
}

BOOST_AUTO_TEST_CASE(recheck_timeout_fails_closed_and_drains_the_check) {
   auto runtime = forge::asio::runtime{};
   auto exited = std::make_shared<std::atomic_bool>(false);
   auto context = admitted("Alice");
   context.lifetime.recheck_interval = std::chrono::milliseconds{1};
   context.lifetime.recheck_timeout = std::chrono::milliseconds{5};
   context.lifetime.recheck = [exited](core::trusted_invocation) -> boost::asio::awaitable<bool> {
      static_cast<void>(co_await wait_operation(exited));
      co_return true;
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, core::enforce_invocation(
      wait_operation(std::make_shared<std::atomic_bool>(false)), context)), core::exceptions::invocation_denied);
   BOOST_TEST(exited->load());
}

class slow_body final : public http::body_reader::source {
 public:
   std::atomic_bool started{false};
   std::atomic_bool cancelled{false};
   std::shared_ptr<std::atomic_bool> exited = std::make_shared<std::atomic_bool>(false);
   boost::asio::awaitable<std::optional<http::body_chunk>> async_read() override {
      started.store(true);
      static_cast<void>(co_await wait_operation(exited));
      co_return std::nullopt;
   }
   std::uint64_t bytes_read() const noexcept override { return 0; }
   void cancel() noexcept override { cancelled.store(true); }
};

class cancel_probe final : public http::body_reader::source {
 public:
   std::promise<void> cancelled;
   boost::asio::awaitable<std::optional<http::body_chunk>> async_read() override { co_return std::nullopt; }
   std::uint64_t bytes_read() const noexcept override { return 0; }
   void cancel() noexcept override {
      if (!done_.exchange(true)) {
         cancelled.set_value();
      }
   }

 private:
   std::atomic_bool done_{false};
};

BOOST_AUTO_TEST_CASE(http_buffered_admission_revoke_cancels_and_joins_pending_body) {
   auto runtime = forge::asio::runtime{};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   registry.install<api>(implementation);
   auto source = std::make_shared<slow_body>();
   auto provider_calls = std::atomic_uint{0};
   auto typed_denials = std::atomic_uint{0};
   auto recheck_denied = std::atomic_bool{false};
   auto router = http::router{};
   router.mount(
       forge::api::http::binding()
           .use(core::binding().serve(registry).build())
           .invocation([source, &provider_calls, &recheck_denied](
                           const http::route_context&,
                           const core::method_descriptor&) -> boost::asio::awaitable<core::invocation_context> {
              provider_calls.fetch_add(1);
              auto context = admitted("Alice");
              context.lifetime.limit_for(std::chrono::seconds{1});
              context.lifetime.recheck_interval = std::chrono::milliseconds{1};
              context.lifetime.recheck_timeout = std::chrono::milliseconds{5};
              context.lifetime.recheck = [source,
                                          &recheck_denied](core::trusted_invocation) -> boost::asio::awaitable<bool> {
                 // Revoke only after this request has actually entered its buffered body read.
                 if (source->started.load()) {
                    recheck_denied.store(true);
                    co_return false;
                 }
                 co_return true;
              };
              co_return context;
           })
           .errors([&typed_denials](const forge::api::http::error_context& context) -> std::optional<http::response> {
              if (dynamic_cast<const core::exceptions::invocation_denied*>(context.cause)) {
                 typed_denials.fetch_add(1);
              }
              return http::make_text_response(context.request, context.status, "denied");
           })
           .post<&api::who, input, output>("/who/:name")
           .build());
   auto value = request("Alice");
   value.body().clear();
   auto context = http::make_route_context(value);
   context.runtime = &runtime;
   BOOST_REQUIRE(router.can_handle_stream(context));
   auto stream = http::stream_request{context, http::body_reader{source}};
   const auto response = forge::asio::blocking::run(runtime, router.handle_stream(stream));
   BOOST_TEST(response.head.result_int() == 403U);
   BOOST_TEST(provider_calls.load() == 1U);
   BOOST_TEST(typed_denials.load() == 1U);
   BOOST_TEST(recheck_denied.load());
   BOOST_TEST(source->started.load());
   BOOST_TEST(source->cancelled.load());
   BOOST_TEST(source->exited->load());
   BOOST_TEST(implementation->calls.load() == 0U);
}

boost::asio::awaitable<std::size_t> drain_response(
   boost::beast::tcp_stream& socket, boost::beast::flat_buffer& buffer,
   boost::beast::http::response_parser<boost::beast::http::buffer_body>& parser) {
   auto total = std::size_t{0};
   auto bytes = std::array<std::byte, 64U * 1024U>{};
   for (;;) {
      parser.get().body().data = bytes.data();
      parser.get().body().size = bytes.size();
      socket.expires_after(std::chrono::seconds{2});
      auto error = boost::system::error_code{};
      static_cast<void>(co_await boost::beast::http::async_read_some(
         socket, buffer, parser, boost::asio::redirect_error(boost::asio::use_awaitable, error)));
      total += bytes.size() - parser.get().body().size;
      if (error && error != boost::beast::http::error::need_buffer) {
         BOOST_CHECK(error != boost::beast::error::timeout);
         co_return total;
      }
      if (parser.is_done()) {
         co_return total;
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_revoke_cancels_after_headers) {
   auto runtime = forge::asio::runtime{};
   auto source = std::make_shared<slow_body>();
   auto context = admitted("Alice");
   context.lifetime.recheck_interval = std::chrono::milliseconds{1};
   context.lifetime.recheck_timeout = std::chrono::milliseconds{5};
   context.lifetime.recheck = [](core::trusted_invocation) -> boost::asio::awaitable<bool> { co_return false; };
   auto output = forge::api::http::detail::protect_response_body(
      http::stream_response{
         .head = http::response{http::status::ok, 11},
         .body = [source]() -> boost::asio::awaitable<std::optional<http::body_chunk>> {
            co_return co_await source->async_read();
         },
      }, context, http::body_reader{source});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, output.body()), core::exceptions::invocation_denied);
   BOOST_TEST(source->cancelled.load());
}

BOOST_AUTO_TEST_CASE(buffered_stream_send_guard_survives_middleware_projection) {
   auto runtime = forge::asio::runtime{};
   auto calls = std::make_shared<std::atomic_uint>(0);
   auto router = http::router{};
   router.use([](http::route_context& context, http::next_handler next) -> boost::asio::awaitable<http::response> {
      auto output = co_await next();
      output.set("X-After", "middleware");
      static_cast<void>(context);
      co_return output;
   });
   router.get_stream("/buffered", [calls](http::stream_request& request)
      -> boost::asio::awaitable<http::stream_response> {
      auto output = http::stream_response::buffered(http::make_text_response(request.context.request,
                                                                             http::status::ok, "buffered"));
      output.send_guard = [calls](boost::asio::awaitable<void> operation, std::function<void()>)
         -> boost::asio::awaitable<void> {
         calls->fetch_add(1);
         co_await std::move(operation);
      };
      co_return output;
   });
   auto value = http::request{http::method::get, "/buffered", 11};
   auto context = http::make_route_context(value);
   auto stream = http::stream_request{context, {}};
   auto output = forge::asio::blocking::run(runtime, router.handle_stream(stream));
   BOOST_REQUIRE(static_cast<bool>(output.send_guard));
   BOOST_TEST(output.head.body() == "buffered");
   auto exited = std::make_shared<std::atomic_bool>(false);
   auto operation = [exited]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await wait_operation(exited, std::chrono::milliseconds{1}));
   };
   forge::asio::blocking::run(runtime, output.send_guard(operation(), [] {}));
   BOOST_TEST(exited->load());
   BOOST_TEST(calls->load() == 1U);
}

BOOST_AUTO_TEST_CASE(http_slow_socket_send_is_cut_off_on_expiry_and_revoke) {
   constexpr auto payload_size = std::size_t{8U * 1024U * 1024U};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (const auto revoke : {false, true}) {
      auto source = std::make_shared<cancel_probe>();
      auto denied = source->cancelled.get_future();
      auto revoked = std::make_shared<std::atomic_bool>(false);
      auto produced = std::make_shared<std::promise<void>>();
      auto ready = produced->get_future();
      auto router = http::router{};
      router.get_stream("/slow", [source, revoked, produced, revoke](http::stream_request& request)
         -> boost::asio::awaitable<http::stream_response> {
         auto context = admitted("Alice");
         if (revoke) {
            context.lifetime.recheck_interval = std::chrono::milliseconds{10};
            context.lifetime.recheck_timeout = std::chrono::milliseconds{50};
            context.lifetime.recheck = [revoked](core::trusted_invocation) -> boost::asio::awaitable<bool> {
               co_return !revoked->load();
            };
         } else {
            context.lifetime.expires_at = std::chrono::system_clock::now() + std::chrono::milliseconds{200};
         }
         auto result = http::stream_response{
            .head = http::response{http::status::ok, 11},
            .body = [produced, sent = false]() mutable -> boost::asio::awaitable<std::optional<http::body_chunk>> {
               if (std::exchange(sent, true)) {
                  co_return std::nullopt;
               }
               auto chunk = http::body_chunk{.bytes = std::vector<std::byte>(payload_size, std::byte{42})};
               produced->set_value();
               co_return chunk;
            },
         };
         result.head.keep_alive(false);
         static_cast<void>(request);
         co_return forge::api::http::detail::protect_response_body(std::move(result), std::move(context),
                                                                   http::body_reader{source});
      });
      auto server = http::server{runtime, http::server_config{}, std::move(router)};
      server.start();
      auto socket = boost::beast::tcp_stream{runtime.context()};
      socket.socket().open(boost::asio::ip::tcp::v4());
      socket.socket().set_option(boost::asio::socket_base::receive_buffer_size{1024});
      socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
      auto value = boost::beast::http::request<boost::beast::http::empty_body>{boost::beast::http::verb::get,
                                                                            "/slow", 11};
      value.set(boost::beast::http::field::host, "127.0.0.1");
      boost::beast::http::write(socket, value);
      auto buffer = boost::beast::flat_buffer{};
      auto parser = boost::beast::http::response_parser<boost::beast::http::buffer_body>{};
      parser.body_limit(payload_size * 2U);
      boost::beast::http::read_header(socket, buffer, parser);
      BOOST_REQUIRE(ready.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      // The chunk is fetched; the peer deliberately does not consume its body until authority ends.
      revoked->store(true);
      BOOST_REQUIRE(denied.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
      const auto received = forge::asio::blocking::run(runtime, drain_response(socket, buffer, parser));
      BOOST_TEST(received < payload_size);
      BOOST_TEST(!parser.is_done());
      server.stop();
   }
}

BOOST_AUTO_TEST_CASE(http_stream_shutdown_joins_active_handler_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   auto started = implementation->handler_started.get_future();
   registry.install<api>(implementation);
   auto router = http::router{};
   router.mount(forge::api::http::binding().use(core::binding().serve(registry).build())
      .invocation([](const http::route_context&, const core::method_descriptor&)
         -> boost::asio::awaitable<core::invocation_context> { co_return admitted("Alice"); })
      .post<&api::watch, input, void>("/watch").build());
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto io = boost::asio::io_context{};
   auto socket = boost::beast::tcp_stream{io};
   socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
   auto value = boost::beast::http::request<boost::beast::http::string_body>{boost::beast::http::verb::post,
                                                                        "/watch", 11};
   value.set(boost::beast::http::field::host, "127.0.0.1");
   value.set(boost::beast::http::field::content_type, "application/json");
   value.set(boost::beast::http::field::accept, "application/vnd.forge.api-stream");
   value.body() = R"({"user":{},"claimed":"hold"})";
   value.prepare_payload();
   boost::beast::http::write(socket, value);
   auto buffer = boost::beast::flat_buffer{};
   auto parser = boost::beast::http::response_parser<boost::beast::http::buffer_body>{};
   boost::beast::http::read_header(socket, buffer, parser);
   BOOST_REQUIRE(parser.get().result_int() == 200U);
   BOOST_REQUIRE(started.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   auto stopped = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   stopped.get();
   BOOST_TEST(implementation->handler_exited.load());
}

BOOST_AUTO_TEST_CASE(websocket_captured_subjects_are_isolated_and_revoke_closes_only_alice) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   registry.install<api>(implementation);
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build()).build();
   auto revoked = std::make_shared<std::atomic_bool>(false);
   auto router = http::router{};
   router.websocket_guarded("/ws/:name", binding.guarded(
      [revoked](const http::route_context& route) -> boost::asio::awaitable<core::invocation_context> {
         auto context = admitted(route.route_params.at("name"));
         context.lifetime.recheck_interval = std::chrono::milliseconds{5};
         context.lifetime.recheck_timeout = std::chrono::milliseconds{50};
         context.lifetime.recheck = [revoked](core::trusted_invocation trusted) -> boost::asio::awaitable<bool> {
            co_return trusted.find<subject>()->token()->subject() != "Alice" || !revoked->load();
         };
         co_return context;
      }));
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto client = forge::net::websocket::client{runtime,
      http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port()))};
   auto scenario = [&]() -> boost::asio::awaitable<void> {
      auto alice_socket = co_await client.async_connect("/ws/Alice");
      auto bob_socket = co_await client.async_connect("/ws/Bob");
      auto alice = co_await binding.connect(alice_socket);
      auto bob = co_await binding.connect(bob_socket);
      auto alice_api = co_await alice.get_remote_api<api>();
      auto bob_api = co_await bob.get_remote_api<api>();
      const auto spoofed = input{subject{verifier::issue("spoofed-Bob")}, "spoofed-Bob"};
      BOOST_TEST((co_await alice_api->who(spoofed)).user == "Alice");
      BOOST_TEST((co_await bob_api->who(spoofed)).user == "Bob");
      revoked->store(true);
      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      timer.expires_after(std::chrono::milliseconds{100});
      co_await timer.async_wait(boost::asio::use_awaitable);
      auto denied = false;
      try {
         static_cast<void>(co_await alice_api->who(spoofed));
      } catch (...) {
         denied = true;
      }
      BOOST_TEST(denied);
      BOOST_TEST((co_await bob_api->who(spoofed)).user == "Bob");
      BOOST_TEST(implementation->calls.load() == 3U);
      co_await bob.async_close();
   };
   forge::asio::blocking::run(runtime, scenario());
   server.stop();
}

BOOST_AUTO_TEST_CASE(websocket_guard_rejects_missing_subject_before_upgrade) {
   auto runtime = forge::asio::runtime{};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   registry.install<api>(implementation);
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build()).build();
   auto router = http::router{};
   router.websocket_guarded("/ws", binding.guarded(
      [](const http::route_context&) -> boost::asio::awaitable<core::invocation_context> { co_return core::invocation_context{}; }));
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto io = boost::asio::io_context{};
   auto socket = boost::beast::tcp_stream{io};
   socket.expires_after(std::chrono::seconds{2});
   socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
   auto upgrade = upgrade_request("/ws");
   boost::beast::http::write(socket, upgrade);
   auto buffer = boost::beast::flat_buffer{};
   auto response = boost::beast::http::response<boost::beast::http::string_body>{};
   boost::beast::http::read(socket, buffer, response);
   BOOST_TEST(response.result_int() == 403U);
   BOOST_TEST(implementation->calls.load() == 0U);
   server.stop();
}

BOOST_AUTO_TEST_CASE(websocket_shutdown_cancels_pending_admission_before_101) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   registry.install<api>(std::make_shared<service>());
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build()).build();
   auto entered = std::make_shared<std::promise<void>>();
   auto started = entered->get_future();
   auto exited = std::make_shared<std::atomic_bool>(false);
   auto router = http::router{};
   router.websocket_guarded("/ws", binding.guarded(
      [entered, exited](const http::route_context&) -> boost::asio::awaitable<core::invocation_context> {
         entered->set_value();
         static_cast<void>(co_await wait_operation(exited, std::chrono::seconds{2}));
         co_return admitted("Alice");
      }));
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto io = boost::asio::io_context{};
   auto socket = boost::beast::tcp_stream{io};
   socket.connect({boost::asio::ip::make_address("127.0.0.1"), server.port()});
   boost::beast::http::write(socket, upgrade_request("/ws"));
   BOOST_REQUIRE(started.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   auto stopped = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   stopped.get();
   BOOST_TEST(exited->load());
   auto buffer = boost::beast::flat_buffer{};
   auto response = boost::beast::http::response<boost::beast::http::string_body>{};
   auto error = boost::system::error_code{};
   boost::beast::http::read(socket, buffer, response, error);
   BOOST_CHECK(error);
   BOOST_TEST(response.result_int() != 101U);
}

BOOST_AUTO_TEST_CASE(websocket_accept_cancellation_joins_active_handler_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   auto started = implementation->handler_started.get_future();
   registry.install<api>(implementation);
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build()).build();
   auto executor = boost::asio::make_strand(runtime.context());
   auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   auto completed = std::make_shared<std::promise<void>>();
   auto accepted = completed->get_future();
   auto router = http::router{};
   router.websocket("/ws", [binding, executor, cancellation, completed](auto socket) {
      boost::asio::co_spawn(executor, binding.accept(std::move(socket), admitted("Alice")),
         boost::asio::bind_cancellation_slot(cancellation->slot(), [completed](std::exception_ptr) {
            completed->set_value();
         }));
   });
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto client = forge::net::websocket::client{runtime,
      http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port()))};
   auto call = [&]() -> boost::asio::awaitable<void> {
      auto socket = co_await client.async_connect("/ws");
      auto remote = co_await binding.connect(socket);
      auto proxy = co_await remote.get_remote_api<api>();
      try {
         static_cast<void>(co_await proxy->who({{}, "hold"}));
      } catch (...) {
      }
      co_await remote.async_close();
   };
   auto pending = boost::asio::co_spawn(runtime.context(), call(), boost::asio::use_future);
   BOOST_REQUIRE(started.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   boost::asio::post(executor, [cancellation] { cancellation->emit(boost::asio::cancellation_type::all); });
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   accepted.get();
   BOOST_TEST(implementation->handler_exited.load());
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   pending.get();
   server.stop();
}

BOOST_AUTO_TEST_CASE(live_upgraded_websocket_shutdown_drains_active_api_handler) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto registry = core::registry{};
   auto implementation = std::make_shared<service>();
   auto started = implementation->handler_started.get_future();
   registry.install<api>(implementation);
   auto binding = forge::api::websocket::api().use(core::binding().serve(registry).build()).build();
   auto router = http::router{};
   router.websocket_guarded("/ws", binding.guarded(
      [](const http::route_context&) -> boost::asio::awaitable<core::invocation_context> {
         co_return admitted("Alice");
      }));
   auto server = http::server{runtime, http::server_config{}, std::move(router)};
   server.start();
   auto client = forge::net::websocket::client{runtime,
      http::parse_base_url("http://127.0.0.1:" + std::to_string(server.port()))};
   auto call = [&]() -> boost::asio::awaitable<void> {
      auto socket = co_await client.async_connect("/ws");
      auto remote = co_await binding.connect(socket);
      auto proxy = co_await remote.get_remote_api<api>();
      auto denied = false;
      try {
         static_cast<void>(co_await proxy->who({{}, "hold"}));
      } catch (...) {
         denied = true;
      }
      BOOST_TEST(denied);
      co_await remote.async_close();
   };
   auto pending = boost::asio::co_spawn(runtime.context(), call(), boost::asio::use_future);
   BOOST_REQUIRE(started.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   auto stopped = boost::asio::co_spawn(runtime.context(), server.async_stop(), boost::asio::use_future);
   BOOST_REQUIRE(stopped.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   stopped.get();
   BOOST_TEST(implementation->handler_exited.load());
   BOOST_REQUIRE(pending.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   pending.get();
   const auto calls = implementation->calls.load();
   BOOST_TEST(calls == 1U);
}

BOOST_AUTO_TEST_CASE(websocket_backpressure_revoke_aborts_pending_write_and_joins) {
   check_websocket_backpressure(true);
}

BOOST_AUTO_TEST_CASE(websocket_backpressure_shutdown_aborts_pending_write_and_joins) {
   check_websocket_backpressure(false);
}

BOOST_AUTO_TEST_CASE(websocket_pending_write_close_cancel_joins_native_callback) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto network = boost::asio::make_strand(runtime.context());
   auto observer = boost::asio::make_strand(runtime.context());
   auto listener = boost::asio::ip::tcp::acceptor{
      runtime.context(), {boost::asio::ip::make_address("127.0.0.1"), 0}};
   auto io = boost::asio::io_context{};
   auto peer = boost::beast::websocket::stream<boost::beast::tcp_stream>{io};
   auto& peer_socket = boost::beast::get_lowest_layer(peer).socket();
   peer_socket.open(boost::asio::ip::tcp::v4());
   peer_socket.set_option(boost::asio::socket_base::receive_buffer_size{1024});
   peer_socket.connect(listener.local_endpoint());
   auto stream = boost::beast::tcp_stream{network};
   listener.accept(stream.socket());
   stream.socket().set_option(boost::asio::socket_base::send_buffer_size{4096});
   auto accept = [](boost::beast::tcp_stream stream)
      -> boost::asio::awaitable<forge::net::websocket::connection::ptr> {
      auto buffer = boost::beast::flat_buffer{};
      auto request = boost::beast::http::request<boost::beast::http::string_body>{};
      co_await boost::beast::http::async_read(stream, buffer, request, boost::asio::use_awaitable);
      auto connection = forge::net::websocket::connection::create(std::move(stream));
      co_await connection->accept(request);
      co_return connection;
   };
   auto accepted = boost::asio::co_spawn(network, accept(std::move(stream)), boost::asio::use_future);
   peer.handshake("127.0.0.1", "/pending-close");
   BOOST_REQUIRE(accepted.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   auto connection = accepted.get();
   auto writing = boost::asio::co_spawn(observer, connection->send_binary(std::string(512U * 1024U, 'x')),
                                        boost::asio::use_future);
   auto observation = forge::asio::blocking::run(runtime, wait_for_write_backpressure(connection));
   const auto latest = connection->metrics();
   BOOST_REQUIRE_MESSAGE(observation.has_value(),
      "WebSocket write did not remain pending: queued=" << latest.queued_writes
      << ", sent=" << latest.sent_messages << ", failed=" << latest.failed_writes
      << ", closed=" << latest.close_count);
   const auto before = observation->before;
   const auto stalled = observation->stalled;
   BOOST_REQUIRE(before.queued_writes == 1U);
   BOOST_REQUIRE(stalled.queued_writes == 1U);
   BOOST_REQUIRE(stalled.sent_messages == before.sent_messages);
   BOOST_REQUIRE(writing.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);
   auto close = [connection]() -> boost::asio::awaitable<std::pair<bool, forge::net::websocket::connection_metrics>> {
      auto failed = false;
      try {
         co_await connection->close();
      } catch (...) {
         failed = true;
      }
      co_return std::pair{failed, connection->metrics()};
   };
   auto closing = boost::asio::co_spawn(observer, close(), boost::asio::use_future);
   BOOST_REQUIRE(forge::asio::blocking::run(runtime, wait_for_connection_metrics(connection,
      [](const forge::net::websocket::connection_metrics& metrics, std::chrono::steady_clock::time_point) {
         return metrics.queued_writes == 2U;
      })));
   BOOST_REQUIRE(connection->metrics().queued_writes == 2U);
   BOOST_REQUIRE(closing.wait_for(std::chrono::milliseconds{0}) == std::future_status::timeout);

   auto cancelled = std::make_shared<std::promise<void>>();
   auto cancellation_applied = cancelled->get_future();
   auto release = std::make_shared<std::promise<void>>();
   auto released = release->get_future().share();
   boost::asio::post(network, [connection, cancelled, released] {
      connection->cancel();
      cancelled->set_value();
      // Test-only bounded barrier: another worker can observe close, but native callbacks cannot run yet.
      static_cast<void>(released.wait_for(std::chrono::milliseconds{500}));
   });
   const auto applied = cancellation_applied.wait_for(std::chrono::seconds{1});
   const auto early_close = closing.wait_for(std::chrono::milliseconds{50});
   release->set_value();
   BOOST_REQUIRE(applied == std::future_status::ready);
   BOOST_CHECK(early_close == std::future_status::timeout);
   BOOST_REQUIRE(closing.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   const auto [failed, drained] = closing.get();
   BOOST_TEST(failed);
   BOOST_TEST(drained.failed_writes == 1U);
   BOOST_TEST(drained.queued_writes == 0U);
   BOOST_REQUIRE(writing.wait_for(std::chrono::seconds{1}) == std::future_status::ready);
   BOOST_CHECK_THROW(writing.get(), std::exception);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace f3_test
