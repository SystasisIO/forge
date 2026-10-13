#include <boost/describe.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
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
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

import forge.api.auth.authenticated_subject;
import forge.api.core.binding;
import forge.api.core.handle;
import forge.api.core.invocation_context;
import forge.api.core.registry;
import forge.api.core.stream_writer;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.proxy;
import forge.api.stream.options;
import forge.api.websocket.binding;
import forge.asio.compute;
import forge.asio.runtime;
import forge.auth.liboauth2.verifier;
import forge.codec.json;
import forge.crypto.core.secret_string;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.types;
import forge.net.transport.frame;
import forge.raw.raw;
import forge.variant.described;

namespace auth_resource {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace core = forge::api::core;
namespace http = forge::net::http;
namespace oauth2 = forge::auth::oauth2;
using verifier = forge::auth::liboauth2::verifier;
using subject = forge::api::auth::authenticated_subject;
using secret = forge::crypto::core::secret_string;
using namespace std::chrono_literals;

constexpr auto issuer = "https://localhost:18443/realms/forge-live";
constexpr auto audience = "forge-resource";
constexpr auto workload = "forge-workload";
constexpr auto max_token_bytes = std::size_t{16 * 1024};
constexpr auto max_line_bytes = std::size_t{32 * 1024};
constexpr auto max_frame_bytes = std::uint32_t{64 * 1024};
constexpr auto max_item_bytes = std::uint32_t{4 * 1024};

struct initial_record {
   std::string phase;
   std::string token;
   std::string introspection_client_secret;

   initial_record() = default;
   initial_record(initial_record&&) noexcept = default;
   initial_record& operator=(initial_record&&) noexcept = default;
   initial_record(const initial_record&) = delete;
   initial_record& operator=(const initial_record&) = delete;

   ~initial_record() {
      secret{std::move(token)}.clear();
      secret{std::move(introspection_client_secret)}.clear();
   }
};

struct rotation_record {
   std::string phase;
   std::string token;

   rotation_record() = default;
   rotation_record(rotation_record&&) noexcept = default;
   rotation_record& operator=(rotation_record&&) noexcept = default;
   rotation_record(const rotation_record&) = delete;
   rotation_record& operator=(const rotation_record&) = delete;

   ~rotation_record() { secret{std::move(token)}.clear(); }
};

struct input {
   subject user;
   std::string claimed;
};

struct output {
   bool authenticated = false;
   bool expected_identity = false;
   bool spoof_replaced = false;
};

class api : public core::contract<api, core::surface::local | core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual asio::awaitable<output> who(input request) = 0;
   virtual asio::awaitable<void> watch(input request, core::stream_writer<output> writer) = 0;
};

class service final : public api {
 public:
   explicit service(oauth2::verified_token expected) : _expected{std::move(expected)} {}

   asio::awaitable<output> who(input request) override { co_return inspect(request); }

   asio::awaitable<void> watch(input request, core::stream_writer<output> writer) override {
      co_await writer.async_write(inspect(request));
   }

 private:
   output inspect(const input& request) const {
      const auto* verified = request.user.token();
      return {
         .authenticated = request.user.authenticated() && verified != nullptr,
         .expected_identity = verified != nullptr && verified->issuer() == _expected.issuer() &&
            verified->subject() == _expected.subject() && verified->client_id() == _expected.client_id() &&
            verified->profile() == _expected.profile() &&
            verified->valid_at(std::chrono::system_clock::now()),
         .spoof_replaced = verified != nullptr && !request.claimed.empty() &&
            verified->subject() != request.claimed,
      };
   }

   const oauth2::verified_token _expected;
};

BOOST_DESCRIBE_STRUCT(initial_record, (), (phase, token, introspection_client_secret))
BOOST_DESCRIBE_STRUCT(rotation_record, (), (phase, token))
BOOST_DESCRIBE_STRUCT(input, (), (user, claimed))
BOOST_DESCRIBE_STRUCT(output, (), (authenticated, expected_identity, spoof_replaced))

} // namespace auth_resource

FORGE_API(::auth_resource::api, FORGE_API_CONTRACT("test.auth.resource", 1, 0),
          FORGE_API_METHOD(who, request), FORGE_API_METHOD(watch, request))
