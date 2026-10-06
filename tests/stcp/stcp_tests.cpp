#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <cerrno>
#endif

#include <boost/asio/awaitable.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/error_code.hpp>
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "../../libraries/net/stcp/details/handshake_deadline.hxx"

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.crypto.pki.x509;
import forge.net.stcp.connection;
import forge.net.stcp.connector;
import forge.net.stcp.exceptions;
import forge.net.stcp.listener;
import forge.net.stcp.options;
import forge.net.tcp.connector;
import forge.net.tcp.exceptions;
import forge.net.tcp.listener;
import forge.net.transport.buffer;
import forge.net.transport.endpoint;
import forge.net.transport.exceptions;
import forge.net.transport.stream;

#include "../../libraries/net/stcp/details/connection_access.hxx"


namespace {

using bytes = std::vector<std::uint8_t>;

template <typename T> struct spawned_result {
   explicit spawned_result(boost::asio::any_io_executor executor)
       : strand(boost::asio::make_strand(std::move(executor))),
         timer(strand, (std::chrono::steady_clock::time_point::max)()) {}

   boost::asio::strand<boost::asio::any_io_executor> strand;
   boost::asio::steady_timer timer;
   std::optional<T> value;
   std::exception_ptr error;
   bool done = false;
};

template <typename T>
[[nodiscard]] std::shared_ptr<spawned_result<T>> spawn_result(boost::asio::any_io_executor executor,
                                                              boost::asio::awaitable<T> operation) {
   auto state = std::make_shared<spawned_result<T>>(executor);
   boost::asio::co_spawn(
       state->strand,
       [state, operation = std::move(operation)]() mutable -> boost::asio::awaitable<void> {
          try {
             state->value.emplace(co_await std::move(operation));
             state->done = true;
          } catch (...) {
             state->error = std::current_exception();
             state->done = true;
          }
          state->timer.cancel();
       },
       boost::asio::detached);
   return state;
}

template <typename T> boost::asio::awaitable<T> take_result(std::shared_ptr<spawned_result<T>> state) {
   co_return co_await boost::asio::co_spawn(
       state->strand,
       [state = std::move(state)]() -> boost::asio::awaitable<T> {
          auto error = boost::system::error_code{};
          while (!state->done) {
             co_await state->timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
             if (error && error != boost::asio::error::operation_aborted) {
                throw boost::system::system_error{error};
             }
          }
          if (state->error) {
             std::rethrow_exception(state->error);
          }
          co_return std::move(*state->value);
       },
       boost::asio::use_awaitable);
}

class async_start_barrier final : public std::enable_shared_from_this<async_start_barrier> {
 public:
   async_start_barrier(boost::asio::any_io_executor executor, std::size_t target)
       : arrivals_(executor, target), releases_(std::move(executor), target), target_(target) {}

   void start() {
      auto self = shared_from_this();
      boost::asio::co_spawn(
          arrivals_.get_executor(),
          [self = std::move(self)]() -> boost::asio::awaitable<void> {
             for (auto arrived = std::size_t{}; arrived < self->target_; ++arrived) {
                co_await self->arrivals_.async_receive(boost::asio::use_awaitable);
             }
             for (auto released = std::size_t{}; released < self->target_; ++released) {
                co_await self->releases_.async_send(boost::system::error_code{}, boost::asio::use_awaitable);
             }
          },
          boost::asio::detached);
   }

   boost::asio::awaitable<void> arrive_and_wait() {
      co_await arrivals_.async_send(boost::system::error_code{}, boost::asio::use_awaitable);
      co_await releases_.async_receive(boost::asio::use_awaitable);
   }

 private:
   boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> arrivals_;
   boost::asio::experimental::concurrent_channel<void(boost::system::error_code)> releases_;
   std::size_t target_ = 0;
};

struct pem_pair {
   std::string certificate;
   std::string private_key;
};

struct tls_material {
   pem_pair ca;
   pem_pair server;
   pem_pair client;
};

struct evp_pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct bio_deleter {
   void operator()(BIO* value) const noexcept {
      BIO_free(value);
   }
};

using evp_pkey_ptr = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using bio_ptr = std::unique_ptr<BIO, bio_deleter>;

[[nodiscard]] bytes text_bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] forge::net::transport::endpoint loopback(std::uint16_t port) {
   return forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                          .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                          .host = "127.0.0.1",
                                          .port = port};
}

[[nodiscard]] evp_pkey_ptr make_key() {
   auto context = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>{
       EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free};
   BOOST_REQUIRE(context != nullptr);
   BOOST_REQUIRE(EVP_PKEY_keygen_init(context.get()) == 1);
   BOOST_REQUIRE(EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) == 1);
   auto* raw_key = static_cast<EVP_PKEY*>(nullptr);
   BOOST_REQUIRE(EVP_PKEY_keygen(context.get(), &raw_key) == 1);
   auto key = evp_pkey_ptr{raw_key};
   BOOST_REQUIRE(key != nullptr);
   return key;
}

void add_extension(X509* certificate, X509* issuer, int nid, std::string_view value) {
   auto context = X509V3_CTX{};
   X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
   auto* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, std::string{value}.c_str());
   BOOST_REQUIRE(extension != nullptr);
   X509_add_ext(certificate, extension, -1);
   X509_EXTENSION_free(extension);
}

[[nodiscard]] x509_ptr make_certificate(EVP_PKEY* subject_key, std::string_view common_name, std::uint32_t serial,
                                        X509* issuer, EVP_PKEY* issuer_key, bool ca, std::string_view san = {}) {
   auto certificate = x509_ptr{X509_new()};
   BOOST_REQUIRE(certificate != nullptr);
   X509_set_version(certificate.get(), 2);
   ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), static_cast<long>(serial));
   X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60);
   X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 24 * 60 * 60);
   X509_set_pubkey(certificate.get(), subject_key);

   auto* name = X509_get_subject_name(certificate.get());
   X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name.data()),
                              static_cast<int>(common_name.size()), -1, 0);
   if (issuer == nullptr) {
      X509_set_issuer_name(certificate.get(), name);
      issuer = certificate.get();
      issuer_key = subject_key;
   } else {
      X509_set_issuer_name(certificate.get(), X509_get_subject_name(issuer));
   }

   add_extension(certificate.get(), issuer, NID_basic_constraints, ca ? "critical,CA:TRUE" : "CA:FALSE");
   add_extension(certificate.get(), issuer, NID_key_usage,
                 ca ? "critical,keyCertSign,cRLSign" : "digitalSignature,keyEncipherment");
   if (!san.empty()) {
      add_extension(certificate.get(), issuer, NID_subject_alt_name, san);
   }

   BOOST_REQUIRE(X509_sign(certificate.get(), issuer_key, EVP_sha256()) > 0);
   return certificate;
}

[[nodiscard]] std::string write_certificate_pem(X509* certificate) {
   auto bio = bio_ptr{BIO_new(BIO_s_mem())};
   BOOST_REQUIRE(bio != nullptr);
   BOOST_REQUIRE(PEM_write_bio_X509(bio.get(), certificate) == 1);
   auto* data = static_cast<BUF_MEM*>(nullptr);
   BIO_get_mem_ptr(bio.get(), &data);
   return std::string{data->data, data->length};
}

[[nodiscard]] std::string write_private_key_pem(EVP_PKEY* key) {
   auto bio = bio_ptr{BIO_new(BIO_s_mem())};
   BOOST_REQUIRE(bio != nullptr);
   BOOST_REQUIRE(PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) == 1);
   auto* data = static_cast<BUF_MEM*>(nullptr);
   BIO_get_mem_ptr(bio.get(), &data);
   return std::string{data->data, data->length};
}

[[nodiscard]] tls_material make_tls_material() {
   auto ca_key = make_key();
   auto server_key = make_key();
   auto client_key = make_key();

   auto ca_certificate = make_certificate(ca_key.get(), "forge test ca", 1, nullptr, nullptr, true);
   auto server_certificate = make_certificate(server_key.get(), "localhost", 2, ca_certificate.get(), ca_key.get(),
                                              false, "DNS:localhost,IP:127.0.0.1");
   auto client_certificate =
       make_certificate(client_key.get(), "forge test client", 3, ca_certificate.get(), ca_key.get(), false);

   return tls_material{.ca = {.certificate = write_certificate_pem(ca_certificate.get()),
                              .private_key = write_private_key_pem(ca_key.get())},
                       .server = {.certificate = write_certificate_pem(server_certificate.get()),
                                  .private_key = write_private_key_pem(server_key.get())},
                       .client = {.certificate = write_certificate_pem(client_certificate.get()),
                                  .private_key = write_private_key_pem(client_key.get())}};
}

