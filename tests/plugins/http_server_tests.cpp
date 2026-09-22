#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/describe.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.app.exceptions;
import forge.app.application;
import forge.app.diagnostics;
import forge.app.plugin_context;
import forge.app.plugin;
import forge.app.plugin_registry;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.config.core.decode;
import forge.api.http.binding;
import forge.net.http.base_url;
import forge.api.http.parameters;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.assets;
import forge.net.http.file;
import forge.api.http.mapping;
import forge.net.http.middleware;
import forge.api.http.proxy;
import forge.net.http.router;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.upload;
import forge.plugins.crypto.secrets.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.http.server.types;
import forge.plugins.http.server.exceptions;
import forge.plugins.http.server.middleware;
import forge.plugins.http.server.api;
import forge.plugins.http.server.plugin;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.described;

#include "details/config_assertions.hxx"

using forge::tests::plugins::require_field;

template <typename T>
concept accepts_raw_http_binding = requires(T& api, forge::api::http::binding_plan binding) {
   api.publish(std::move(binding), forge::plugins::http::server::publish_options{});
};

namespace raw_http = forge::net::http;
using raw_http_middleware = raw_http::middleware_descriptor;

template <typename T>
concept accepts_raw_http_middleware =
    requires(T& api, raw_http_middleware descriptor) { api.use(std::move(descriptor)); };

template <typename T>
concept accepts_asset_mount = requires(T& api, raw_http::asset_mount mount) { api.mount_assets(std::move(mount)); };

static_assert(!accepts_raw_http_binding<forge::plugins::http::server::api>);
static_assert(!accepts_raw_http_middleware<forge::plugins::http::server::api>);
static_assert(accepts_asset_mount<forge::plugins::http::server::api>);

[[nodiscard]] bool has_internal_forge_header(const forge::net::http::response& value) {
   for (const auto& header : value.headers()) {
      if (header.name.starts_with("X-FORGE-")) {
         return true;
      }
   }
   return false;
}

struct http_read_request {
   std::string ref;
   std::uint32_t offset = 0;
   std::uint32_t limit = 0;
};

struct http_write_request {
   std::string ref;
   std::string bytes;
};

struct http_chunk {
   std::string bytes;
};

struct http_stream_read_request {
   std::string ref;
};

BOOST_DESCRIBE_STRUCT(http_read_request, (), (ref, offset, limit))
BOOST_DESCRIBE_STRUCT(http_write_request, (), (ref, bytes))
BOOST_DESCRIBE_STRUCT(http_chunk, (), (bytes))
BOOST_DESCRIBE_STRUCT(http_stream_read_request, (), (ref))
namespace plugin_test_contract {

class http_cache_api : public forge::api::core::contract<http_cache_api, forge::api::core::surface::local |
                                                                             forge::api::core::surface::remote> {
 public:
   virtual ~http_cache_api() = default;
   virtual boost::asio::awaitable<http_chunk> read(http_read_request request) = 0;
   virtual boost::asio::awaitable<http_chunk> write(http_write_request request) = 0;
};

class http_stream_api : public forge::api::core::contract<http_stream_api, forge::api::core::surface::local |
                                                                               forge::api::core::surface::remote> {
 public:
   virtual ~http_stream_api() = default;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> download(http_stream_read_request request) = 0;
};

class http_empty_api : public forge::api::core::contract<http_empty_api, forge::api::core::surface::local |
                                                                             forge::api::core::surface::remote> {
 public:
   virtual ~http_empty_api() = default;
   virtual boost::asio::awaitable<forge::api::http::empty_response> clear(http_stream_read_request request) = 0;
};

} // namespace plugin_test_contract

FORGE_API(::plugin_test_contract::http_cache_api, FORGE_API_CONTRACT("cache", 1, 0), FORGE_API_METHOD(read),
          FORGE_API_METHOD(write))
FORGE_API(::plugin_test_contract::http_stream_api, FORGE_API_CONTRACT("stream-cache", 1, 0),
          FORGE_API_METHOD_TYPED(download, ::http_stream_read_request, ::forge::net::http::streaming_response))
FORGE_API(::plugin_test_contract::http_empty_api, FORGE_API_CONTRACT("empty-cache", 1, 0),
          FORGE_API_METHOD_TYPED(clear, ::http_stream_read_request, ::forge::api::http::empty_response))
FORGE_HTTP_API(::plugin_test_contract::http_cache_api,
               FORGE_HTTP_GET(read, "/cache/chunks/:ref?offset={offset}&limit={limit}"),
               FORGE_HTTP_PUT(write, "/cache/chunks/:ref", created))
FORGE_HTTP_API(::plugin_test_contract::http_stream_api,
               FORGE_HTTP_GET(download, "/stream/:ref", FORGE_HTTP_RESPONSE_STREAM))
FORGE_HTTP_API(::plugin_test_contract::http_empty_api, FORGE_HTTP_DELETE(clear, "/empty/:ref", no_content))

namespace {

using plugin_test_contract::http_cache_api;
using plugin_test_contract::http_empty_api;
using plugin_test_contract::http_stream_api;
namespace crypto_secrets = forge::plugins::crypto::secrets;
namespace http_server = forge::plugins::http::server;

class http_cache_api_impl final : public http_cache_api {
 public:
   boost::asio::awaitable<http_chunk> read(http_read_request request) override {
      co_return http_chunk{.bytes = request.ref + ":" + std::to_string(request.offset) + ":" +
                                    std::to_string(request.limit)};
   }