FORGE_HTTP_API(::auth_resource::api, FORGE_HTTP_POST(who, "/who", ok), FORGE_HTTP_POST(watch, "/watch", ok))

namespace auth_resource {
namespace {

int output_fd = STDOUT_FILENO;
std::atomic_bool failed{false};

void emit(std::string_view label) noexcept {
   if (::write(output_fd, label.data(), label.size()) != static_cast<ssize_t>(label.size())) {
      std::_Exit(1);
   }
}

void fail(std::string_view label) noexcept {
   if (!failed.exchange(true)) {
      emit(label);
   }
}

void require(bool condition) {
   if (!condition) {
      throw std::runtime_error{"resource fixture assertion failed"};
   }
}

void private_output() {
   const auto result = ::dup(STDOUT_FILENO);
   require(result >= 0);
   output_fd = result;
   require(::fcntl(output_fd, F_SETFD, FD_CLOEXEC) == 0);
   const auto flags = ::fcntl(output_fd, F_GETFL);
   require(flags >= 0 && ::fcntl(output_fd, F_SETFL, flags | O_NONBLOCK) == 0);
   const auto sink = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
   require(sink >= 0);
   const auto out = ::dup2(sink, STDOUT_FILENO);
   const auto err = ::dup2(sink, STDERR_FILENO);
   ::close(sink);
   require(out >= 0 && err >= 0);
   // Only emit() owns the private result pipe; SDK/vendor diagnostics cannot reach it.
   std::signal(SIGPIPE, SIG_IGN);
}

std::filesystem::path configuration(int argc, char** argv) {
   require(argc == 3 && std::string_view{argv[1]} == "--ca");
   const auto value = std::string_view{argv[2]};
   require(!value.empty() && value.size() <= 4096);
   auto ca = std::filesystem::path{value};
   require(ca.is_absolute() && std::filesystem::is_regular_file(ca));
   struct stat metadata{};
   // Tokens must arrive through the orchestrator's private pipe, never a tty/token file.
   require(::fstat(STDIN_FILENO, &metadata) == 0 && S_ISFIFO(metadata.st_mode));
   return ca;
}

secret read_line(std::chrono::seconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   auto value = std::string{};
   value.reserve(max_line_bytes);
   try {
      for (;;) {
         const auto remaining = deadline - std::chrono::steady_clock::now();
         require(remaining > std::chrono::steady_clock::duration::zero());
         auto descriptor = pollfd{STDIN_FILENO, POLLIN, 0};
         const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
         const auto ready = ::poll(&descriptor, 1, static_cast<int>(milliseconds));
         if (ready < 0 && errno == EINTR) {
            continue;
         }
         require(ready > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0);
         auto byte = char{};
         const auto count = ::read(STDIN_FILENO, &byte, 1);
         if (count < 0 && errno == EINTR) {
            continue;
         }
         require(count == 1);
         if (byte == '\n') {
            require(!value.empty());
            return secret{std::move(value)};
         }
         require(byte >= 32 && byte <= 126 && value.size() < max_line_bytes);
         value.push_back(byte);
      }
   } catch (...) {
      secret{std::move(value)}.clear();
      throw;
   }
}

bool bounded_secret(std::string_view value, std::size_t maximum) {
   return !value.empty() && value.size() <= maximum &&
      std::all_of(value.begin(), value.end(), [](unsigned char byte) { return byte >= 33 && byte <= 126; });
}

template <typename T>
T parse_record(const secret& line) {
   auto parsed = forge::codec::json::read<T>(line.view(), {
      .max_depth = 4,
      .unknown_fields = forge::codec::json::unknown_field_policy::error,
      .described_records = forge::codec::json::described_record_policy::exact,
   });
   require(parsed.ok());
   return std::move(parsed.value);
}

template <typename T>
asio::awaitable<std::optional<std::conditional_t<std::is_void_v<T>, bool, T>>>
completion(asio::awaitable<T> operation) {
   if constexpr (std::is_void_v<T>) {
      co_await std::move(operation);
      co_return true;
   } else {
      co_return std::optional<T>{co_await std::move(operation)};
   }
}

template <typename T>
T run(forge::asio::runtime& runtime, asio::awaitable<T> operation) {
   auto context = core::invocation_context{};
   context.lifetime.limit_for(10s);
   auto result = asio::co_spawn(asio::make_strand(runtime.context()),
      completion(core::enforce_invocation(std::move(operation), std::move(context))), asio::use_future);
   if (result.wait_for(12s) != std::future_status::ready) {
      // A canceled native callback that will not join is a failed disposable process, not drain success.
      fail("FAIL/cancellation-drain\n");
      std::_Exit(124);
   }
   auto completed = result.get();
   require(completed.has_value());
   if constexpr (!std::is_void_v<T>) {
      return std::move(*completed);
   }
}

asio::awaitable<void> wait_for_refresh_cooldown() {
   auto timer = asio::steady_timer{co_await asio::this_coro::executor};
   // The first JWKS fetch starts the 1 s refresh cooldown, even while its snapshot remains fresh.
   timer.expires_after(2s);
   co_await timer.async_wait(asio::use_awaitable);
}

verifier::options jwt_options(const std::filesystem::path& ca) {
   return {
      .policy = {.issuer = issuer, .audience = audience, .version = "real-idp-v1",
                 .algorithms = {oauth2::signature_algorithm::rs256}, .required_scopes = {"profile"}},
      .source = verifier::jwks{.endpoint = std::string{issuer} + "/protocol/openid-connect/certs",
                              .max_keys = 16, .cache_lifetime = 600s, .refresh_interval = 1s,
                              .refresh_wait_timeout = 500ms},
      .ca_bundle = ca,
      .http_timeout = 2s,
      .timeout = 5s,
      .max_in_flight = 4,
      .max_token_bytes = max_token_bytes,
      .max_response_bytes = 128 * 1024,
      .max_claim_bytes = 16 * 1024,
   };
}

asio::awaitable<core::invocation_context> admit(std::shared_ptr<verifier> instance,
                                               oauth2::verification_policy policy,
                                               const http::route_context& route) {
   const auto headers = route.request.headers();
   require(std::count_if(headers.begin(), headers.end(), [](const auto& header) {
      return http::header_name_equal(header.name, "Authorization");
   }) == 1);
   const auto authorization = route.request.header("Authorization");
   require(authorization && authorization->starts_with("Bearer "));
   const auto bearer = authorization->substr(7);
   require(bounded_secret(bearer, max_token_bytes));
   auto verified = co_await instance->async_verify(secret{std::string{bearer}}, std::move(policy));
   auto context = core::invocation_context{};
   context.lifetime.expires_at = verified.valid_until();
   context.lifetime.limit_for(10s);
   context.trusted = core::trusted_invocation_builder{}.set(subject{std::move(verified)}).build();
   co_return context;
}

void mount(http::router& router, core::registry& registry, std::shared_ptr<verifier> instance,
           oauth2::verification_policy policy, std::string prefix) {
   auto plan = core::binding().serve(registry).build();
   forge::api::http::binding().use(plan)
      .invocation([instance, policy](const auto& route, const auto&) -> asio::awaitable<core::invocation_context> {
         co_return co_await admit(instance, policy, route);
      }).bind<api>().build().mount(router, prefix);
   auto websocket = forge::api::websocket::api_binding{std::move(plan),
      forge::api::stream::options{.max_inflight = 4, .deadline = 5s, .max_frame_size = max_frame_bytes,
                                  .max_item_size = max_item_bytes, .initial_window_items = 2,
                                  .initial_window_bytes = 8192, .max_buffered_bytes = 256 * 1024,
                                  .disconnect_grace = 1s, .control_timeout = 5s, .idle_timeout = 5s}};
   router.websocket_guarded(prefix + "/ws", websocket.guarded(
      [instance, policy](const auto& route) -> asio::awaitable<core::invocation_context> {
         co_return co_await admit(instance, policy, route);
      }));
}

void accepted(const output& value) {
   require(value.authenticated && value.expected_identity && value.spoof_replaced);
}

input spoof(const oauth2::verified_token& evidence) {
   // Actual production evidence is intentionally untrusted on the client; its wire form must be empty.
   auto value = subject{evidence};
   require(value.token() != nullptr && !value.authenticated() && forge::raw::pack(value).empty());
   return {std::move(value), "wire-spoofed-subject"};
}

asio::awaitable<void> http_scenario(forge::asio::runtime& runtime, std::uint16_t port, std::string prefix,
                                   secret token, oauth2::verified_token evidence) {
   auto client = http::client{runtime, http::parse_base_url("http://127.0.0.1:" + std::to_string(port) + prefix),
      {.header_provider = [token = std::move(token)](const http::request&) {
         return std::vector<http::header_entry>{{"Authorization", "Bearer " + std::string{token.view()}}};
      }, .max_provider_header_bytes = max_token_bytes + 64}};
   auto remote = co_await forge::api::http::remote<api>(client);
   accepted(co_await remote->who(spoof(evidence)));
   auto stream = co_await remote.async_open<&api::watch>(
      core::call_options{.max_item_bytes = max_item_bytes, .max_buffered_items = 2,
                         .max_buffered_bytes = 8192, .deadline = std::chrono::steady_clock::now() + 5s},
      spoof(evidence));
   const auto item = co_await stream.async_read();
   require(item.has_value());
   accepted(*item);
   require(!(co_await stream.async_read()).has_value());
   co_await stream.async_finish();
}

using peer = beast::websocket::stream<beast::tcp_stream>;

asio::awaitable<void> send(peer& socket, core::frame value) {
   const auto payload = forge::raw::pack(value);
   const auto wire = forge::net::transport::encode_frame(payload, {.max_size = max_frame_bytes});
   co_await socket.async_write(asio::buffer(wire), asio::use_awaitable);
}

asio::awaitable<core::frame> receive(peer& socket) {
   auto buffer = beast::flat_buffer{max_frame_bytes + 4U};
   co_await socket.async_read(buffer, asio::use_awaitable);
   require(socket.got_binary());
   const auto data = buffer.cdata();
   const auto bytes = std::span<const std::uint8_t>{static_cast<const std::uint8_t*>(data.data()), data.size()};
   auto decoded = forge::net::transport::decode_frame(bytes, {.max_size = max_frame_bytes});
   require(decoded.status == forge::net::transport::frame_decode_status::complete && decoded.consumed == bytes.size());
   co_return forge::raw::unpack_exact<core::frame>(decoded.payload);
}

void check_frame(const core::frame& value, std::uint64_t id, std::string_view method) {
   require(value.id.value == id && value.api == api::ref() && value.method == method &&
           value.codec.value == "forge.raw" && value.meta.empty());
}

asio::awaitable<void> websocket_scenario(std::uint16_t port, std::string prefix, secret token,
                                        oauth2::verified_token evidence) {
   // Beast is only the test peer; the server and every frame/DTO codec are the production Forge path.
   auto socket = peer{co_await asio::this_coro::executor};
   socket.read_message_max(max_frame_bytes + 4U);
   socket.set_option(beast::websocket::stream_base::timeout{5s, 5s, false});
   socket.set_option(beast::websocket::stream_base::decorator{
      [authorization = secret{"Bearer " + std::string{token.view()}}](auto& request) {
         request.set(beast::http::field::authorization,
                     beast::string_view{authorization.data(), authorization.size()});
      }});
   socket.next_layer().expires_after(5s);
   co_await socket.next_layer().async_connect(
      {asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   socket.next_layer().expires_never();
   co_await socket.async_handshake("127.0.0.1:" + std::to_string(port), prefix + "/ws", asio::use_awaitable);
   socket.set_option(beast::websocket::stream_base::decorator{[](auto&) {}});
   token.clear();
   socket.binary(true);

   auto hello = core::session_hello{};
   hello.capabilities = forge::api::websocket::binding_capabilities();
   hello.limits = {.max_frame_bytes = max_frame_bytes, .max_item_bytes = max_item_bytes,
                   .max_inflight_calls = 4, .initial_window_items = 2, .initial_window_bytes = 8192,
                   .max_buffered_bytes = 256 * 1024, .idle_timeout_ms = 5000, .shutdown_grace_ms = 1000};
   co_await send(socket, {.kind = core::frame_kind::session_hello, .codec = {.value = "forge.raw"},
                          .payload = forge::raw::pack(hello)});
   const auto greeting = co_await receive(socket);
   require(greeting.kind == core::frame_kind::session_hello && greeting.id.value == 0 &&
           greeting.api.id.value.empty() && greeting.method.empty() && greeting.meta.empty() &&
           greeting.codec.value == "forge.raw");
   const auto server_hello = forge::raw::unpack_exact<core::session_hello>(greeting.payload);
   require(server_hello.version.major == hello.version.major && server_hello.codec == hello.codec &&
           server_hello.capabilities.supports(core::capability::unary) &&
           server_hello.capabilities.supports(core::capability::server_stream) &&
           server_hello.capabilities.supports(core::capability::stream_window));

   co_await send(socket, {.kind = core::frame_kind::request, .id = {.value = 1}, .api = api::ref(),
                          .method = "who", .codec = {.value = "forge.raw"},
                          .payload = forge::raw::pack(spoof(evidence))});
   const auto unary = co_await receive(socket);
   check_frame(unary, 1, "who");
   require(unary.kind == core::frame_kind::response);
   accepted(forge::raw::unpack_exact<output>(unary.payload));

   co_await send(socket, {.kind = core::frame_kind::request, .id = {.value = 2}, .api = api::ref(),
                          .method = "watch", .codec = {.value = "forge.raw"},
                          .payload = forge::raw::pack(spoof(evidence))});
   co_await send(socket, {.kind = core::frame_kind::stream_window, .id = {.value = 2}, .api = api::ref(),
                          .method = "watch", .codec = {.value = "forge.raw"},
                          .payload = forge::raw::pack(core::stream_window{
                             .direction = core::stream_direction::output, .max_items = 2, .max_bytes = 8192})});
   const auto item = co_await receive(socket);
   check_frame(item, 2, "watch");
   require(item.kind == core::frame_kind::stream_item);
   accepted(forge::raw::unpack_exact<output>(item.payload));
   const auto end = co_await receive(socket);
   check_frame(end, 2, "watch");
   require(end.kind == core::frame_kind::stream_end &&
           forge::raw::unpack_exact<core::stream_end>(end.payload).direction == core::stream_direction::output);
   const auto terminal = co_await receive(socket);
   check_frame(terminal, 2, "watch");
   require(terminal.kind == core::frame_kind::response && terminal.payload.empty());
   co_await socket.async_close(beast::websocket::close_code::normal, asio::use_awaitable);
}

asio::awaitable<void> shutdown(http::server* server, std::shared_ptr<verifier> jwt,
                                std::shared_ptr<verifier> introspection, forge::asio::compute::pool& pool) {
   if (jwt) {
      jwt->request_stop();
   }
   if (introspection) {
      introspection->request_stop();
   }
   if (server) {
      co_await server->async_stop();
   }
   if (jwt) {
      co_await jwt->async_drain();
   }
   if (introspection) {
      co_await introspection->async_drain();
   }
   co_await pool.shutdown();
}

} // namespace

int run_driver(int argc, char** argv) {
   private_output();
   const auto ca = configuration(argc, argv);
   auto runtime = forge::asio::runtime{{.worker_threads = 2}};
   auto pool = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 8,
                                          .max_waiting_submissions = 0}};
   auto watchdog = asio::steady_timer{runtime.context()};
   auto watchdog_joined = std::make_shared<std::promise<void>>();
   auto watchdog_done = watchdog_joined->get_future();
   watchdog.expires_after(180s);
   watchdog.async_wait([watchdog_joined](const boost::system::error_code& error) {
      if (!error) {
         fail("FAIL/hard-deadline\n");
         std::_Exit(124);
      }
      watchdog_joined->set_value();
   });