[[nodiscard]] forge::net::stcp::server_options server_options(const tls_material& material, bool verify_peer = false) {
   auto out = forge::net::stcp::server_options{};
   out.certificate_pem = material.server.certificate;
   out.private_key_pem = material.server.private_key;
   out.security.verify_peer = verify_peer;
   out.security.trusted_ca_pem = material.ca.certificate;
   out.alpn_protocols = {"forge-test/1"};
   return out;
}

[[nodiscard]] forge::net::stcp::client_options client_options(const tls_material& material,
                                                              bool with_certificate = false) {
   auto out = forge::net::stcp::client_options{};
   out.security.trusted_ca_pem = material.ca.certificate;
   out.server_name = "localhost";
   out.sni = forge::net::stcp::sni_policy::explicit_name;
   out.alpn_protocols = {"forge-test/1"};
   if (with_certificate) {
      out.certificate_pem = material.client.certificate;
      out.private_key_pem = material.client.private_key;
   }
   return out;
}

struct observed_lower_state {
   std::atomic_size_t cancel_requests = 0;
   std::atomic_size_t close_calls = 0;
   std::atomic_size_t read_calls = 0;
   std::atomic_bool close_completed = false;
   forge::asio::notification model_released;
};

class observed_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   observed_transport_stream(forge::net::transport::stream stream, std::shared_ptr<observed_lower_state> state,
                             std::size_t read_fragment_size)
       : stream_(std::move(stream)), state_(std::move(state)), read_fragment_size_(read_fragment_size) {}

   ~observed_transport_stream() override {
      // This observes final ownership release, not native close completion.
      state_->model_released.notify();
   }

   [[nodiscard]] bool valid() const noexcept override {
      return !cancel_requested_.load(std::memory_order_acquire) && stream_.valid();
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return stream_.id();
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> value) override {
      co_await stream_.async_write(value);
   }

   boost::asio::awaitable<bytes> async_read() override {
      state_->read_calls.fetch_add(1, std::memory_order_release);
      while (pending_.empty()) {
         pending_ = co_await stream_.async_read();
      }
      const auto size = read_fragment_size_ == 0 ? pending_.size() : std::min(read_fragment_size_, pending_.size());
      auto result = bytes{pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(size)};
      pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(size));
      co_return result;
   }

   boost::asio::awaitable<void> async_close() override {
      state_->close_calls.fetch_add(1, std::memory_order_relaxed);
      co_await stream_.async_close();
      state_->close_completed.store(true, std::memory_order_release);
   }

   void cancel() override {
      request_cancel();
   }

   void request_cancel() noexcept {
      if (!cancel_requested_.exchange(true, std::memory_order_acq_rel)) {
         state_->cancel_requests.fetch_add(1, std::memory_order_relaxed);
         stream_.request_cancel();
      }
   }

 private:
   forge::net::transport::stream stream_;
   std::shared_ptr<observed_lower_state> state_;
   bytes pending_;
   std::size_t read_fragment_size_ = 0;
   std::atomic_bool cancel_requested_ = false;
};

class failing_transport_stream final : public forge::net::transport::detail::stream_concept {
 public:
   [[nodiscard]] bool valid() const noexcept override {
      return !canceled_;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return 1;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t>) override {
      co_return;
   }

   boost::asio::awaitable<bytes> async_read() override {
      throw forge::net::transport::exceptions::protocol_error{"synthetic non-terminal transport failure"};
   }

   boost::asio::awaitable<void> async_close() override {
      canceled_ = true;
      co_return;
   }

   void cancel() override {
      canceled_ = true;
   }

 private:
   bool canceled_ = false;
};

[[nodiscard]] forge::net::transport::stream_connection
observe_lower(forge::net::transport::stream_connection connection, std::shared_ptr<observed_lower_state> state,
              std::size_t read_fragment_size, std::weak_ptr<observed_transport_stream>* model = nullptr) {
   auto value = std::make_shared<observed_transport_stream>(std::move(connection.stream), std::move(state),
                                                             read_fragment_size);
   if (model) {
      *model = value;
   }
   auto weak = std::weak_ptr<observed_transport_stream>{value};
   connection.stream = forge::net::transport::detail::stream_access::make_cancelable(
       std::move(value),
       [weak]() noexcept {
          if (const auto locked = weak.lock()) {
             locked->request_cancel();
          }
       },
       [weak]() noexcept {
          if (const auto locked = weak.lock()) {
             locked->request_cancel();
          }
       });
   return connection;
}

boost::asio::awaitable<std::size_t> write_bytes(forge::net::stcp::connection& connection, bytes value) {
   co_await connection.async_write(value);
   co_return value.size();
}

struct raw_tls_observation {
   std::string sni;
};

int capture_sni_callback(SSL* ssl, int*, void* arg) {
   auto* observation = static_cast<raw_tls_observation*>(arg);
   if (const auto* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name)) {
      observation->sni = name;
   }
   return SSL_TLSEXT_ERR_OK;
}

boost::asio::awaitable<raw_tls_observation> accept_raw_tls_once(forge::net::tcp::listener& listener,
                                                                const tls_material& material) {
   namespace asio = boost::asio;
   auto observation = raw_tls_observation{};
   auto tcp = co_await listener.async_accept_connection();
   auto context = asio::ssl::context{asio::ssl::context::tls_server};
   context.use_certificate_chain(asio::buffer(material.server.certificate.data(), material.server.certificate.size()));
   context.use_private_key(asio::buffer(material.server.private_key.data(), material.server.private_key.size()),
                           asio::ssl::context::pem);
   SSL_CTX_set_min_proto_version(context.native_handle(), TLS1_3_VERSION);
   SSL_CTX_set_tlsext_servername_callback(context.native_handle(), capture_sni_callback);
   SSL_CTX_set_tlsext_servername_arg(context.native_handle(), &observation);
   auto stream = asio::ssl::stream<boost::asio::ip::tcp::socket>{std::move(tcp).release_socket(), context};
   auto error = boost::system::error_code{};
   co_await stream.async_handshake(asio::ssl::stream_base::server,
                                   boost::asio::redirect_error(boost::asio::use_awaitable, error));
   if (error) {
      throw boost::system::system_error{error};
   }
   auto ignored = boost::system::error_code{};
   stream.lowest_layer().close(ignored);
   co_return observation;
}