   boost::asio::awaitable<http_chunk> write(http_write_request request) override {
      co_return http_chunk{.bytes = request.ref + ":" + request.bytes};
   }
};

struct http_publish_state {
   std::string base_path;
   std::vector<std::string> middleware_events;
   bool short_circuit = false;
   bool replace_stream_after_next = false;
   bool empty_replace_stream_after_next = false;
   bool set_stream_content_type_after_next = false;
   bool append_cookies_after_next = false;
   std::atomic<unsigned> stream_calls = 0;
   std::atomic<unsigned> stream_chunks = 0;
};

struct http_asset_publish_state {
   std::filesystem::path root;
   std::string mount_path = "/admin";
};

[[nodiscard]] forge::app::application_shell_options http_asset_application_options(bool with_compute) {
   auto options = forge::app::application_shell_options{};
   if (with_compute) {
      options.compute = forge::asio::compute::pool::options{
          .worker_threads = 2,
          .max_pending_tasks = 16,
          .max_waiting_submissions = 16,
          .thread_name = "forge-http-asset-test",
      };
   }
   return options;
}

class http_stream_api_impl final : public http_stream_api {
 public:
   explicit http_stream_api_impl(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

   boost::asio::awaitable<forge::net::http::streaming_response> download(http_stream_read_request request) override {
      state_->stream_calls.fetch_add(1);
      auto chunks =
          std::make_shared<std::vector<std::string>>(std::vector<std::string>{"stream:", request.ref, ":payload"});
      auto index = std::make_shared<std::size_t>(0);
      auto state = state_;
      co_return forge::net::http::streaming_response::from_source(forge::net::http::streaming_response_options{
          .content_type = "text/plain",
          .body = [chunks, index,
                   state]() mutable -> boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> {
             if (*index == chunks->size()) {
                co_return std::nullopt;
             }
             const auto& text = (*chunks)[(*index)++];
             auto bytes = std::vector<std::byte>(text.size());
             std::memcpy(bytes.data(), text.data(), text.size());
             state->stream_chunks.fetch_add(1);
             co_return forge::net::http::body_chunk{.bytes = std::move(bytes)};
          },
      });
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class http_empty_api_impl final : public http_empty_api {
 public:
   boost::asio::awaitable<forge::api::http::empty_response> clear(http_stream_read_request) override {
      co_return forge::api::http::empty_response{.status_code = forge::net::http::status::no_content};
   }
};

class http_cache_publisher_plugin final : public forge::app::plugin {
 public:
   explicit http_cache_publisher_plugin(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http-cache-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      co_await http->publish<http_cache_api>(http_server::publish_options{.base_path = state_->base_path});
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class http_asset_publisher_plugin final : public forge::app::plugin {
 public:
   explicit http_asset_publisher_plugin(std::shared_ptr<http_asset_publish_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http-asset-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      co_await http->mount_assets(raw_http::asset_mount{
          .path = state_->mount_path,
          .root = state_->root,
      });
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_asset_publish_state> state_;
};

class http_stream_publisher_plugin final : public forge::app::plugin {
 public:
   explicit http_stream_publisher_plugin(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http-stream-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      co_await http->publish<http_stream_api>(http_server::publish_options{.base_path = state_->base_path});
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class http_empty_publisher_plugin final : public forge::app::plugin {
 public:
   explicit http_empty_publisher_plugin(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http-empty-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      co_await http->publish<http_empty_api>(http_server::publish_options{.base_path = state_->base_path});
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class http_middleware_plugin final : public forge::app::plugin {
 public:
   explicit http_middleware_plugin(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "http-middleware"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      auto state = state_;
      co_await http->use(http_server::middleware_descriptor{
          .id = "security",
          .phase = http_server::middleware_phase::security,
          .order = 10,
          .path_prefix = "/api",
          .handler = [state](const http_server::middleware_request& request, http_server::middleware_next next)
              -> boost::asio::awaitable<http_server::middleware_response> {
             state->middleware_events.push_back("security");
             if (state->short_circuit && !request.header("Authorization").has_value()) {
                co_return http_server::middleware_response::text(forge::net::http::status::unauthorized,
                                                                 "missing authorization");
             }
             co_return co_await next();
          },
      });
      co_await http->use(http_server::middleware_descriptor{
          .id = "before",
          .phase = http_server::middleware_phase::before_handler,
          .order = 20,
          .path_prefix = "/api",
          .handler = [state](const http_server::middleware_request&, http_server::middleware_next next)
              -> boost::asio::awaitable<http_server::middleware_response> {
             state->middleware_events.push_back("before");
             auto response = co_await next();
             if (state->replace_stream_after_next) {
                co_return http_server::middleware_response::text(forge::net::http::status::forbidden, "blocked");
             }
             if (state->empty_replace_stream_after_next) {
                response.set_status(forge::net::http::status::forbidden);
                response.clear_body();
                co_return response;
             }
             if (state->set_stream_content_type_after_next) {
                response.set_content_type("application/x-ndjson");
             }
             if (state->append_cookies_after_next) {
                response.append_header("Set-Cookie", "session=alpha; Path=/api; HttpOnly");
                response.append_header("Set-Cookie", "csrf=beta; Path=/api; Secure");
             }
             response.set_header("Server", "forge-test");
             co_return response;
          },
      });
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class duplicate_http_cache_publisher_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "duplicate-http-cache-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      auto http = context.apis().get<http_server::api>(http_server::api::ref());
      co_await http->publish<http_cache_api>(http_server::publish_options{.base_path = "/api"});
      co_await http->publish<http_cache_api>(http_server::publish_options{.base_path = "/api"});
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }
};

class late_http_publish_plugin final : public forge::app::plugin {
 public:
   [[nodiscard]] forge::app::plugin_id id() const override {
      return forge::app::plugin_id{.value = "late-http-publisher"};
   }

   [[nodiscard]] std::string version() const override {
      return "1";
   }

   boost::asio::awaitable<void> startup() override {
      auto http = context_->apis().get<http_server::api>(http_server::api::ref());
      co_await http->publish<http_cache_api>(http_server::publish_options{.base_path = "/late"});
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context& context) override {
      context_ = &context;
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   forge::app::plugin_context* context_ = nullptr;
};

class temp_directory {
 public:
   temp_directory() {
      path_ = std::filesystem::temp_directory_path() /
              ("forge-plugin-http-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
      std::filesystem::create_directories(path_);
   }

   ~temp_directory() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
   }

   [[nodiscard]] const std::filesystem::path& path() const noexcept {
      return path_;
   }

   void write(std::string_view name, std::string_view bytes) const {
      auto output = std::ofstream{path_ / std::string{name}, std::ios::binary};
      output << bytes;
   }

 private:
   std::filesystem::path path_;
};

struct http_tls_secret_state {
   forge::tests::p2p::identity_fixture identity = forge::tests::p2p::make_identity_fixture("http-tls-plugin-suite");
   mutable std::mutex mutex;
   std::condition_variable changed;
   std::vector<std::string> purposes;
   bool reject_reads = false;
   bool block_reads = false;
   bool read_blocked = false;
   std::optional<std::string> certificate_override;
   std::optional<std::string> private_key_override;
   std::optional<std::string> client_ca_override;
   std::shared_ptr<boost::asio::steady_timer> blocked_read;
};

class http_tls_secrets_api final : public crypto_secrets::api {
 public:
   explicit http_tls_secrets_api(std::shared_ptr<http_tls_secret_state> state) : state_(std::move(state)) {}

   boost::asio::awaitable<crypto_secrets::snapshot> status(crypto_secrets::query) override {
      co_return crypto_secrets::snapshot{.configured_secrets = 3};
   }

   boost::asio::awaitable<crypto_secrets::get_result> get_bytes(crypto_secrets::get_request request) override {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto material = std::string{};
      auto wait_for_release = false;
      auto release_gate = std::shared_ptr<boost::asio::steady_timer>{};
      {
         const auto lock = std::scoped_lock{state_->mutex};
         if (state_->reject_reads) {
            throw std::runtime_error{"configured HTTP TLS secret read rejected"};
         }
         if (request.secret_id == "http/test-certificate") {
            material = state_->certificate_override.value_or(state_->identity.certificate_pem);
         } else if (request.secret_id == "http/test-private-key") {
            material = state_->private_key_override.value_or(state_->identity.private_key_pem);
         } else if (request.secret_id == "http/test-client-ca") {
            material = state_->client_ca_override.value_or(state_->identity.certificate_pem);
         } else {
            throw std::runtime_error{"unknown HTTP TLS test secret"};
         }
         state_->purposes.push_back(request.purpose);
         if (state_->block_reads) {
            release_gate = std::make_shared<boost::asio::steady_timer>(executor);
            release_gate->expires_at((std::chrono::steady_clock::time_point::max)());
            state_->blocked_read = release_gate;
            state_->read_blocked = true;
            wait_for_release = true;
         }
      }
      state_->changed.notify_all();
      if (wait_for_release) {
         auto error = boost::system::error_code{};
         co_await release_gate->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      auto bytes = decltype(crypto_secrets::get_result{}.bytes){};
      bytes.reserve(material.size());
      for (const auto value : material) {
         bytes.push_back(static_cast<std::uint8_t>(static_cast<unsigned char>(value)));
      }
      co_return crypto_secrets::get_result{.secret_id = std::move(request.secret_id), .bytes = std::move(bytes)};
   }

   boost::asio::awaitable<crypto_secrets::derive_result> derive_hkdf_sha256(crypto_secrets::derive_request) override {
      throw std::logic_error{"HTTP TLS test secrets do not implement derivation"};
   }

   boost::asio::awaitable<crypto_secrets::aead_encrypt_result>
   encrypt_aes_gcm(crypto_secrets::aead_encrypt_request) override {
      throw std::logic_error{"HTTP TLS test secrets do not implement encryption"};
   }

   boost::asio::awaitable<crypto_secrets::aead_decrypt_result>
   decrypt_aes_gcm(crypto_secrets::aead_decrypt_request) override {
      throw std::logic_error{"HTTP TLS test secrets do not implement decryption"};
   }

 private:
   std::shared_ptr<http_tls_secret_state> state_;
};

class http_tls_secrets_plugin final : public forge::app::plugin {
 public:
   explicit http_tls_secrets_plugin(std::shared_ptr<http_tls_secret_state> state) : state_(std::move(state)) {}

   [[nodiscard]] forge::app::plugin_id id() const override {
      return {.value = "forge.plugins.crypto.secrets"};
   }

   [[nodiscard]] std::string version() const override {
      return "test";
   }

   boost::asio::awaitable<void> provide(forge::api::core::provider& provider) override {
      provider.install<crypto_secrets::api>(std::make_shared<http_tls_secrets_api>(state_));
      co_return;
   }

   boost::asio::awaitable<void> initialize(forge::app::plugin_context&) override {
      co_return;
   }

   boost::asio::awaitable<void> startup() override {
      co_return;
   }

   boost::asio::awaitable<void> shutdown() override {
      co_return;
   }

 private:
   std::shared_ptr<http_tls_secret_state> state_;
};

class http_tls_server_application final : public forge::app::application_shell {
 public:
   explicit http_tls_server_application(std::shared_ptr<http_tls_secret_state> state) : state_(std::move(state)) {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = {.value = "forge.plugins.crypto.secrets"},
          .factory = [state = state_] { return std::make_unique<http_tls_secrets_plugin>(state); },
      });
      registry.register_plugin(http_server::descriptor());
   }

 private:
   std::shared_ptr<http_tls_secret_state> state_;
};

[[nodiscard]] bool plugin_tls_request_succeeds(std::uint16_t port) {
   try {
      namespace asio = boost::asio;
      namespace beast = boost::beast;
      namespace beast_http = boost::beast::http;
      auto io = asio::io_context{};
      auto context = asio::ssl::context{asio::ssl::context::tls_client};
      context.set_verify_mode(asio::ssl::verify_none);
      auto stream = beast::ssl_stream<beast::tcp_stream>{beast::tcp_stream{io}, context};
      beast::get_lowest_layer(stream).expires_after(std::chrono::seconds{2});
      beast::get_lowest_layer(stream).connect({asio::ip::make_address("127.0.0.1"), port});
      stream.handshake(asio::ssl::stream_base::client);

      auto request = beast_http::request<beast_http::empty_body>{beast_http::verb::get, "/", 11};
      request.set(beast_http::field::host, "127.0.0.1");
      beast_http::write(stream, request);
      auto buffer = beast::flat_buffer{};
      auto response = beast_http::response<beast_http::string_body>{};
      beast_http::read(stream, buffer, response);
      return response.result() == beast_http::status::not_found;
   } catch (...) {
      return false;
   }
}

class http_server_application final : public forge::app::application_shell {
 public:
   http_server_application(std::shared_ptr<http_publish_state> state, bool middleware = false)
       : state_{std::move(state)}, middleware_{middleware} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      if (middleware_) {
         registry.register_plugin(forge::app::plugin_descriptor{
             .id = forge::app::plugin_id{.value = "http-middleware"},
             .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
             .factory = [state = state_] { return std::make_unique<http_middleware_plugin>(state); },
         });
      }
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-cache-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = state_] { return std::make_unique<http_cache_publisher_plugin>(state); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_cache_api>(http_cache_api::describe(), std::make_shared<http_cache_api_impl>());
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
   bool middleware_ = false;
};

class http_assets_server_application final : public forge::app::application_shell {
 public:
   http_assets_server_application(std::shared_ptr<http_publish_state> publish,
                                  std::shared_ptr<http_asset_publish_state> assets, bool with_compute = true)
       : forge::app::application_shell{http_asset_application_options(with_compute)}, publish_{std::move(publish)},
         assets_{std::move(assets)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-cache-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = publish_] { return std::make_unique<http_cache_publisher_plugin>(state); },
      });
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-asset-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = assets_] { return std::make_unique<http_asset_publisher_plugin>(state); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_cache_api>(http_cache_api::describe(), std::make_shared<http_cache_api_impl>());
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> publish_;
   std::shared_ptr<http_asset_publish_state> assets_;
};

class http_stream_server_application final : public forge::app::application_shell {
 public:
   explicit http_stream_server_application(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-middleware"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = state_] { return std::make_unique<http_middleware_plugin>(state); },
      });
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-stream-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = state_] { return std::make_unique<http_stream_publisher_plugin>(state); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_stream_api>(http_stream_api::describe(),
                                              std::make_shared<http_stream_api_impl>(state_));
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class http_empty_server_application final : public forge::app::application_shell {
 public:
   explicit http_empty_server_application(std::shared_ptr<http_publish_state> state) : state_{std::move(state)} {}

 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-middleware"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = state_] { return std::make_unique<http_middleware_plugin>(state); },
      });
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "http-empty-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [state = state_] { return std::make_unique<http_empty_publisher_plugin>(state); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_empty_api>(http_empty_api::describe(), std::make_shared<http_empty_api_impl>());
      co_return;
   }

 private:
   std::shared_ptr<http_publish_state> state_;
};

class duplicate_http_server_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "duplicate-http-cache-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [] { return std::make_unique<duplicate_http_cache_publisher_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_cache_api>(http_cache_api::describe(), std::make_shared<http_cache_api_impl>());
      co_return;
   }
};

class late_http_server_application final : public forge::app::application_shell {
 protected:
   void on_register_plugins(forge::app::plugin_registry& registry) override {
      registry.register_plugin(http_server::descriptor());
      registry.register_plugin(forge::app::plugin_descriptor{
          .id = forge::app::plugin_id{.value = "late-http-publisher"},
          .dependencies = {forge::app::plugin_id{.value = "forge.plugins.http.server"}},
          .factory = [] { return std::make_unique<late_http_publish_plugin>(); },
      });
   }

   boost::asio::awaitable<void> on_provide(forge::app::application_context& context) override {
      context.apis().install<http_cache_api>(http_cache_api::describe(), std::make_shared<http_cache_api_impl>());
      co_return;
   }
};

[[nodiscard]] std::uint16_t reserve_loopback_port() {
   auto io = boost::asio::io_context{};
   auto acceptor = boost::asio::ip::tcp::acceptor{io};
   acceptor.open(boost::asio::ip::tcp::v4());
   acceptor.bind({boost::asio::ip::make_address("127.0.0.1"), 0});
   const auto port = acceptor.local_endpoint().port();
   acceptor.close();
   return port;
}

} // namespace

BOOST_AUTO_TEST_CASE(http_server_config_is_described_from_schema) {
   auto plugin = http_server::plugin{};
   const auto descriptor = plugin.describe_config();
   BOOST_REQUIRE(descriptor.has_value());
   BOOST_TEST(descriptor->section == "plugins.http.server");

   const auto& bind_address = require_field(*descriptor, "bind-address");
   BOOST_TEST(bind_address.has_default);
   BOOST_TEST(std::get<std::string>(bind_address.default_value.storage) == "127.0.0.1");

   const auto& port = require_field(*descriptor, "port");
   BOOST_TEST(port.has_default);
   BOOST_TEST(std::get<std::uint64_t>(port.default_value.storage) == 0U);

   const auto& base_path = require_field(*descriptor, "api-base-path");
   BOOST_TEST(base_path.has_default);
   BOOST_TEST(std::get<std::string>(base_path.default_value.storage) == "/");

   const auto& body_limit = require_field(*descriptor, "max-request-body-bytes");
   BOOST_TEST(body_limit.has_default);
   BOOST_TEST(std::get<std::uint64_t>(body_limit.default_value.storage) == 16U * 1024U * 1024U);

   BOOST_TEST(require_field(*descriptor, "max-header-bytes").has_default);
   BOOST_TEST(require_field(*descriptor, "read-timeout-ms").has_default);
   BOOST_TEST(require_field(*descriptor, "idle-timeout-ms").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.mode").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.certificate-chain-secret").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.private-key-secret").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.client-ca-secret").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.handshake-timeout-ms").has_default);
   BOOST_TEST(require_field(*descriptor, "tls.max-pending-handshakes").has_default);
   BOOST_TEST(http_server::api::ref().major == 2U);
   BOOST_TEST(plugin.version() == "2.0.0");
}

BOOST_AUTO_TEST_CASE(http_server_rejects_invalid_schema_config) {
   auto plugin = http_server::plugin{};
   auto document = forge::config::core::document{};
   document.set("plugins.http.server.port", std::uint64_t{70000});

   auto runtime = forge::asio::runtime{};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plugin.configure(forge::config::core::component_view{
                                                             document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_invalid_api_base_path_during_configure) {
   auto runtime = forge::asio::runtime{};

   auto empty = http_server::plugin{};
   auto empty_document = forge::config::core::document{};
   empty_document.set("plugins.http.server.api-base-path", std::string{});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, empty.configure(forge::config::core::component_view{
                                                             empty_document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);

   auto relative = http_server::plugin{};
   auto relative_document = forge::config::core::document{};
   relative_document.set("plugins.http.server.api-base-path", std::string{"api"});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, relative.configure(forge::config::core::component_view{
                                                             relative_document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_plaintext_non_loopback_and_incomplete_tls_config) {
   auto runtime = forge::asio::runtime{};

   auto plaintext = http_server::plugin{};
   auto plaintext_document = forge::config::core::document{};
   plaintext_document.set("plugins.http.server.bind-address", std::string{"0.0.0.0"});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, plaintext.configure(forge::config::core::component_view{
                                                             plaintext_document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);

   auto server_tls = http_server::plugin{};
   auto server_tls_document = forge::config::core::document{};
   server_tls_document.set("plugins.http.server.tls.mode", std::string{"server"});
   server_tls_document.set("plugins.http.server.tls.certificate-chain-secret", std::string{"certificate"});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, server_tls.configure(forge::config::core::component_view{
                                                             server_tls_document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);

   auto mutual_tls = http_server::plugin{};
   auto mutual_tls_document = forge::config::core::document{};
   mutual_tls_document.set("plugins.http.server.tls.mode", std::string{"mutual"});
   mutual_tls_document.set("plugins.http.server.tls.certificate-chain-secret", std::string{"certificate"});
   mutual_tls_document.set("plugins.http.server.tls.private-key-secret", std::string{"private-key"});
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, mutual_tls.configure(forge::config::core::component_view{
                                                             mutual_tls_document, "plugins.http.server"})),
                     http_server::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_tls_requires_secrets_api_only_when_tls_is_enabled) {
   auto state = std::make_shared<http_publish_state>();
   auto app = http_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.tls.mode", std::string{"server"});
   config.set("plugins.http.server.tls.certificate-chain-secret", std::string{"http/test-certificate"});
   config.set("plugins.http.server.tls.private-key-secret", std::string{"http/test-private-key"});

   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), http_server::exceptions::startup_failed);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_tls_reload_uses_distinct_secret_purposes_and_preserves_active_context) {
   auto state = std::make_shared<http_tls_secret_state>();
   auto app = http_tls_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{0});
   config.set("plugins.http.server.tls.mode", std::string{"mutual"});
   config.set("plugins.http.server.tls.certificate-chain-secret", std::string{"http/test-certificate"});
   config.set("plugins.http.server.tls.private-key-secret", std::string{"http/test-private-key"});
   config.set("plugins.http.server.tls.client-ca-secret", std::string{"http/test-client-ca"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());
   const auto api = app.apis().get<http_server::api>(http_server::api::ref());
   forge::asio::blocking::run(app.runtime(), api->reload_tls());

   auto purposes = std::vector<std::string>{};
   {
      const auto lock = std::scoped_lock{state->mutex};
      purposes = state->purposes;
   }
   BOOST_REQUIRE_EQUAL(purposes.size(), 6U);
   BOOST_TEST(purposes[0] == "http.server.tls.certificate-chain");
   BOOST_TEST(purposes[1] == "http.server.tls.private-key");
   BOOST_TEST(purposes[2] == "http.server.tls.client-ca");
   BOOST_TEST(purposes[3] == "http.server.tls.certificate-chain");
   BOOST_TEST(purposes[4] == "http.server.tls.private-key");
   BOOST_TEST(purposes[5] == "http.server.tls.client-ca");

   {
      const auto lock = std::scoped_lock{state->mutex};
      state->reject_reads = true;
   }
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), api->reload_tls()),
                     http_server::exceptions::tls_reload_failed);
   {
      const auto lock = std::scoped_lock{state->mutex};
      state->reject_reads = false;
   }
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(app.runtime(), api->reload_tls()));

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_tls_reload_preserves_live_context_and_cannot_publish_after_shutdown) {
   auto state = std::make_shared<http_tls_secret_state>();
   auto app = http_tls_server_application{state};
   const auto port = reserve_loopback_port();
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", static_cast<std::uint64_t>(port));
   config.set("plugins.http.server.tls.mode", std::string{"server"});
   config.set("plugins.http.server.tls.certificate-chain-secret", std::string{"http/test-certificate"});
   config.set("plugins.http.server.tls.private-key-secret", std::string{"http/test-private-key"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());
   const auto api = app.apis().get<http_server::api>(http_server::api::ref());
   BOOST_REQUIRE(plugin_tls_request_succeeds(port));

   {
      const auto lock = std::scoped_lock{state->mutex};
      state->reject_reads = true;
   }
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), api->reload_tls()),
                     http_server::exceptions::tls_reload_failed);
   BOOST_CHECK(plugin_tls_request_succeeds(port));