   auto jwt = std::shared_ptr<verifier>{};
   auto introspection = std::shared_ptr<verifier>{};
   auto server = std::unique_ptr<http::server>{};
   auto jwt_registry = core::registry{};
   auto introspection_registry = core::registry{};
   auto stage = std::string_view{"FAIL/initial-input\n"};
   try {
      auto initial_line = read_line(30s);
      auto initial = parse_record<initial_record>(initial_line);
      initial_line.clear();
      require(initial.phase == "initial" && bounded_secret(initial.token, max_token_bytes) &&
              bounded_secret(initial.introspection_client_secret, 4096));
      auto token = secret{std::move(initial.token)};
      stage = "FAIL/configuration\n";
      auto options = jwt_options(ca);
      const auto jwt_policy = options.policy;
      jwt = verifier::create(options, pool.get_executor());
      options.policy.profile = oauth2::token_profile::opaque_introspection;
      options.policy.algorithms.clear();
      options.source = verifier::introspection{
         .endpoint = std::string{issuer} + "/protocol/openid-connect/token/introspect",
         .client_id = audience, .client_secret = secret{std::move(initial.introspection_client_secret)}};
      const auto introspection_policy = options.policy;
      introspection = verifier::create(std::move(options), pool.get_executor());

      stage = "FAIL/jwt-evidence\n";
      const auto evidence = run(runtime, jwt->async_verify(token, jwt_policy));
      require(evidence.client_id() == std::optional<std::string>{workload});
      stage = "FAIL/introspection-evidence\n";
      const auto opaque = run(runtime, introspection->async_verify(token, introspection_policy));
      require(opaque.client_id() == evidence.client_id() && opaque.subject() == evidence.subject());
      jwt_registry.install<api>(std::make_shared<service>(evidence));
      introspection_registry.install<api>(std::make_shared<service>(opaque));
      auto router = http::router{};
      mount(router, jwt_registry, jwt, jwt_policy, "/jwt");
      mount(router, introspection_registry, introspection, introspection_policy, "/introspection");
      server = std::make_unique<http::server>(runtime,
         http::server_config{.bind_address = "127.0.0.1", .max_request_body_bytes = 4096,
                             .max_header_bytes = max_token_bytes + 4096, .read_timeout = 5s,
                             .idle_timeout = 10s, .handshake_timeout = 5s}, std::move(router));
      stage = "FAIL/server-start\n";
      run(runtime, server->async_start());
      const auto port = server->port();
      stage = "FAIL/jwt-http\n";
      run(runtime, http_scenario(runtime, port, "/jwt", token, evidence));
      emit("PASS/jwt-http\n");
      stage = "FAIL/jwt-ws\n";
      run(runtime, websocket_scenario(port, "/jwt", token, evidence));
      emit("PASS/jwt-ws\n");
      stage = "FAIL/introspection-http\n";
      run(runtime, http_scenario(runtime, port, "/introspection", token, opaque));
      emit("PASS/introspection-http\n");
      stage = "FAIL/introspection-ws\n";
      run(runtime, websocket_scenario(port, "/introspection", token, opaque));
      emit("PASS/introspection-ws\n");
      token.clear();
      stage = "FAIL/rotation\n";
      run(runtime, wait_for_refresh_cooldown());
      emit("READY/rotation\n");

      stage = "FAIL/rotation-input\n";
      auto rotation_line = read_line(90s);
      auto rotation = parse_record<rotation_record>(rotation_line);
      rotation_line.clear();
      require(rotation.phase == "rotation" && bounded_secret(rotation.token, max_token_bytes));
      auto rotated = secret{std::move(rotation.token)};
      stage = "FAIL/rotation\n";
      // No recreate or pre-verification: the first HTTP admission must refresh this same warmed verifier.
      run(runtime, http_scenario(runtime, port, "/jwt", rotated, evidence));
      run(runtime, websocket_scenario(port, "/jwt", std::move(rotated), evidence));
      emit("PASS/rotation\n");
   } catch (...) {
      fail(stage);
   }

   try {
      run(runtime, shutdown(server.get(), jwt, introspection, pool));
   } catch (...) {
      fail("FAIL/shutdown\n");
      std::_Exit(124);
   }
   server.reset();
   jwt.reset();
   introspection.reset();
   asio::post(runtime.context(), [&watchdog] { watchdog.cancel(); });
   if (watchdog_done.wait_for(2s) != std::future_status::ready) {
      fail("FAIL/shutdown\n");
      std::_Exit(124);
   }
   watchdog_done.get();
   runtime.stop();
   if (!failed.load()) {
      emit("PASS/shutdown\n");
      return 0;
   }
   return 1;
}

} // namespace auth_resource

int main(int argc, char** argv) {
   try {
      return auth_resource::run_driver(argc, argv);
   } catch (...) {
      auth_resource::fail("FAIL/configuration\n");
      return 1;
   }
}