boost::asio::awaitable<void> stcp_direct_roundtrip() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto pinned_client = client_options(material);
   pinned_client.security.expected_sha256_fingerprint =
       forge::crypto::pki::x509::certificate::from_pem(material.server.certificate).fingerprint_sha256_text();
   auto connector = forge::net::stcp::connector{executor, pinned_client};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);

   BOOST_CHECK(client.valid());
   BOOST_CHECK(server.valid());
   BOOST_CHECK_EQUAL(client.selected_alpn(), "forge-test/1");
   BOOST_CHECK_EQUAL(server.selected_alpn(), "forge-test/1");
   BOOST_CHECK(client.peer_certificate().has_value());
   BOOST_CHECK(server.peer_certificate().has_value() == false);

   const auto ping = text_bytes("tls ping");
   co_await client.async_write(ping);
   auto received_ping = co_await server.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_ping.begin(), received_ping.end(), ping.begin(), ping.end());

   auto client_stream = std::move(client).into_transport_stream();
   auto server_stream = std::move(server).into_transport_stream();

   const auto chunk_payload = text_bytes("tls chunk");
   co_await client_stream.stream.async_write(forge::net::transport::chunk{chunk_payload});
   auto received_chunk = co_await server_stream.stream.async_read_chunk();
   const auto received_chunk_bytes = received_chunk.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_chunk_bytes.begin(), received_chunk_bytes.end(), chunk_payload.begin(),
                                 chunk_payload.end());

   const auto framed = text_bytes("tls framed chunk");
   co_await client_stream.stream.async_write_frame(forge::net::transport::chunk{framed});
   auto received_frame = co_await server_stream.stream.async_read_frame_chunk();
   const auto received_frame_bytes = received_frame.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_frame_bytes.begin(), received_frame_bytes.end(), framed.begin(),
                                 framed.end());

   co_await client_stream.stream.async_close();
   co_await server_stream.stream.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_stalled_peer_returns_typed_handshake_timeout(tls_material material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);
   try {
      static_cast<void>(co_await forge::net::stcp::async_upgrade_client(std::move(client_tcp), client_options(material),
                                                                        std::chrono::milliseconds{25}));
      BOOST_FAIL("stalled TCP peer should time out during TLS handshake");
   } catch (const forge::exceptions::base& error) {
      BOOST_REQUIRE(forge::net::stcp::exceptions::code_of(error).has_value());
      BOOST_CHECK(*forge::net::stcp::exceptions::code_of(error) == forge::net::stcp::exceptions::code::timeout);
   }
   co_await server_tcp.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_invalid_handshake_setup_joins_cancel_worker(tls_material material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);

   BOOST_CHECK_THROW(
       (void)co_await forge::net::stcp::async_upgrade_client(
           std::move(client_tcp), client_options(material), std::chrono::milliseconds{0}),
       forge::net::stcp::exceptions::invalid_options);

   // The upgrade owns no live cancel worker after returning its primary setup
   // error, so destruction of its TLS stream must promptly close the peer.
   BOOST_CHECK_THROW((void)co_await server_tcp.async_read(), forge::net::tcp::exceptions::closed);
   co_await server_tcp.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_stalled_handshake_cancel_from_foreign_thread(tls_material material) {
   constexpr auto iteration_count = std::size_t{8};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};

   for (auto iteration = std::size_t{}; iteration < iteration_count; ++iteration) {
      auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
      auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
      auto server_tcp = co_await take_result(accept);
      auto stop = std::stop_source{};
      auto upgrade = spawn_result<forge::net::stcp::connection>(
          executor, forge::net::stcp::async_upgrade_client(std::move(client_tcp), client_options(material),
                                                           std::chrono::seconds{2}, stop.get_token()));

      auto client_hello = bytes(1);
      BOOST_REQUIRE(co_await server_tcp.async_read_some(client_hello) == client_hello.size());

      auto request_won = false;
      auto stop_thread = std::thread{[&] { request_won = stop.request_stop(); }};
      stop_thread.join();
      BOOST_TEST(request_won);

      try {
         static_cast<void>(co_await take_result(upgrade));
         BOOST_FAIL("stalled STCP handshake should be canceled from foreign thread");
      } catch (const forge::net::stcp::exceptions::canceled&) {
      }
      co_await server_tcp.async_close();
   }

   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_completed_handshake_survives_late_foreign_thread_stop(tls_material material) {
   constexpr auto iteration_count = std::size_t{8};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};

   for (auto iteration = std::size_t{}; iteration < iteration_count; ++iteration) {
      auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
      auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
      auto server_tcp = co_await take_result(accept);
      auto stop = std::stop_source{};
      auto request_won = false;
      auto client = client_options(material);
      client.security.verifier = [&](const forge::net::stcp::certificate_chain&) {
         auto stop_thread = std::thread{[&] { request_won = stop.request_stop(); }};
         stop_thread.join();
         return true;
      };

      auto server_upgrade = spawn_result<forge::net::stcp::connection>(
          executor, forge::net::stcp::async_upgrade_server(std::move(server_tcp), server_options(material),
                                                           std::chrono::seconds{2}));
      auto client_connection = co_await forge::net::stcp::async_upgrade_client(
          std::move(client_tcp), std::move(client), std::chrono::seconds{2}, stop.get_token());
      auto server_connection = co_await take_result(server_upgrade);

      BOOST_TEST(request_won);
      BOOST_TEST(client_connection.valid());
      const auto payload = bytes{static_cast<std::uint8_t>(iteration)};
      co_await client_connection.async_write(payload);
      auto received = co_await server_connection.async_read();
      BOOST_TEST(received == payload, boost::test_tools::per_element());

      co_await client_connection.async_close();
      co_await server_connection.async_close();
   }

   co_await listener.async_close();
}

boost::asio::awaitable<std::size_t> read_framed_sequence(forge::net::transport::stream& stream, std::uint8_t marker,
                                                         std::size_t count) {
   for (auto index = std::size_t{}; index < count; ++index) {
      const auto received = co_await stream.async_read_frame();
      const auto expected = bytes{marker, static_cast<std::uint8_t>(index)};
      BOOST_TEST(received == expected, boost::test_tools::per_element());
   }
   co_return count;
}