   {
      const auto lock = std::scoped_lock{state->mutex};
      state->reject_reads = false;
      state->block_reads = true;
      state->read_blocked = false;
      state->blocked_read.reset();
   }
   auto reload = boost::asio::co_spawn(app.runtime().context(), api->reload_tls(), boost::asio::use_future);
   {
      auto lock = std::unique_lock{state->mutex};
      BOOST_REQUIRE(state->changed.wait_for(lock, std::chrono::seconds{2}, [&] { return state->read_blocked; }));
   }

   auto shutdown = boost::asio::co_spawn(app.runtime().context(), app.shutdown(), boost::asio::use_future);
   BOOST_REQUIRE(shutdown.wait_for(std::chrono::seconds{2}) == std::future_status::ready);

   auto blocked_read = std::shared_ptr<boost::asio::steady_timer>{};
   {
      const auto lock = std::scoped_lock{state->mutex};
      state->block_reads = false;
      blocked_read = std::move(state->blocked_read);
   }
   if (blocked_read) {
      blocked_read->cancel();
   }
   BOOST_CHECK_THROW(reload.get(), http_server::exceptions::tls_reload_failed);
   BOOST_CHECK_NO_THROW(shutdown.get());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_tls_reload_rejects_malformed_material_and_keeps_live_context) {
   auto state = std::make_shared<http_tls_secret_state>();
   auto app = http_tls_server_application{state};
   const auto port = reserve_loopback_port();
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", static_cast<std::uint64_t>(port));
   config.set("plugins.http.server.tls.mode", std::string{"server"});
   config.set("plugins.http.server.tls.certificate-chain-secret", std::string{"http/test-certificate"});
   config.set("plugins.http.server.tls.private-key-secret", std::string{"http/test-private-key"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());
   const auto api = app.apis().get<http_server::api>(http_server::api::ref());
   BOOST_REQUIRE(plugin_tls_request_succeeds(port));

   {
      const auto lock = std::scoped_lock{state->mutex};
      state->certificate_override = "not a PEM certificate";
   }
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), api->reload_tls()),
                     http_server::exceptions::tls_reload_failed);
   BOOST_CHECK(plugin_tls_request_succeeds(port));

   const auto other_identity = forge::tests::p2p::make_identity_fixture("http-tls-plugin-mismatched-key");
   {
      const auto lock = std::scoped_lock{state->mutex};
      state->certificate_override.reset();
      state->private_key_override = other_identity.private_key_pem;
   }
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), api->reload_tls()),
                     http_server::exceptions::tls_reload_failed);
   BOOST_CHECK(plugin_tls_request_succeeds(port));

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_publishes_typed_api_under_configured_base_path) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   auto app = http_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{
       app.runtime(), forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port) + "/api")};
   auto cache = forge::asio::blocking::run(app.runtime(), forge::api::http::remote<http_cache_api>(client));
   const auto chunk = forge::asio::blocking::run(
       app.runtime(), cache->read(http_read_request{.ref = "alpha", .offset = 7, .limit = 9}));
   BOOST_TEST(chunk.bytes == "alpha:7:9");

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_preserves_repeated_set_cookie_from_middleware) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->append_cookies_after_next = true;
   auto app = http_server_application{state, true};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   const auto response =
       forge::asio::blocking::run(app.runtime(), client.async_get("/api/cache/chunks/cookie?offset=0&limit=1"));
   auto cookies = std::vector<std::string>{};
   for (const auto& header : response.headers()) {
      if (forge::net::http::header_name_equal(header.name, "Set-Cookie")) {
         cookies.push_back(header.text);
      }
   }
   BOOST_TEST(cookies ==
                  (std::vector<std::string>{"session=alpha; Path=/api; HttpOnly", "csrf=beta; Path=/api; Secure"}),
              boost::test_tools::per_element());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_mounts_assets_without_shadowing_a_narrow_api_prefix) {
   auto files = temp_directory{};
   files.write("index.html", "asset-console");
   const auto port = reserve_loopback_port();
   auto publish = std::make_shared<http_publish_state>();
   auto assets = std::make_shared<http_asset_publish_state>();
   assets->root = files.path();
   auto app = http_assets_server_application{publish, assets};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/admin-ui/v1"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   const auto asset = forge::asio::blocking::run(app.runtime(), client.async_get("/admin/dashboard"));
   BOOST_TEST(static_cast<unsigned>(asset.result()) == static_cast<unsigned>(forge::net::http::status::ok));
   BOOST_TEST(asset.body() == "asset-console");
   const auto api_miss = forge::asio::blocking::run(app.runtime(), client.async_get("/admin-ui/v1/missing"));
   BOOST_TEST(static_cast<unsigned>(api_miss.result()) == static_cast<unsigned>(forge::net::http::status::not_found));

   auto api_client = forge::net::http::client{
       app.runtime(), forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port) + "/admin-ui/v1")};
   auto cache = forge::asio::blocking::run(app.runtime(), forge::api::http::remote<http_cache_api>(api_client));
   const auto chunk = forge::asio::blocking::run(
       app.runtime(), cache->read(http_read_request{.ref = "asset", .offset = 1, .limit = 2}));
   BOOST_TEST(chunk.bytes == "asset:1:2");

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_asset_publication_without_application_compute) {
   auto files = temp_directory{};
   files.write("index.html", "asset-console");
   auto publish = std::make_shared<http_publish_state>();
   auto assets = std::make_shared<http_asset_publish_state>();
   assets->root = files.path();
   auto app = http_assets_server_application{publish, assets, false};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{reserve_loopback_port()});

   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), http_server::exceptions::startup_failed);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_root_api_prefix_overlapping_asset_mount_before_listener_start) {
   auto files = temp_directory{};
   files.write("index.html", "asset-console");
   auto publish = std::make_shared<http_publish_state>();
   auto assets = std::make_shared<http_asset_publish_state>();
   assets->root = files.path();
   auto app = http_assets_server_application{publish, assets};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{reserve_loopback_port()});
   config.set("plugins.http.server.api-base-path", std::string{"/"});

   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), forge::net::http::exceptions::conflict);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_uses_publish_base_path_override) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/custom";
   auto app = http_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{
       app.runtime(), forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port) + "/custom")};
   auto cache = forge::asio::blocking::run(app.runtime(), forge::api::http::remote<http_cache_api>(client));
   const auto chunk =
       forge::asio::blocking::run(app.runtime(), cache->write(http_write_request{.ref = "beta", .bytes = "payload"}));
   BOOST_TEST(chunk.bytes == "beta:payload");

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_applies_middleware_order_and_short_circuit) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->short_circuit = true;
   auto app = http_server_application{state, true};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   const auto denied =
       forge::asio::blocking::run(app.runtime(), client.async_get("/api/cache/chunks/secure?offset=1&limit=1"));
   BOOST_TEST(static_cast<unsigned>(denied.result()) == static_cast<unsigned>(forge::net::http::status::unauthorized));
   BOOST_TEST(state->middleware_events == (std::vector<std::string>{"security"}), boost::test_tools::per_element());

   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::get);
   request.target("/api/cache/chunks/secure?offset=1&limit=1");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");
   const auto allowed = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));
   BOOST_TEST(static_cast<unsigned>(allowed.result()) == static_cast<unsigned>(forge::net::http::status::ok));
   BOOST_TEST(std::string{allowed[forge::net::http::field::server]} == "forge-test");
   BOOST_TEST(state->middleware_events == (std::vector<std::string>{"security", "security", "before"}),
              boost::test_tools::per_element());

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_preserves_stream_framing_through_middleware) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/api";
   auto app = http_stream_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::get);
   request.target("/api/stream/alpha");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");

   const auto response = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));

   BOOST_TEST(static_cast<unsigned>(response.result()) == static_cast<unsigned>(forge::net::http::status::ok));
   BOOST_TEST(response.body() == "stream:alpha:payload");
   BOOST_TEST(std::string{response[forge::net::http::field::server]} == "forge-test");
   const auto has_content_length = response.find(forge::net::http::field::content_length) != response.end();
   BOOST_TEST(!has_content_length);
   BOOST_TEST(std::string{response[forge::net::http::field::transfer_encoding]} == "chunked");
   BOOST_TEST(!has_internal_forge_header(response));
   BOOST_TEST(state->stream_calls.load() == 1U);
   BOOST_TEST(state->stream_chunks.load() == 3U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_stream_middleware_content_type_preserves_stream) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/api";
   state->set_stream_content_type_after_next = true;
   auto app = http_stream_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::get);
   request.target("/api/stream/alpha");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");

   const auto response = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));

   BOOST_TEST(static_cast<unsigned>(response.result()) == static_cast<unsigned>(forge::net::http::status::ok));
   BOOST_TEST(response.body() == "stream:alpha:payload");
   BOOST_TEST(std::string{response[forge::net::http::field::content_type]} == "application/x-ndjson");
   BOOST_TEST(std::string{response[forge::net::http::field::server]} == "forge-test");
   const auto has_content_length = response.find(forge::net::http::field::content_length) != response.end();
   BOOST_TEST(!has_content_length);
   BOOST_TEST(std::string{response[forge::net::http::field::transfer_encoding]} == "chunked");
   BOOST_TEST(!has_internal_forge_header(response));
   BOOST_TEST(state->stream_calls.load() == 1U);
   BOOST_TEST(state->stream_chunks.load() == 3U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_preserves_absent_content_type_through_middleware) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/api";
   auto app = http_empty_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::delete_);
   request.target("/api/empty/alpha");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");

   const auto response = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));

   BOOST_TEST(static_cast<unsigned>(response.result()) == static_cast<unsigned>(forge::net::http::status::no_content));
   BOOST_TEST(response.body().empty());
   const auto has_content_type = response.find(forge::net::http::field::content_type) != response.end();
   BOOST_TEST(!has_content_type);
   BOOST_TEST(!has_internal_forge_header(response));
   BOOST_TEST(std::string{response[forge::net::http::field::server]} == "forge-test");

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_stream_middleware_replacement_does_not_leak_original_body) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/api";
   state->replace_stream_after_next = true;
   auto app = http_stream_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::get);
   request.target("/api/stream/alpha");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");

   const auto response = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));

   BOOST_TEST(static_cast<unsigned>(response.result()) == static_cast<unsigned>(forge::net::http::status::forbidden));
   BOOST_TEST(response.body() == "blocked");
   const auto has_transfer_encoding = response.find(forge::net::http::field::transfer_encoding) != response.end();
   BOOST_TEST(!has_transfer_encoding);
   BOOST_TEST(!has_internal_forge_header(response));
   BOOST_TEST(state->stream_calls.load() == 1U);
   BOOST_TEST(state->stream_chunks.load() == 0U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_empty_stream_middleware_replacement_clears_hidden_token) {
   const auto port = reserve_loopback_port();
   auto state = std::make_shared<http_publish_state>();
   state->base_path = "/api";
   state->empty_replace_stream_after_next = true;
   auto app = http_stream_server_application{state};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});
   config.set("plugins.http.server.api-base-path", std::string{"/api"});

   app.configure(config);
   forge::asio::blocking::run(app.runtime(), app.startup());

   auto client = forge::net::http::client{app.runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};
   auto request = forge::net::http::request{};
   request.method(forge::net::http::method::get);
   request.target("/api/stream/alpha");
   request.version(11);
   request.set(forge::net::http::field::authorization, "Bearer test");

   const auto response = forge::asio::blocking::run(app.runtime(), client.async_request(std::move(request)));

   BOOST_TEST(static_cast<unsigned>(response.result()) == static_cast<unsigned>(forge::net::http::status::forbidden));
   BOOST_TEST(response.body().empty());
   const auto has_transfer_encoding = response.find(forge::net::http::field::transfer_encoding) != response.end();
   BOOST_TEST(!has_transfer_encoding);
   BOOST_TEST(!has_internal_forge_header(response));
   BOOST_TEST(state->stream_calls.load() == 1U);
   BOOST_TEST(state->stream_chunks.load() == 0U);

   forge::asio::blocking::run(app.runtime(), app.shutdown());
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_duplicate_publication_on_startup) {
   const auto port = reserve_loopback_port();
   auto app = duplicate_http_server_application{};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});

   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()), forge::net::http::exceptions::conflict);
}

BOOST_AUTO_TEST_CASE(http_server_plugin_rejects_late_publication_after_startup_closed) {
   const auto port = reserve_loopback_port();
   auto app = late_http_server_application{};
   auto config = forge::config::core::document{};
   config.set("plugins.http.server.port", std::uint64_t{port});

   app.configure(config);
   BOOST_CHECK_THROW(forge::asio::blocking::run(app.runtime(), app.startup()),
                     http_server::exceptions::publication_closed);
}