boost::asio::awaitable<void> stcp_full_duplex_uses_one_connection_strand(tls_material material) {
   constexpr auto frame_count = std::size_t{64};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto client_stream = std::move(client).into_transport_stream();
   auto server_stream = std::move(server).into_transport_stream();

   auto client_reads =
       spawn_result<std::size_t>(executor, read_framed_sequence(client_stream.stream, std::uint8_t{0x5a}, frame_count));
   auto server_reads =
       spawn_result<std::size_t>(executor, read_framed_sequence(server_stream.stream, std::uint8_t{0xa5}, frame_count));

   for (auto index = std::size_t{}; index < frame_count; ++index) {
      co_await client_stream.stream.async_write_frame(bytes{std::uint8_t{0xa5}, static_cast<std::uint8_t>(index)});
      co_await server_stream.stream.async_write_frame(bytes{std::uint8_t{0x5a}, static_cast<std::uint8_t>(index)});
   }

   BOOST_TEST(co_await take_result(client_reads) == frame_count);
   BOOST_TEST(co_await take_result(server_reads) == frame_count);
   co_await client_stream.stream.async_close();
   co_await server_stream.stream.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<bytes> read_exact(forge::net::stcp::connection& connection, std::size_t size) {
   auto out = bytes{};
   out.reserve(size);
   while (out.size() < size) {
      auto next = co_await connection.async_read();
      out.insert(out.end(), next.begin(), next.end());
   }
   co_return out;
}

boost::asio::awaitable<std::size_t> write_after_barrier(forge::net::stcp::connection& connection,
                                                        std::shared_ptr<async_start_barrier> barrier, bytes payload,
                                                        std::size_t index) {
   co_await barrier->arrive_and_wait();
   co_await connection.async_write(payload);
   co_return index;
}

boost::asio::awaitable<void> stcp_same_direction_writes_are_serialized(tls_material material) {
   constexpr auto write_count = std::size_t{16};
   constexpr auto record_size = std::size_t{32};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);

   auto received = spawn_result<bytes>(executor, read_exact(server, write_count * record_size));
   auto barrier = std::make_shared<async_start_barrier>(executor, write_count);
   barrier->start();
   auto writes = std::vector<std::shared_ptr<spawned_result<std::size_t>>>{};
   writes.reserve(write_count);
   for (auto index = std::size_t{}; index < write_count; ++index) {
      writes.push_back(spawn_result<std::size_t>(
          executor,
          write_after_barrier(client, barrier, bytes(record_size, static_cast<std::uint8_t>(index + 1U)), index)));
   }

   auto completed = std::vector<bool>(write_count, false);
   for (auto& write : writes) {
      const auto index = co_await take_result(write);
      BOOST_REQUIRE(index < write_count);
      BOOST_TEST(!completed[index]);
      completed[index] = true;
   }
   const auto wire = co_await take_result(received);
   BOOST_REQUIRE(wire.size() == write_count * record_size);
   auto seen = std::vector<bool>(write_count, false);
   for (auto offset = std::size_t{}; offset < wire.size(); offset += record_size) {
      const auto marker = wire[offset];
      BOOST_REQUIRE(marker >= 1U);
      BOOST_REQUIRE(marker <= write_count);
      BOOST_TEST(std::all_of(wire.begin() + static_cast<std::ptrdiff_t>(offset),
                             wire.begin() + static_cast<std::ptrdiff_t>(offset + record_size),
                             [marker](std::uint8_t value) { return value == marker; }));
      BOOST_TEST(!seen[marker - 1U]);
      seen[marker - 1U] = true;
   }
   BOOST_TEST(std::ranges::all_of(seen, [](bool value) { return value; }));

   co_await client.async_close();
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_cancel_wakes_same_direction_read_waiter(tls_material material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);

   auto first = spawn_result<bytes>(executor, client.async_read());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   auto second = spawn_result<bytes>(executor, client.async_read());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   client.cancel();

   for (const auto& read : {first, second}) {
      try {
         static_cast<void>(co_await take_result(read));
         BOOST_FAIL("stcp read should be canceled");
      } catch (const forge::net::stcp::exceptions::canceled&) {
      }
   }
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_peer_eof_terminalizes_both_io_directions(tls_material material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto client_stream = std::move(client).into_transport_stream();
   auto server_stream = std::move(server).into_transport_stream();

   auto first = spawn_result<bytes>(executor, client_stream.stream.async_read());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   auto queued = spawn_result<bytes>(executor, client_stream.stream.async_read());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);

   co_await server_stream.stream.async_close();
   for (const auto& read : {first, queued}) {
      try {
         static_cast<void>(co_await take_result(read));
         BOOST_FAIL("stcp read should close after peer EOF");
      } catch (const forge::net::stcp::exceptions::closed&) {
      }
   }
   const auto after_eof = text_bytes("after EOF");
   try {
      co_await client_stream.stream.async_write(after_eof);
      BOOST_FAIL("stcp write gate should close after peer EOF");
   } catch (const forge::net::stcp::exceptions::closed&) {
   }
   BOOST_TEST(!client_stream.stream.valid());
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_upgrade_roundtrip() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);

   const auto prelude = text_bytes("clear prelude");
   co_await client_tcp.async_write(prelude);
   auto received_prelude = co_await server_tcp.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_prelude.begin(), received_prelude.end(), prelude.begin(), prelude.end());

   auto server_upgrade = spawn_result<forge::net::stcp::connection>(
       executor, forge::net::stcp::async_upgrade_server(std::move(server_tcp), server_options(material)));
   auto client = co_await forge::net::stcp::async_upgrade_client(std::move(client_tcp), client_options(material));
   auto server = co_await take_result(server_upgrade);

   const auto encrypted = text_bytes("encrypted after upgrade");
   co_await server.async_write(encrypted);
   auto received = co_await client.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), encrypted.begin(), encrypted.end());

   co_await client.async_close();
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_transport_upgrade_preserves_endpoints_lifetime_and_full_duplex() {
   const auto material = make_tls_material();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);

   auto client_state = std::make_shared<observed_lower_state>();
   auto server_state = std::make_shared<observed_lower_state>();
   auto client_model = std::weak_ptr<observed_transport_stream>{};
   auto client_lower = observe_lower(std::move(client_tcp).into_transport_stream(), client_state, 3, &client_model);
   auto server_lower = observe_lower(std::move(server_tcp).into_transport_stream(), server_state, 11);
   client_lower.local_endpoint = {.host_type = forge::net::transport::endpoint::host_kind::dns,
                                  .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
                                  .host = "client-lower.example",
                                  .port = 4101};
   client_lower.remote_endpoint = {.host_type = forge::net::transport::endpoint::host_kind::dns,
                                   .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
                                   .host = "server-lower.example",
                                   .port = 4102};
   server_lower.local_endpoint = client_lower.remote_endpoint;
   server_lower.remote_endpoint = client_lower.local_endpoint;

   auto server_upgrade = spawn_result<forge::net::stcp::connection>(
       executor, forge::net::stcp::async_upgrade_server(std::move(server_lower), server_options(material)));
   auto client = co_await forge::net::stcp::async_upgrade_client(std::move(client_lower), client_options(material));
   auto server = co_await take_result(server_upgrade);

   BOOST_TEST(client.local_endpoint().host == "client-lower.example");
   BOOST_CHECK(client.local_endpoint().protocol == forge::net::transport::endpoint::protocol_kind::quic_v1);
   BOOST_TEST(client.remote_endpoint().host == "server-lower.example");
   BOOST_TEST(server.local_endpoint().host == "server-lower.example");
   BOOST_TEST(server.remote_endpoint().host == "client-lower.example");
   BOOST_TEST(!client_model.expired());

   const auto client_payload = text_bytes("transport client payload");
   const auto server_payload = text_bytes("transport server payload");
   auto client_read = spawn_result<bytes>(executor, read_exact(client, server_payload.size()));
   auto server_read = spawn_result<bytes>(executor, read_exact(server, client_payload.size()));
   auto client_write = spawn_result<std::size_t>(executor, write_bytes(client, client_payload));
   auto server_write = spawn_result<std::size_t>(executor, write_bytes(server, server_payload));

   BOOST_TEST(co_await take_result(client_write) == client_payload.size());
   BOOST_TEST(co_await take_result(server_write) == server_payload.size());
   const auto client_received = co_await take_result(client_read);
   const auto server_received = co_await take_result(server_read);
   BOOST_TEST(client_received == server_payload, boost::test_tools::per_element());
   BOOST_TEST(server_received == client_payload, boost::test_tools::per_element());

   {
      auto handed_off = std::move(client).into_transport_stream();
      const auto handoff_payload = text_bytes("transport handoff payload");
      co_await handed_off.stream.async_write(handoff_payload);
      const auto handoff_received = co_await read_exact(server, handoff_payload.size());
      BOOST_TEST(handoff_received == handoff_payload, boost::test_tools::per_element());
      co_await handed_off.stream.async_close();
      BOOST_TEST(client_state->close_completed.load(std::memory_order_acquire));
      BOOST_TEST(!client_model.expired());
   }
   // A caller-executor post cannot join worker/completion captures on another
   // strand. Await their actual final lower-model release, without polling.
   static_cast<void>(co_await client_state->model_released.async_wait(0));
   BOOST_TEST(client_model.expired());
   BOOST_TEST(client_state->cancel_requests.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(client_state->close_calls.load(std::memory_order_relaxed) == 1U);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_transport_upgrade_timeout_requests_one_lower_cancel() {
   const auto material = make_tls_material();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);
   auto state = std::make_shared<observed_lower_state>();
   auto lower = observe_lower(std::move(client_tcp).into_transport_stream(), state, 0);

   try {
      static_cast<void>(co_await forge::net::stcp::async_upgrade_client(
          std::move(lower), client_options(material), std::optional{std::chrono::milliseconds{25}}));
      BOOST_FAIL("stalled generic transport peer should time out during TLS handshake");
   } catch (const forge::net::stcp::exceptions::timeout&) {
   }
   BOOST_TEST(state->cancel_requests.load(std::memory_order_relaxed) == 1U);
   co_await server_tcp.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_transport_upgrade_preserves_non_terminal_failure() {
   const auto material = make_tls_material();
   auto lower = forge::net::transport::stream_connection{
       .local_endpoint = loopback(4101),
       .remote_endpoint = loopback(4102),
       .stream = forge::net::transport::detail::stream_access::make(
           std::make_shared<failing_transport_stream>()),
   };

   BOOST_CHECK_THROW(
       static_cast<void>(co_await forge::net::stcp::async_upgrade_client(
           std::move(lower), client_options(material), std::optional{std::chrono::milliseconds{1000}})),
       forge::net::stcp::exceptions::handshake_failed);
}

boost::asio::awaitable<void> stcp_transport_upgrade_stop_requests_one_lower_cancel() {
   const auto material = make_tls_material();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);
   auto state = std::make_shared<observed_lower_state>();
   auto lower = observe_lower(std::move(client_tcp).into_transport_stream(), state, 0);
   auto stop = std::stop_source{};
   auto upgrade = spawn_result<forge::net::stcp::connection>(
       executor, forge::net::stcp::async_upgrade_client(std::move(lower), client_options(material),
                                                         std::optional{std::chrono::milliseconds{2000}},
                                                         stop.get_token()));

   auto client_hello = bytes(1);
   BOOST_REQUIRE(co_await server_tcp.async_read_some(client_hello) == client_hello.size());
   BOOST_REQUIRE(stop.request_stop());
   BOOST_CHECK_THROW((void)co_await take_result(upgrade), forge::net::stcp::exceptions::canceled);
   BOOST_TEST(state->cancel_requests.load(std::memory_order_relaxed) == 1U);
   co_await server_tcp.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_transport_upgrade_cancel_closes_lower_once() {
   const auto material = make_tls_material();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::tcp::connector{executor};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_tcp = co_await take_result(accept);
   auto client_state = std::make_shared<observed_lower_state>();
   auto client_lower = observe_lower(std::move(client_tcp).into_transport_stream(), client_state, 0);
   auto server_lower = std::move(server_tcp).into_transport_stream();

   auto server_upgrade = spawn_result<forge::net::stcp::connection>(
       executor, forge::net::stcp::async_upgrade_server(std::move(server_lower), server_options(material)));
   auto client = co_await forge::net::stcp::async_upgrade_client(std::move(client_lower), client_options(material));
   auto server = co_await take_result(server_upgrade);
   auto pending_read = spawn_result<bytes>(executor, client.async_read());
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   client.cancel();
   BOOST_CHECK_THROW((void)co_await take_result(pending_read), forge::net::stcp::exceptions::canceled);
   co_await client.async_close();
   BOOST_TEST(client_state->cancel_requests.load(std::memory_order_relaxed) == 1U);
   BOOST_TEST(client_state->close_calls.load(std::memory_order_relaxed) == 1U);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_verification_failures() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   {
      auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
      auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
      auto client = client_options(material);
      client.server_name = "wrong.local";
      auto connector = forge::net::stcp::connector{executor, client};
      BOOST_CHECK_THROW((void)co_await connector.async_connect_connection(listener.local_endpoint()),
                        forge::net::stcp::exceptions::verification_failed);
      listener.cancel();
      try {
         (void)co_await take_result(accept);
      } catch (...) {
      }
   }

   {
      auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
      auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
      const auto wrong_ca = make_tls_material();
      auto client = client_options(material);
      client.security.trusted_ca_pem = wrong_ca.ca.certificate;
      auto connector = forge::net::stcp::connector{executor, client};
      BOOST_CHECK_THROW((void)co_await connector.async_connect_connection(listener.local_endpoint()),
                        forge::net::stcp::exceptions::verification_failed);
      listener.cancel();
      try {
         (void)co_await take_result(accept);
      } catch (...) {
      }
   }

   {
      auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
      auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
      auto client = client_options(material);
      client.security.expected_sha256_fingerprint = std::string(64, '0');
      auto connector = forge::net::stcp::connector{executor, client};
      BOOST_CHECK_THROW((void)co_await connector.async_connect_connection(listener.local_endpoint()),
                        forge::net::stcp::exceptions::verification_failed);
      listener.cancel();
      try {
         (void)co_await take_result(accept);
      } catch (...) {
      }
   }
}

boost::asio::awaitable<void> stcp_mutual_tls() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material, true)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material, true)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   BOOST_CHECK(server.peer_certificate().has_value());
   co_await client.async_close();
   co_await server.async_close();
   co_await listener.async_close();

   auto default_paths_server = server_options(material, true);
   default_paths_server.security.trusted_ca_pem.clear();
   auto default_paths_listener = forge::net::stcp::listener{executor, loopback(0), std::move(default_paths_server)};
   auto default_paths_accept =
       spawn_result<forge::net::stcp::connection>(executor, default_paths_listener.async_accept_connection());
   auto default_paths_client = forge::net::stcp::connector{executor, client_options(material, true)};
   try {
      (void)co_await default_paths_client.async_connect_connection(default_paths_listener.local_endpoint());
   } catch (const forge::net::stcp::exceptions::handshake_failed&) {
   }
   BOOST_CHECK_THROW((void)co_await take_result(default_paths_accept), forge::net::stcp::exceptions::handshake_failed);
   co_await default_paths_listener.async_close();

   auto rejecting_listener = forge::net::stcp::listener{executor, loopback(0), server_options(material, true)};
   auto rejecting_accept =
       spawn_result<forge::net::stcp::connection>(executor, rejecting_listener.async_accept_connection());
   auto no_cert_connector = forge::net::stcp::connector{executor, client_options(material, false)};
   auto no_cert_client = std::optional<forge::net::stcp::connection>{};
   try {
      no_cert_client.emplace(co_await no_cert_connector.async_connect_connection(rejecting_listener.local_endpoint()));
   } catch (const forge::net::stcp::exceptions::handshake_failed&) {
   }
   BOOST_CHECK_THROW((void)co_await take_result(rejecting_accept), forge::net::stcp::exceptions::handshake_failed);
   if (no_cert_client) {
      co_await no_cert_client->async_close();
   }
   co_await rejecting_listener.async_close();

   const auto untrusted_material = make_tls_material();
   auto strict_server = server_options(material, true);
   auto untrusted_listener = forge::net::stcp::listener{executor, loopback(0), std::move(strict_server)};
   auto untrusted_accept =
       spawn_result<forge::net::stcp::connection>(executor, untrusted_listener.async_accept_connection());
   auto untrusted_client = client_options(material);
   untrusted_client.certificate_pem = untrusted_material.client.certificate;
   untrusted_client.private_key_pem = untrusted_material.client.private_key;
   auto untrusted_connector = forge::net::stcp::connector{executor, std::move(untrusted_client)};
   try {
      (void)co_await untrusted_connector.async_connect_connection(untrusted_listener.local_endpoint());
   } catch (const forge::net::stcp::exceptions::handshake_failed&) {
   }
   BOOST_CHECK_THROW((void)co_await take_result(untrusted_accept), forge::net::stcp::exceptions::handshake_failed);
   co_await untrusted_listener.async_close();

   auto application_server = server_options(material, false);
   application_server.security.require_peer_certificate = true;
   auto application_listener = forge::net::stcp::listener{executor, loopback(0), std::move(application_server)};
   auto application_accept =
       spawn_result<forge::net::stcp::connection>(executor, application_listener.async_accept_connection());
   auto application_client_options = client_options(material);
   application_client_options.certificate_pem = untrusted_material.client.certificate;
   application_client_options.private_key_pem = untrusted_material.client.private_key;
   auto application_connector = forge::net::stcp::connector{executor, std::move(application_client_options)};
   auto application_client =
       co_await application_connector.async_connect_connection(application_listener.local_endpoint());
   auto application_connection = co_await take_result(application_accept);
   BOOST_CHECK(application_connection.peer_certificate().has_value());
   co_await application_client.async_close();
   co_await application_connection.async_close();
   co_await application_listener.async_close();
}

boost::asio::awaitable<void> stcp_sni_policy() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   {
      auto listener = forge::net::tcp::listener{executor, loopback(0)};
      auto accept = spawn_result<raw_tls_observation>(executor, accept_raw_tls_once(listener, material));
      auto connector = forge::net::tcp::connector{executor};
      auto tcp = co_await connector.async_connect_connection(listener.local_endpoint());
      auto options = forge::net::stcp::client_options{};
      options.security.verify_peer = false;
      options.sni = forge::net::stcp::sni_policy::disabled;
      auto tls = co_await forge::net::stcp::async_upgrade_client(std::move(tcp), options);
      const auto observation = co_await take_result(accept);
      BOOST_TEST(observation.sni.empty());
      co_await tls.async_close();
      co_await listener.async_close();
   }

   {
      auto listener = forge::net::tcp::listener{executor, loopback(0)};
      auto accept = spawn_result<raw_tls_observation>(executor, accept_raw_tls_once(listener, material));
      auto connector = forge::net::tcp::connector{executor};
      auto tcp = co_await connector.async_connect_connection(listener.local_endpoint());
      auto options = forge::net::stcp::client_options{};
      options.security.verify_peer = false;
      options.sni = forge::net::stcp::sni_policy::explicit_name;
      options.server_name = "libp2p.example";
      auto tls = co_await forge::net::stcp::async_upgrade_client(std::move(tcp), options);
      const auto observation = co_await take_result(accept);
      BOOST_TEST(observation.sni == "libp2p.example");
      co_await tls.async_close();
      co_await listener.async_close();
   }
}

boost::asio::awaitable<void> stcp_verifier_receives_certificate_chain() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   auto server = server_options(material, false);
   server.security.require_peer_certificate = true;
   auto saw_chain = std::make_shared<bool>(false);
   auto seen_size = std::make_shared<std::size_t>(0);
   server.security.verifier = [saw_chain, seen_size](const forge::net::stcp::certificate_chain& chain) {
      *saw_chain = true;
      *seen_size = chain.certificates.size();
      return !chain.certificates.empty() && !chain.certificates.front().der.empty();
   };

   auto listener = forge::net::stcp::listener{executor, loopback(0), server};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material, true)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_connection = co_await take_result(accept);

   BOOST_TEST(*saw_chain);
   BOOST_TEST(*seen_size >= 1U);
   BOOST_TEST(server_connection.peer_certificate_chain().certificates.size() == *seen_size);

   co_await client.async_close();
   co_await server_connection.async_close();
   co_await listener.async_close();

   auto rejecting_server = server_options(material, false);
   rejecting_server.security.require_peer_certificate = true;
   auto rejected = std::make_shared<bool>(false);
   rejecting_server.security.verifier = [rejected](const forge::net::stcp::certificate_chain&) {
      *rejected = true;
      return false;
   };

   auto rejecting_listener = forge::net::stcp::listener{executor, loopback(0), std::move(rejecting_server)};
   auto rejecting_accept =
       spawn_result<forge::net::stcp::connection>(executor, rejecting_listener.async_accept_connection());
   auto rejecting_connector = forge::net::stcp::connector{executor, client_options(material, true)};
   auto rejecting_client = std::optional<forge::net::stcp::connection>{};
   try {
      rejecting_client.emplace(
          co_await rejecting_connector.async_connect_connection(rejecting_listener.local_endpoint()));
   } catch (const forge::exceptions::base& error) {
      BOOST_CHECK(forge::net::stcp::exceptions::code_of(error).has_value());
   }
   BOOST_CHECK_THROW((void)co_await take_result(rejecting_accept), forge::net::stcp::exceptions::verification_failed);
   BOOST_CHECK(*rejected);
   if (rejecting_client) {
      co_await rejecting_client->async_close();
   }
   co_await rejecting_listener.async_close();
}

boost::asio::awaitable<void> stcp_alpn_uses_client_preference() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   auto server = server_options(material);
   server.alpn_protocols = {"muxer2", "/yamux/1.0.0", "libp2p"};
   auto client = client_options(material);
   client.alpn_protocols = {"/yamux/1.0.0", "muxer2", "libp2p"};

   auto listener = forge::net::stcp::listener{executor, loopback(0), server};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client};
   auto client_connection = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_connection = co_await take_result(accept);

   BOOST_TEST(client_connection.selected_alpn() == "/yamux/1.0.0");
   BOOST_TEST(server_connection.selected_alpn() == "/yamux/1.0.0");

   co_await client_connection.async_close();
   co_await server_connection.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_alpn_no_match_does_not_select_a_protocol() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;

   auto server = server_options(material);
   server.alpn_protocols = {"server-only"};
   auto client = client_options(material);
   client.alpn_protocols = {"client-only"};

   auto listener = forge::net::stcp::listener{executor, loopback(0), std::move(server)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, std::move(client)};
   auto client_connection = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server_connection = co_await take_result(accept);

   BOOST_TEST(client_connection.selected_alpn().empty());
   BOOST_TEST(server_connection.selected_alpn().empty());

   co_await client_connection.async_close();
   co_await server_connection.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_connection_cancel_unblocks_pending_read() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);

   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&server, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          server.cancel();
       },
       boost::asio::detached);

   try {
      (void)co_await server.async_read();
      BOOST_FAIL("stcp read should be canceled");
   } catch (const forge::net::stcp::exceptions::canceled&) {
   }

   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_active_read_rejects_transport_handoff_scenario() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto pending_read = std::make_shared<spawned_result<bytes>>(executor);
   boost::asio::co_spawn(
       executor,
       [pending_read, &server]() -> boost::asio::awaitable<void> {
          try {
             pending_read->value.emplace(co_await server.async_read());
             pending_read->done = true;
          } catch (...) {
             pending_read->error = std::current_exception();
             pending_read->done = true;
          }
          pending_read->timer.cancel();
       },
       boost::asio::detached);

   // With one worker, this FIFO barrier runs after async_read claims its reservation and suspends on native I/O.
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_CHECK_THROW((void)std::move(server).into_transport_stream(), forge::net::stcp::exceptions::io_error);
   BOOST_TEST(server.valid());

   server.cancel();
   try {
      static_cast<void>(co_await take_result(pending_read));
      BOOST_FAIL("pending STCP read should be canceled");
   } catch (const forge::net::stcp::exceptions::canceled&) {
   }
   BOOST_CHECK_THROW((void)std::move(server).into_transport_stream(), forge::net::stcp::exceptions::closed);

   co_await server.async_close();
   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_stream_request_cancel_before_terminal_worker_arms_is_sticky(
    const tls_material& material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto handed_off = std::move(client).into_transport_stream();

   // The notification must retain this request before its worker first runs.
   handed_off.stream.request_cancel();
   BOOST_TEST(!handed_off.stream.valid());
   co_await handed_off.stream.async_close();

   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_connection_cancel_before_terminal_worker_arms_is_sticky(
    const tls_material& material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto peer_read = spawn_result<bytes>(executor, server.async_read());

   // No suspension occurs between publication and cancellation, so the owner
   // worker must consume the sticky request when it first runs.
   client.cancel();
   BOOST_TEST(!client.valid());
   co_await client.async_close();

   BOOST_CHECK_THROW((void)co_await take_result(peer_read), forge::net::stcp::exceptions::closed);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_transport_stream_destruction_completes_owned_terminal_worker_scenario(
    const tls_material& material) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto peer_read = spawn_result<bytes>(executor, server.async_read());

   {
      auto handed_off = std::move(client).into_transport_stream();
      BOOST_TEST(handed_off.stream.valid());
   }

   BOOST_CHECK_THROW((void)co_await take_result(peer_read),
                     forge::net::stcp::exceptions::closed);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_precommit_handoff_failure_preserves_connection_scenario() {
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await take_result(accept);
   auto pending_read = std::make_shared<spawned_result<bytes>>(executor);
   boost::asio::co_spawn(
       executor,
       [pending_read, &server]() -> boost::asio::awaitable<void> {
          try {
             pending_read->value.emplace(co_await server.async_read());
             pending_read->done = true;
          } catch (...) {
             pending_read->error = std::current_exception();
             pending_read->done = true;
          }
          pending_read->timer.cancel();
       },
       boost::asio::detached);

   // With one worker, this FIFO barrier runs after async_read claims its reservation and suspends on native I/O.
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   BOOST_CHECK_THROW((void)std::move(server).into_transport_stream(), forge::net::stcp::exceptions::io_error);
   BOOST_TEST(server.valid());

   const auto payload = text_bytes("handoff survived precommit failure");
   co_await client.async_write(payload);
   const auto received = co_await take_result(pending_read);
   BOOST_TEST(received == payload, boost::test_tools::per_element());

   auto handed_off = std::move(server).into_transport_stream();
   BOOST_TEST(handed_off.stream.valid());
   const auto reply = text_bytes("handoff committed after retry");
   co_await handed_off.stream.async_write(reply);
   const auto client_received = co_await client.async_read();
   BOOST_TEST(client_received == reply, boost::test_tools::per_element());

   co_await handed_off.stream.async_close();
   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_cancel_racing_handoff_has_one_terminal_owner(tls_material material) {
   constexpr auto race_iterations = std::size_t{64};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto connector = forge::net::stcp::connector{executor, client_options(material)};
   auto successful_handoffs = std::size_t{};

   for (auto iteration = std::size_t{}; iteration < race_iterations; ++iteration) {
      auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());
      auto client = co_await connector.async_connect_connection(listener.local_endpoint());
      auto server = co_await take_result(accept);
      auto start = std::barrier{2};
      auto handoff_finished = std::atomic_bool{false};
      auto cancel_thread = std::thread{[&, iteration] {
         start.arrive_and_wait();
         if (iteration == 0) {
            while (!handoff_finished.load(std::memory_order_acquire)) {
               std::this_thread::yield();
            }
         }
         client.cancel();
      }};
      start.arrive_and_wait();

      auto handed_off = std::optional<forge::net::transport::stream_connection>{};
      try {
         handed_off.emplace(std::move(client).into_transport_stream());
      } catch (const forge::net::stcp::exceptions::closed&) {
      }
      handoff_finished.store(true, std::memory_order_release);
      cancel_thread.join();

      if (handed_off) {
         ++successful_handoffs;
         const auto payload = bytes{static_cast<std::uint8_t>(iteration)};
         co_await handed_off->stream.async_write(payload);
         auto received = co_await server.async_read();
         BOOST_TEST(received == payload, boost::test_tools::per_element());
         co_await handed_off->stream.async_close();
      } else {
         BOOST_CHECK_THROW((void)std::move(client).into_transport_stream(), forge::net::stcp::exceptions::closed);
      }
      co_await server.async_close();
   }

   BOOST_TEST(successful_handoffs > 0U);
   co_await listener.async_close();
}

boost::asio::awaitable<void> stcp_rejects_tls12_peer() {
   namespace asio = boost::asio;
   const auto material = make_tls_material();
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::stcp::listener{executor, loopback(0), server_options(material)};
   auto accept = spawn_result<forge::net::stcp::connection>(executor, listener.async_accept_connection());

   auto socket = boost::asio::ip::tcp::socket{executor};
   co_await socket.async_connect(
       boost::asio::ip::tcp::endpoint{boost::asio::ip::make_address("127.0.0.1"), listener.local_endpoint().port},
       boost::asio::use_awaitable);
   auto context = asio::ssl::context{asio::ssl::context::tls_client};
   SSL_CTX_set_max_proto_version(context.native_handle(), TLS1_2_VERSION);
   context.set_verify_mode(asio::ssl::verify_none);
   auto stream = asio::ssl::stream<boost::asio::ip::tcp::socket>{std::move(socket), context};
   auto error = boost::system::error_code{};
   co_await stream.async_handshake(asio::ssl::stream_base::client,
                                   boost::asio::redirect_error(boost::asio::use_awaitable, error));
   BOOST_TEST(error.failed());
   BOOST_CHECK_THROW((void)co_await take_result(accept), forge::net::stcp::exceptions::handshake_failed);
   co_await listener.async_close();
}

struct native_owner_probe {
   std::mutex mutex;
   std::condition_variable changed;
   std::optional<forge::net::stcp::detail::startup_stage> fail_at;
   bool reject_verification = false;
   bool handoff = false;
   bool handoff_used = false;
   bool hold_timer = false;
   bool timer_entered = false;
   bool release_timer = false;
   bool timer_completed = false;
   bool close_entered = false;
   bool release_close = false;
   bool native_open = false;
   bool closed = false;
   bool gate_timed_out = false;
   bool token_released = false;
   bool early_ack = false;
   bool finished = false;
   int descriptor = -1;
   std::exception_ptr primary;
   std::exception_ptr unexpected;
};

boost::asio::awaitable<void> stcp_native_owner_fixture(
    tls_material material, std::shared_ptr<native_owner_probe> probe,
    std::shared_ptr<forge::net::stcp::detail::connection_test_hooks> hooks) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};
   auto accept = spawn_result<forge::net::tcp::connection>(executor, listener.async_accept_connection());
   auto token = std::shared_ptr<void>{new int{1}, [probe](void* value) noexcept {
      const auto lock = std::scoped_lock{probe->mutex};
      probe->early_ack = !probe->closed;
      probe->token_released = true;
      delete static_cast<int*>(value);
      probe->changed.notify_all();
   }};
   auto client_tcp = co_await connector.async_connect_connection(listener.local_endpoint(), {}, std::move(token));
   auto server_tcp = co_await take_result(accept);
   auto server_upgrade = spawn_result<forge::net::stcp::connection>(
       executor, forge::net::stcp::async_upgrade_server(std::move(server_tcp), server_options(material),
                                                        std::chrono::seconds{3}));
   auto options = client_options(material);
   if (probe->reject_verification) {
      options.security.verifier = [](const forge::net::stcp::certificate_chain&) { return false; };
   }
   auto client = forge::net::stcp::connection{};
   try {
      client = co_await forge::net::stcp::detail::connection_access::async_upgrade_client(
          std::move(client_tcp), std::move(options), std::chrono::seconds{3}, std::move(hooks));
   } catch (...) {
      const auto lock = std::scoped_lock{probe->mutex};
      probe->primary = std::current_exception();
   }
   auto server = forge::net::stcp::connection{};
   try {
      server = co_await take_result(server_upgrade);
   } catch (const forge::net::stcp::exceptions::closed&) {
      // Backend allocation can fail before the real server finishes TLS.
   } catch (...) {
      const auto lock = std::scoped_lock{probe->mutex};
      probe->unexpected = std::current_exception();
   }
   if (client.valid()) {
      if (probe->handoff) {
         auto lower = std::move(client).into_transport_stream();
         client = forge::net::stcp::connection{};
         const auto payload = bytes{1, 2, 3};
         co_await lower.stream.async_write(payload);
         const auto received = co_await server.async_read();
         {
            const auto lock = std::scoped_lock{probe->mutex};
            probe->handoff_used = received == payload && !probe->token_released;
         }
         co_await lower.stream.async_close();
      } else {
         co_await client.async_close();
      }
   }
   client = forge::net::stcp::connection{};
   co_await server.async_close();
   co_await listener.async_close();
   co_await connector.async_stop();
}

void stcp_native_owner_holds_real_close(
    std::optional<forge::net::stcp::detail::startup_stage> fail_at, bool reject_verification, bool handoff) {
   using stage = forge::net::stcp::detail::startup_stage;
   using close_stage = forge::net::stcp::detail::native_close_stage;
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto probe = std::make_shared<native_owner_probe>();
   probe->fail_at = fail_at;
   probe->reject_verification = reject_verification;
   probe->handoff = handoff;
   probe->hold_timer = reject_verification || fail_at == stage::owner_allocation || fail_at == stage::terminal_launch;
   auto hooks = std::make_shared<forge::net::stcp::detail::connection_test_hooks>();
   hooks->state = probe;
   hooks->startup = [](void* value, stage current) {
      const auto& state = *static_cast<native_owner_probe*>(value);
      if (state.fail_at == current) {
         throw std::bad_alloc{};
      }
   };
   hooks->timer_callback = [](void* value, const boost::system::error_code&) noexcept {
      auto& state = *static_cast<native_owner_probe*>(value);
      auto lock = std::unique_lock{state.mutex};
      state.timer_entered = true;
      state.changed.notify_all();
      if (state.hold_timer) {
         state.gate_timed_out = !state.changed.wait_for(lock, std::chrono::seconds{5}, [&] {
            return state.release_timer;
         });
      }
      state.timer_completed = true;
      state.changed.notify_all();
   };
   hooks->native_close = [](void* value, const boost::asio::ip::tcp::socket& native, close_stage current) noexcept {
      auto& state = *static_cast<native_owner_probe*>(value);
      auto lock = std::unique_lock{state.mutex};
      if (current == close_stage::before_close) {
         state.native_open = native.is_open();
         state.descriptor = const_cast<boost::asio::ip::tcp::socket&>(native).native_handle();
         state.close_entered = true;
         state.changed.notify_all();
         state.gate_timed_out = state.gate_timed_out || !state.changed.wait_for(
             lock, std::chrono::seconds{5}, [&] { return state.release_close; });
      } else {
         state.closed = !native.is_open();
#if defined(__APPLE__) || defined(__linux__)
         errno = 0;
         state.closed = state.closed && fcntl(state.descriptor, F_GETFD) == -1 && errno == EBADF;
#endif
         state.changed.notify_all();
      }
   };
   auto material = make_tls_material();
   auto worker = std::jthread{[&runtime, probe, hooks, material = std::move(material)]() mutable {
      auto failure = std::exception_ptr{};
      try {
         forge::asio::blocking::run(runtime, stcp_native_owner_fixture(std::move(material), probe, hooks));
      } catch (...) {
         failure = std::current_exception();
      }
      const auto lock = std::scoped_lock{probe->mutex};
      if (failure) {
         probe->unexpected = std::move(failure);
      }
      probe->finished = true;
      probe->changed.notify_all();
   }};
   const auto release = [](native_owner_probe* state) noexcept {
      const auto lock = std::scoped_lock{state->mutex};
      state->release_timer = true;
      state->release_close = true;
      state->changed.notify_all();
   };
   // Both native gates are released before unconditional worker join, including
   // assertion failures. No callback refers to stack-owned probe state.
   auto release_guard = std::unique_ptr<native_owner_probe, decltype(release)>{probe.get(), release};
   {
      auto lock = std::unique_lock{probe->mutex};
      if (probe->hold_timer) {
         BOOST_CHECK(probe->changed.wait_for(lock, std::chrono::seconds{2}, [&] { return probe->timer_entered; }));
         BOOST_CHECK(!probe->close_entered);
         BOOST_CHECK(!probe->token_released);
         BOOST_CHECK(!probe->finished);
         probe->release_timer = true;
         probe->changed.notify_all();
      }
      BOOST_CHECK(probe->changed.wait_for(lock, std::chrono::seconds{2}, [&] { return probe->close_entered; }));
      BOOST_CHECK(probe->native_open);
      BOOST_CHECK(!probe->token_released);
      BOOST_CHECK(!probe->finished);
   }
   release_guard.reset();
   worker.join();
   {
      auto lock = std::unique_lock{probe->mutex};
      BOOST_CHECK(probe->changed.wait_for(lock, std::chrono::seconds{2}, [&] { return probe->token_released; }));
      BOOST_CHECK(probe->closed);
      BOOST_CHECK(!probe->early_ack);
      BOOST_CHECK(!probe->gate_timed_out);
      BOOST_CHECK(!probe->unexpected);
      if (probe->hold_timer) {
         BOOST_CHECK(probe->timer_completed);
      }
      if (handoff) {
         BOOST_CHECK(probe->handoff_used);
         BOOST_CHECK(!probe->primary);
      } else if (probe->primary) {
         if (reject_verification) {
            BOOST_CHECK_THROW(std::rethrow_exception(probe->primary), forge::net::stcp::exceptions::verification_failed);
         } else {
            BOOST_CHECK_THROW(std::rethrow_exception(probe->primary), std::bad_alloc);
         }
      } else {
         BOOST_ERROR("native startup failure must preserve its primary exception");
      }
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(stcp)

BOOST_AUTO_TEST_CASE(stcp_native_startup_failures_keep_owner_until_actual_close_and_timer_drain) {
   using stage = forge::net::stcp::detail::startup_stage;
   stcp_native_owner_holds_real_close(stage::backend_allocation, false, false);
   stcp_native_owner_holds_real_close(stage::owner_allocation, false, false);
   stcp_native_owner_holds_real_close(stage::terminal_launch, false, false);
   stcp_native_owner_holds_real_close(std::nullopt, true, false);
}

BOOST_AUTO_TEST_CASE(stcp_native_tcp_and_stream_handoffs_do_not_acknowledge_socket_close) {
   stcp_native_owner_holds_real_close(std::nullopt, false, true);
}

BOOST_AUTO_TEST_CASE(stcp_loopback_roundtrip_and_transport_stream) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_direct_roundtrip());
}

BOOST_AUTO_TEST_CASE(stcp_upgrades_existing_tcp_connection) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_upgrade_roundtrip());
}

BOOST_AUTO_TEST_CASE(stcp_upgrades_generic_transport_streams_with_full_duplex_and_handoff_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_transport_upgrade_preserves_endpoints_lifetime_and_full_duplex(), std::chrono::seconds{10}));
}

BOOST_AUTO_TEST_CASE(stcp_generic_transport_timeout_requests_one_lower_cancel) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_transport_upgrade_timeout_requests_one_lower_cancel(), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(stcp_generic_transport_preserves_non_terminal_lower_failure) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_transport_upgrade_preserves_non_terminal_failure());
}

BOOST_AUTO_TEST_CASE(stcp_generic_transport_stop_requests_one_lower_cancel) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_transport_upgrade_stop_requests_one_lower_cancel(), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_generic_transport_cancel_closes_lower_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_transport_upgrade_cancel_closes_lower_once(), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_handshake_deadline_has_one_deterministic_terminal_state) {
   using forge::net::stcp::detail::handshake_deadline_state;
   using forge::net::stcp::detail::handshake_terminal_state;

   auto completed = handshake_deadline_state{};
   BOOST_TEST(completed.try_complete());
   BOOST_TEST(!completed.try_timeout());
   BOOST_CHECK(completed.current() == handshake_terminal_state::completed);

   auto timed_out = handshake_deadline_state{};
   BOOST_TEST(timed_out.try_timeout());
   BOOST_TEST(!timed_out.try_complete());
   BOOST_CHECK(timed_out.current() == handshake_terminal_state::timed_out);
}

BOOST_AUTO_TEST_CASE(stcp_stalled_peer_reports_typed_handshake_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_stalled_peer_returns_typed_handshake_timeout(make_tls_material()), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(stcp_handshake_setup_failure_joins_published_cancel_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_invalid_handshake_setup_joins_cancel_worker(make_tls_material()), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(stcp_stalled_handshake_cancel_from_foreign_thread_is_typed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_stalled_handshake_cancel_from_foreign_thread(make_tls_material()), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_late_foreign_thread_stop_does_not_close_handed_off_connection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_completed_handshake_survives_late_foreign_thread_stop(make_tls_material()),
       std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_serializes_full_duplex_ssl_operations_on_multi_worker_runtime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 8}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, stcp_full_duplex_uses_one_connection_strand(make_tls_material()),
                                              std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_serializes_concurrent_same_direction_writes) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 8}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, stcp_same_direction_writes_are_serialized(make_tls_material()),
                                              std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_cancel_wakes_queued_same_direction_read) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_cancel_wakes_same_direction_read_waiter(make_tls_material()), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_peer_eof_wakes_queued_read_and_closes_opposite_direction) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_peer_eof_terminalizes_both_io_directions(make_tls_material()), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_rejects_wrong_hostname_and_fingerprint) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_verification_failures());
}

BOOST_AUTO_TEST_CASE(stcp_supports_mutual_tls) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_mutual_tls());
}

BOOST_AUTO_TEST_CASE(stcp_controls_sni_explicitly) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_sni_policy());
}

BOOST_AUTO_TEST_CASE(stcp_verifier_receives_full_certificate_chain) {
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK(
       forge::asio::blocking::run_for(runtime, stcp_verifier_receives_certificate_chain(), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_alpn_selects_client_preferred_supported_protocol) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_alpn_uses_client_preference());
}

BOOST_AUTO_TEST_CASE(stcp_alpn_no_match_selects_nothing) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_alpn_no_match_does_not_select_a_protocol());
}

BOOST_AUTO_TEST_CASE(stcp_cancel_unblocks_pending_read) {
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, stcp_connection_cancel_unblocks_pending_read(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(stcp_active_read_rejects_transport_handoff_without_moving_stream) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, stcp_active_read_rejects_transport_handoff_scenario(),
                                              std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(stcp_stream_request_cancel_before_terminal_worker_arm_is_sticky) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_stream_request_cancel_before_terminal_worker_arms_is_sticky(make_tls_material()),
       std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_connection_cancel_before_terminal_worker_arm_is_sticky) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_connection_cancel_before_terminal_worker_arms_is_sticky(make_tls_material()),
       std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_transport_stream_destruction_completes_owned_terminal_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_transport_stream_destruction_completes_owned_terminal_worker_scenario(make_tls_material()),
       std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(stcp_precommit_handoff_failure_does_not_cancel_original_connection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_precommit_handoff_failure_preserves_connection_scenario(), std::chrono::seconds{3}));
}

BOOST_AUTO_TEST_CASE(stcp_cancel_racing_handoff_has_one_terminal_owner_and_preserves_handoff_winner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, stcp_cancel_racing_handoff_has_one_terminal_owner(make_tls_material()), std::chrono::seconds{20}));
}

BOOST_AUTO_TEST_CASE(stcp_requires_tls13_by_default) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, stcp_rejects_tls12_peer());
}

BOOST_AUTO_TEST_SUITE_END()
