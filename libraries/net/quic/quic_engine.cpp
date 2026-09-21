#include "details/quic_engine.hxx"
#include "details/acknowledged_ranges.hxx"
#include "details/initial_token.hxx"
#include "details/server_udp_socket.hxx"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <ngtcp2/ngtcp2.h>
#include <ngtcp2/ngtcp2_crypto.h>
#include <ngtcp2/ngtcp2_crypto_ossl.h>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

import forge.crypto.core.random;
import forge.crypto.core.secret_string;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.asio.notification;
import forge.asio.exceptions;
import forge.net.transport.endpoint;
#include "details/engine_client_options.hxx"
#include "details/engine_server_options.hxx"

namespace forge::net::quic::detail {
namespace {

namespace asio = boost::asio;
using udp = asio::ip::udp;

constexpr auto cid_length = std::size_t{8};
constexpr auto max_udp_payload_size = std::size_t{1350};
constexpr auto max_packets_per_drain = std::size_t{64};
constexpr auto max_queued_datagram_bytes = std::size_t{16 * 1024 * 1024};
constexpr auto detached_write_drain_timeout = std::chrono::seconds{5};
constexpr auto stateless_reset_secret_size = std::size_t{32};
constexpr auto retry_token_lifetime = 10 * NGTCP2_SECONDS;
constexpr auto regular_token_lifetime = 60 * 60 * NGTCP2_SECONDS;
constexpr auto max_client_token_bytes = std::size_t{512};

using timer_ptr = std::shared_ptr<asio::steady_timer>;
using stateless_reset_secret = std::array<std::uint8_t, stateless_reset_secret_size>;

[[nodiscard]] ngtcp2_tstamp timestamp() noexcept {
   return static_cast<ngtcp2_tstamp>(
       std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
           .count());
}

[[nodiscard]] std::string openssl_error() {
   auto out = std::string{};
   auto code = 0UL;
   while ((code = ERR_get_error()) != 0) {
      if (!out.empty()) {
         out += "; ";
      }
      out += ERR_error_string(code, nullptr);
   }
   return out.empty() ? "OpenSSL operation failed" : out;
}

[[noreturn]] void throw_engine(engine_error_kind kind, std::string message) {
   throw engine_failure{kind, std::move(message)};
}

[[nodiscard]] std::string ngtcp2_read_error_message(ngtcp2_conn* conn, int rv) {
   auto out = std::string{"ngtcp2_conn_read_pkt failed: "};
   out += ngtcp2_strerror(rv);
   if (conn == nullptr) {
      return out;
   }
   out += "; client_version=0x";
   out += forge::codec::hex::encode(ngtcp2_conn_get_client_chosen_version(conn), 8);
   out += "; negotiated_version=0x";
   out += forge::codec::hex::encode(ngtcp2_conn_get_negotiated_version(conn), 8);
   if (const auto tls_error = ngtcp2_conn_get_tls_error(conn); tls_error != 0) {
      out += "; tls_error=";
      out += std::to_string(tls_error);
      out += " ";
      out += ERR_error_string(static_cast<unsigned long>(tls_error), nullptr);
   }
   if (const auto alert = ngtcp2_conn_get_tls_alert(conn); alert != 0) {
      out += "; tls_alert=";
      out += SSL_alert_desc_string_long(alert);
   }
   const auto* close_error = ngtcp2_conn_get_ccerr(conn);
   if (close_error != nullptr && (close_error->type != NGTCP2_CCERR_TYPE_IDLE_CLOSE || close_error->error_code != 0 ||
                                  close_error->reasonlen != 0)) {
      out += "; close_type=";
      out += std::to_string(static_cast<int>(close_error->type));
      out += "; close_code=";
      out += std::to_string(close_error->error_code);
      if (close_error->frame_type != 0) {
         out += "; close_frame=";
         out += std::to_string(close_error->frame_type);
      }
      if (close_error->reason != nullptr && close_error->reasonlen != 0) {
         out += "; close_reason=";
         out.append(reinterpret_cast<const char*>(close_error->reason), close_error->reasonlen);
      }
   }
   return out;
}

[[nodiscard]] std::chrono::milliseconds remaining_timeout_budget(std::chrono::steady_clock::time_point started,
                                                                 std::chrono::milliseconds timeout) noexcept {
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   if (elapsed >= timeout) {
      return std::chrono::milliseconds{0};
   }
   return timeout - elapsed;
}

[[nodiscard]] bool connect_failpoint_enabled(const engine_client_options& options, std::string_view name) {
   return options.test_failpoint && options.test_failpoint(name);
}

int accept_any_certificate_cb(int, X509_STORE_CTX*) {
   return 1;
}

[[nodiscard]] bool fill_random(std::span<std::uint8_t> bytes) {
   try {
      forge::crypto::core::fill_random(bytes);
      return true;
   } catch (...) {
      return false;
   }
}

[[nodiscard]] stateless_reset_secret random_stateless_reset_secret() {
   auto secret = stateless_reset_secret{};
   if (!fill_random(secret)) {
      throw_engine(engine_error_kind::tls_failed, "failed to generate QUIC stateless reset secret");
   }
   return secret;
}

[[nodiscard]] initial_token_validator::secret random_initial_token_secret() {
   auto secret = initial_token_validator::secret{};
   if (!fill_random(secret)) {
      throw_engine(engine_error_kind::tls_failed, "failed to generate QUIC initial token secret");
   }
   return secret;
}

void rand_cb(std::uint8_t* dest, std::size_t destlen, const ngtcp2_rand_ctx*) {
   (void)fill_random({dest, destlen});
}

int get_new_connection_id_cb(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen,
                             void* user_data);
int remove_connection_id_cb(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data);

[[nodiscard]] std::string cid_key(const ngtcp2_cid& cid) {
   return std::string{reinterpret_cast<const char*>(cid.data), reinterpret_cast<const char*>(cid.data) + cid.datalen};
}

[[nodiscard]] std::string cid_key(const std::uint8_t* data, std::size_t len) {
   return std::string{reinterpret_cast<const char*>(data), reinterpret_cast<const char*>(data) + len};
}

[[nodiscard]] sockaddr_storage to_sockaddr_storage(const udp::endpoint& endpoint) {
   auto storage = sockaddr_storage{};
   // Keep the native scope ID and platform sockaddr length fields intact.
   assert(endpoint.size() <= sizeof(storage));
   std::memcpy(&storage, endpoint.data(), endpoint.size());
   return storage;
}

[[nodiscard]] ngtcp2_addr to_ngtcp2_addr(sockaddr_storage& storage) {
   auto* addr = reinterpret_cast<sockaddr*>(&storage);
   const auto len = addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
   return ngtcp2_addr{.addr = addr, .addrlen = static_cast<socklen_t>(len)};
}

[[nodiscard]] udp::endpoint from_ngtcp2_addr(const ngtcp2_addr& address) {
   auto result = udp::endpoint{};
   if (address.addr == nullptr || address.addrlen > result.capacity() ||
       (address.addr->sa_family != AF_INET && address.addr->sa_family != AF_INET6) ||
       address.addrlen != (address.addr->sa_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6))) {
      throw_engine(engine_error_kind::internal_error, "invalid QUIC output path");
   }
   std::memcpy(result.data(), address.addr, address.addrlen);
   result.resize(address.addrlen);
   return result;
}

[[nodiscard]] forge::net::transport::datagram_io::received copy_route(const ngtcp2_path& path) {
   return {.remote = from_ngtcp2_addr(path.remote), .local = from_ngtcp2_addr(path.local)};
}

struct path_storage {
   sockaddr_storage local_storage{};
   sockaddr_storage remote_storage{};
   ngtcp2_path path{};
};

[[nodiscard]] path_storage make_path(const udp::endpoint& local, const udp::endpoint& remote) {
   auto storage = path_storage{};
   storage.local_storage = to_sockaddr_storage(local);
   storage.remote_storage = to_sockaddr_storage(remote);
   storage.path.local = to_ngtcp2_addr(storage.local_storage);
   storage.path.remote = to_ngtcp2_addr(storage.remote_storage);
   return storage;
}

[[nodiscard]] std::vector<std::uint8_t> length_prefixed_alpn(std::string_view alpn) {
   auto out = std::vector<std::uint8_t>{};
   if (alpn.empty() || alpn.size() > 255) {
      throw_engine(engine_error_kind::invalid_options, "QUIC ALPN must be 1..255 bytes");
   }
   out.push_back(static_cast<std::uint8_t>(alpn.size()));
   out.insert(out.end(), alpn.begin(), alpn.end());
   return out;
}

int select_alpn_cb(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
                   void* arg) {
   const auto& alpn = *static_cast<const std::string*>(arg);
   auto pos = std::size_t{0};
   while (pos < inlen) {
      const auto len = static_cast<std::size_t>(in[pos]);
      ++pos;
      if (pos + len > inlen) {
         return SSL_TLSEXT_ERR_ALERT_FATAL;
      }
      if (len == alpn.size() && std::memcmp(in + pos, alpn.data(), len) == 0) {
         *out = in + pos;
         *outlen = static_cast<unsigned char>(len);
         return SSL_TLSEXT_ERR_OK;
      }
      pos += len;
   }
   return SSL_TLSEXT_ERR_ALERT_FATAL;
}

struct ssl_ctx_deleter {
   void operator()(SSL_CTX* ctx) const noexcept {
      SSL_CTX_free(ctx);
   }
};

struct ssl_deleter {
   void operator()(SSL* ssl) const noexcept {
      if (ssl != nullptr) {
         SSL_set_app_data(ssl, nullptr);
      }
      SSL_free(ssl);
   }
};

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

using ssl_ctx_ptr = std::unique_ptr<SSL_CTX, ssl_ctx_deleter>;
using ssl_ptr = std::unique_ptr<SSL, ssl_deleter>;
using x509_ptr = std::unique_ptr<X509, x509_deleter>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, pkey_deleter>;

[[nodiscard]] x509_ptr load_certificate(std::string_view pem) {
   auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
   if (bio == nullptr) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   auto* cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
   BIO_free(bio);
   if (cert == nullptr) {
      throw_engine(engine_error_kind::tls_failed, "invalid QUIC server certificate: " + openssl_error());
   }
   return x509_ptr{cert};
}

[[nodiscard]] pkey_ptr load_private_key(std::string_view pem) {
   auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
   if (bio == nullptr) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   auto* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
   BIO_free(bio);
   if (key == nullptr) {
      throw_engine(engine_error_kind::tls_failed, "invalid QUIC server private key: " + openssl_error());
   }
   return pkey_ptr{key};
}

void add_trusted_certificate(SSL_CTX* ctx, std::string_view pem) {
   auto* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
   if (bio == nullptr) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   auto bio_guard = std::unique_ptr<BIO, decltype(&BIO_free)>{bio, BIO_free};
   auto* store = SSL_CTX_get_cert_store(ctx);
   if (store == nullptr) {
      throw_engine(engine_error_kind::tls_failed, "failed to access QUIC TLS trust store");
   }

   auto loaded = false;
   while (auto* raw = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
      loaded = true;
      auto certificate = x509_ptr{raw};
      if (X509_STORE_add_cert(store, certificate.get()) != 1) {
         const auto code = ERR_peek_last_error();
         if (ERR_GET_REASON(code) == X509_R_CERT_ALREADY_IN_HASH_TABLE) {
            ERR_clear_error();
            continue;
         }
         throw_engine(engine_error_kind::tls_failed, "failed to add trusted QUIC CA certificate: " + openssl_error());
      }
   }

   const auto code = ERR_peek_last_error();
   if (code != 0 && ERR_GET_REASON(code) != PEM_R_NO_START_LINE) {
      throw_engine(engine_error_kind::tls_failed, "failed to parse trusted QUIC CA certificate: " + openssl_error());
   }
   ERR_clear_error();
   if (!loaded) {
      throw_engine(engine_error_kind::tls_failed, "trusted QUIC CA PEM does not contain a certificate");
   }
}

void configure_default_trust(SSL_CTX* ctx, const engine_security_options& security) {
   if (!security.trusted_ca_pem.empty()) {
      add_trusted_certificate(ctx, security.trusted_ca_pem);
      return;
   }
   if (SSL_CTX_set_default_verify_paths(ctx) != 1) {
      throw_engine(engine_error_kind::tls_failed, "failed to load default QUIC TLS trust paths: " + openssl_error());
   }
}

[[nodiscard]] bool configure_verify_peer_name(SSL* ssl, std::string_view host) {
   auto* params = SSL_get0_param(ssl);
   if (params == nullptr) {
      throw_engine(engine_error_kind::tls_failed, "failed to access QUIC TLS verification parameters");
   }

   const auto peer_name = host.empty() ? std::string{"localhost"} : std::string{host};
   if (X509_VERIFY_PARAM_set1_ip_asc(params, peer_name.c_str()) == 1) {
      return false;
   }
   ERR_clear_error();
   if (SSL_set1_host(ssl, peer_name.c_str()) != 1) {
      throw_engine(engine_error_kind::tls_failed, "failed to bind QUIC TLS peer hostname: " + openssl_error());
   }
   return true;
}

[[nodiscard]] std::vector<std::uint8_t> der_from_certificate(X509* certificate) {
   if (certificate == nullptr) {
      return {};
   }
   const auto len = i2d_X509(certificate, nullptr);
   if (len <= 0) {
      throw_engine(engine_error_kind::tls_failed, "failed to DER-encode peer certificate");
   }
   auto der = std::vector<std::uint8_t>(static_cast<std::size_t>(len));
   auto* out = der.data();
   if (i2d_X509(certificate, &out) != len) {
      throw_engine(engine_error_kind::tls_failed, "failed to DER-encode peer certificate");
   }
   return der;
}

void wake(const std::shared_ptr<asio::steady_timer>& timer) noexcept {
   try {
      asio::dispatch(timer->get_executor(), [timer] {
         try {
            timer->cancel();
         } catch (...) {
         }
      });
   } catch (...) {
   }
}

void wake(std::vector<std::weak_ptr<asio::steady_timer>>& waiters) noexcept {
   auto current = std::move(waiters);
   waiters.clear();
   for (auto& weak : current) {
      if (auto timer = weak.lock()) {
         wake(timer);
      }
   }
}

void remove_waiter(std::vector<std::weak_ptr<asio::steady_timer>>& waiters,
                   const std::shared_ptr<asio::steady_timer>& target) noexcept {
   std::erase_if(waiters, [&target](const auto& weak) {
      const auto timer = weak.lock();
      return !timer || timer == target;
   });
}

[[nodiscard]] engine_endpoint from_udp_endpoint(const udp::endpoint& value) {
   const auto endpoint = forge::net::transport::endpoint::from_address(
       value.address(), value.port(), forge::net::transport::endpoint::protocol_kind::quic_v1);
   return engine_endpoint{.host = endpoint.host,
                          .port = endpoint.port,
                          .family = value.address().is_v4() ? engine_endpoint::address_family::ipv4
                                                            : engine_endpoint::address_family::ipv6,
                          .zone = endpoint.zone};
}

[[nodiscard]] asio::ip::address literal_address(const engine_endpoint& value) {
   using endpoint = forge::net::transport::endpoint;
   try {
      return endpoint{.host_type = value.host.find(':') == std::string::npos ? endpoint::host_kind::ip4
                                                                             : endpoint::host_kind::ip6,
                      .host = value.host,
                      .port = value.port,
                      .zone = value.zone}
          .literal_address();
   } catch (const boost::system::system_error& error) {
      throw_engine(engine_error_kind::invalid_endpoint, error.what());
   }
}

} // namespace

engine_failure::engine_failure(engine_error_kind kind, std::string message)
    : kind_(kind), message_(std::move(message)) {}

engine_error_kind engine_failure::kind() const noexcept {
   return kind_;
}

const char* engine_failure::what() const noexcept {
   return message_.c_str();
}

const std::string& engine_failure::message() const noexcept {
   return message_;
}

std::string normalize_engine_sha256_fingerprint(std::string_view value) {
   auto normalized = std::string{};
   normalized.reserve(value.size());
   for (const auto ch : value) {
      if (ch == ':' || ch == '-' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
         continue;
      }
      if (std::isxdigit(static_cast<unsigned char>(ch)) == 0) {
         throw_engine(engine_error_kind::invalid_options, "invalid SHA-256 fingerprint");
      }
      normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
   }
   if (normalized.size() != 64) {
      throw_engine(engine_error_kind::invalid_options, "SHA-256 fingerprint must contain 32 bytes");
   }
   return normalized;
}

std::string engine_sha256_fingerprint(std::span<const std::uint8_t> data) {
   const auto fingerprint = forge::crypto::digest::sha256::hash(data);
   return forge::codec::hex::encode(fingerprint.to_uint8_span());
}

struct engine_stream::impl {
   struct pending_write {
      std::vector<std::uint8_t> data;
      std::shared_ptr<void> lifetime;
      std::size_t submitted = 0;
      std::uint64_t base_offset = 0;
      bool base_offset_set = false;
      bool fin = false;
      std::vector<std::weak_ptr<asio::steady_timer>> waiters;
   };

   struct retained_write {
      std::vector<std::uint8_t> data;
      std::shared_ptr<void> lifetime;
      std::uint64_t base_offset = 0;
      bool fin = false;
   };

   explicit impl(std::int64_t id_value) : id(id_value) {}

   std::int64_t id = -1;
   std::weak_ptr<engine_connection::impl> connection;
   std::map<std::uint64_t, std::vector<std::uint8_t>> inbound_segments;
   std::deque<std::vector<std::uint8_t>> inbound_ready;
   std::deque<pending_write> outbound;
   std::deque<retained_write> retained;
   acknowledged_ranges acknowledged;
   std::uint64_t recv_next_offset = 0;
   std::uint64_t send_next_offset = 0;
   bool remote_read_closed = false;
   bool remote_read_reset = false;
   bool local_write_closed = false;
   bool fin_queued = false;
   bool reset = false;
   bool reset_counted = false;
   bool closed = false;
   bool cancel_worker_started = false;
   // Strand-owned recovery work is distinct from native stream termination.
   std::size_t terminal_cleanup_owners = 0;
   forge::asio::notification cancel_requested;
   // Owner-strand state is mirrored through atomics so callers can join a
   // terminal recovery without reading the strand-owned booleans.
   std::atomic_bool terminal_published = false;
   std::atomic_bool terminal_cleanup_complete = false;
   forge::asio::notification terminal_notification;
   std::vector<std::weak_ptr<asio::steady_timer>> read_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> write_waiters;
};

void publish_stream_terminal(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
   if (!stream) {
      return;
   }
   stream->terminal_published.store(true, std::memory_order_release);
   stream->terminal_notification.notify();
}

void finish_stream_terminal_cleanup(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
   if (!stream) {
      return;
   }
   publish_stream_terminal(stream);
   if (stream->terminal_cleanup_owners != 0) {
      return;
   }
   stream->terminal_cleanup_complete.store(true, std::memory_order_release);
   stream->terminal_notification.notify();
}

void release_stream_terminal_owner(engine_stream::impl* stream) noexcept {
   assert(stream->terminal_cleanup_owners != 0);
   if (--stream->terminal_cleanup_owners == 0) {
      stream->terminal_published.store(true, std::memory_order_release);
      stream->terminal_cleanup_complete.store(true, std::memory_order_release);
      stream->terminal_notification.notify();
   }
}

using stream_terminal_owner = std::unique_ptr<engine_stream::impl, decltype(&release_stream_terminal_owner)>;

[[nodiscard]] stream_terminal_owner claim_stream_terminal_owner(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
   ++stream->terminal_cleanup_owners;
   stream->terminal_cleanup_complete.store(false, std::memory_order_release);
   return {stream.get(), &release_stream_terminal_owner};
}

boost::asio::awaitable<void> wait_for_stream_terminal_cleanup(const std::shared_ptr<engine_stream::impl>& stream) {
   while (!stream->terminal_cleanup_complete.load(std::memory_order_acquire)) {
      const auto observed = stream->terminal_notification.epoch();
      if (!stream->terminal_cleanup_complete.load(std::memory_order_acquire)) {
         (void)co_await stream->terminal_notification.async_wait(observed);
      }
   }
}

struct engine_connection_metrics_state {
   std::atomic<std::uint64_t> connections_opened{0};
   std::atomic<std::uint64_t> connections_closed{0};
   std::atomic<std::uint64_t> handshakes_started{0};
   std::atomic<std::uint64_t> handshakes_completed{0};
   std::atomic<std::uint64_t> handshakes_failed{0};
   std::atomic<std::uint64_t> streams_opened{0};
   std::atomic<std::uint64_t> streams_accepted{0};
   std::atomic<std::uint64_t> streams_reset{0};
   std::atomic<std::uint64_t> frames_sent{0};
   std::atomic<std::uint64_t> frames_received{0};
   std::atomic<std::uint64_t> bytes_sent{0};
   std::atomic<std::uint64_t> bytes_received{0};
   std::atomic<std::uint64_t> packets_sent{0};
   std::atomic<std::uint64_t> packets_received{0};
   std::atomic<std::uint64_t> timeouts{0};
   std::atomic<std::uint64_t> cancellations{0};
   std::atomic<std::uint64_t> backpressure_rejections{0};
   std::atomic<std::uint64_t> retry_packets_received{0};
   std::atomic<std::uint64_t> new_tokens_received{0};
   std::atomic<std::uint64_t> new_tokens_submitted{0};
   std::atomic<std::size_t> queued_bytes{0};
   std::atomic<std::size_t> active_streams{0};
   std::atomic<bool> closed{false};

   [[nodiscard]] engine_connection_metrics snapshot() const noexcept {
      const auto relaxed = std::memory_order_relaxed;
      return engine_connection_metrics{
          .connections_opened = connections_opened.load(relaxed),
          .connections_closed = connections_closed.load(relaxed),
          .handshakes_started = handshakes_started.load(relaxed),
          .handshakes_completed = handshakes_completed.load(relaxed),
          .handshakes_failed = handshakes_failed.load(relaxed),
          .streams_opened = streams_opened.load(relaxed),
          .streams_accepted = streams_accepted.load(relaxed),
          .streams_reset = streams_reset.load(relaxed),
          .frames_sent = frames_sent.load(relaxed),
          .frames_received = frames_received.load(relaxed),
          .bytes_sent = bytes_sent.load(relaxed),
          .bytes_received = bytes_received.load(relaxed),
          .packets_sent = packets_sent.load(relaxed),
          .packets_received = packets_received.load(relaxed),
          .timeouts = timeouts.load(relaxed),
          .cancellations = cancellations.load(relaxed),
          .backpressure_rejections = backpressure_rejections.load(relaxed),
          .retry_packets_received = retry_packets_received.load(relaxed),
          .new_tokens_received = new_tokens_received.load(relaxed),
          .new_tokens_submitted = new_tokens_submitted.load(relaxed),
          .queued_bytes = queued_bytes.load(relaxed),
          .active_streams = active_streams.load(relaxed),
          .closed = closed.load(relaxed),
      };
   }
};


struct engine_connection::impl {
   struct queued_packet {
      std::vector<std::uint8_t> bytes;
      forge::net::transport::datagram_io::received route;
   };

   impl(asio::io_context& context_value, std::shared_ptr<udp::socket> socket_value, udp::endpoint local_endpoint_value,
        udp::endpoint remote_endpoint_value, engine_transport_limits limits_value)
       : context(context_value), strand(asio::make_strand(context_value)), socket(std::move(socket_value)),
         local_endpoint_value(std::move(local_endpoint_value)), remote_endpoint(std::move(remote_endpoint_value)),
         limits(limits_value), handshake_timer(strand), expiry_timer(strand), owner_drain_timer(strand) {}

   impl(asio::io_context& context_value, std::shared_ptr<server_udp_socket> server_socket_value,
        udp::endpoint local_endpoint_value, udp::endpoint remote_endpoint_value, engine_transport_limits limits_value)
       : context(context_value), strand(asio::make_strand(context_value)),
         server_socket(std::move(server_socket_value)), local_endpoint_value(std::move(local_endpoint_value)),
         remote_endpoint(std::move(remote_endpoint_value)), limits(limits_value), handshake_timer(strand),
         expiry_timer(strand), owner_drain_timer(strand) {}

   ~impl() {
      if (conn != nullptr) {
         ngtcp2_conn_del(conn);
      }
      if (ossl_ctx != nullptr) {
         ngtcp2_crypto_ossl_ctx_del(ossl_ctx);
      }
      if (ssl) {
         SSL_set_app_data(ssl.get(), nullptr);
      }
   }

   asio::io_context& context;
   asio::strand<asio::io_context::executor_type> strand;
   std::shared_ptr<udp::socket> socket;
   std::shared_ptr<server_udp_socket> server_socket;
   forge::asio::gate send_gate;
   udp::endpoint local_endpoint_value;
   udp::endpoint remote_endpoint;
   engine_transport_limits limits;
   engine_connection_metrics_state metrics{};
   stateless_reset_secret reset_secret = random_stateless_reset_secret();
   std::weak_ptr<impl> self;

   ngtcp2_conn* conn = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
   ngtcp2_crypto_conn_ref conn_ref{};
   ssl_ctx_ptr ssl_ctx;
   ssl_ptr ssl;
   engine_security_options peer_security{};
   std::optional<engine_peer_certificate> peer_certificate_value;
   std::mutex inbound_admission_mutex;
   std::shared_ptr<void> inbound_admission;

   std::unordered_map<std::int64_t, std::shared_ptr<engine_stream::impl>> streams;
   std::deque<std::shared_ptr<engine_stream::impl>> accepted_streams;
   std::vector<std::weak_ptr<asio::steady_timer>> handshake_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> accept_stream_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> open_stream_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> background_waiters;
   std::function<void()> handshake_completed_hook;
   std::function<void(std::shared_ptr<impl>)> closed_hook;
   std::function<void(const ngtcp2_cid&)> local_connection_id_issued_hook;
   std::function<void(const ngtcp2_cid&)> local_connection_id_retired_hook;
   std::function<void(impl&)> issue_new_token;
   std::function<void(std::vector<std::uint8_t>)> client_token_store;
   std::function<bool(std::string_view)> test_failpoint;
   std::optional<std::vector<std::uint8_t>> pending_client_token;

   asio::steady_timer handshake_timer;
   asio::steady_timer expiry_timer;
   asio::steady_timer owner_drain_timer;
   std::deque<server_udp_socket::packet> outbound_datagrams;
   std::deque<queued_packet> inbound_packets;
   std::size_t queued_datagram_bytes = 0;
   std::size_t queued_inbound_packet_bytes = 0;
   bool handshake_done = false;
   std::optional<engine_error_kind> handshake_terminal_cause;
   boost::system::error_code handshake_transport_error;
   bool closing = false;
   bool canceled = false;
   bool terminal_cleanup_complete = false;
   bool close_started = false;
   bool close_cleanup_complete = false;
   std::exception_ptr close_error;
   std::atomic_bool cancellation_requested{false};
   std::atomic_bool owner_released{false};
   std::atomic<ngtcp2_tstamp> owner_released_at{0};
   std::atomic_bool terminal_signaled{false};
   forge::asio::notification termination_changed;
   bool closed_hook_called = false;
   bool closed_hook_delivered = false;
   bool receive_loop_started = false;
   bool cancel_request_worker_started = false;
   bool owner_drain_timer_started = false;
   bool owner_drain_timer_expired = false;
   std::atomic_size_t background_jobs{0};
   bool drain_active = false;
   bool drain_requested = false;
   // Owner-strand generations let a caller join the native drain pass it
   // requested without waiting for unrelated future drain work to become idle.
   std::uint64_t drain_request_generation = 0;
   std::uint64_t drain_completed_generation = 0;
   forge::asio::notification drain_completion_changed;
   bool udp_send_active = false;
   bool packet_processing_active = false;
   bool expiry_event_pending = false;
   bool server_side = false;
   bool listener_accept_notified = false;
   bool report_accept_failure = true;
   bool client_token_store_verified = false;
   bool new_token_submitted = false;
   std::int64_t last_writable_stream_id = -1;

   [[nodiscard]] udp::endpoint local_endpoint() const {
      return local_endpoint_value;
   }

   [[nodiscard]] std::shared_ptr<engine_stream::impl> ensure_stream(std::int64_t stream_id) {
      if (auto it = streams.find(stream_id); it != streams.end()) {
         return it->second;
      }
      auto stream = std::make_shared<engine_stream::impl>(stream_id);
      stream->connection = self;
      streams.emplace(stream_id, stream);
      update_active_stream_metrics();
      if (conn != nullptr) {
         (void)ngtcp2_conn_set_stream_user_data(conn, stream_id, stream.get());
      }
      return stream;
   }

   [[nodiscard]] static bool stream_is_active(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
      return stream && !stream->closed && !stream->reset &&
             !(stream->local_write_closed && (stream->remote_read_closed || stream->remote_read_reset));
   }

   [[nodiscard]] std::size_t active_stream_count() const {
      return static_cast<std::size_t>(
          std::ranges::count_if(streams, [](const auto& item) { return stream_is_active(item.second); }));
   }

   void update_active_stream_metrics() {
      metrics.active_streams.store(active_stream_count(), std::memory_order_relaxed);
   }

   void clear_queued_work() {
      outbound_datagrams.clear();
      inbound_packets.clear();
      queued_datagram_bytes = 0;
      queued_inbound_packet_bytes = 0;
      metrics.queued_bytes.store(0, std::memory_order_relaxed);
   }

   void release_queued_stream_writes(const std::shared_ptr<engine_stream::impl>& stream) {
      auto released = std::size_t{0};
      for (auto& write : stream->outbound) {
         if (released <= (std::numeric_limits<std::size_t>::max)() - write.data.size()) {
            released += write.data.size();
         } else {
            released = (std::numeric_limits<std::size_t>::max)();
         }
         wake(write.waiters);
      }
      for (const auto& write : stream->retained) {
         if (released <= (std::numeric_limits<std::size_t>::max)() - write.data.size()) {
            released += write.data.size();
         } else {
            released = (std::numeric_limits<std::size_t>::max)();
         }
      }
      if (released > 0) {
         const auto queued_bytes = metrics.queued_bytes.load(std::memory_order_relaxed);
         if (queued_bytes >= released) {
            metrics.queued_bytes.store(queued_bytes - released, std::memory_order_relaxed);
         } else {
            metrics.queued_bytes.store(0, std::memory_order_relaxed);
         }
      }
      stream->outbound.clear();
      stream->retained.clear();
      if (owner_released.load(std::memory_order_acquire) &&
          metrics.queued_bytes.load(std::memory_order_relaxed) == 0) {
         try {
            static_cast<void>(owner_drain_timer.cancel());
         } catch (...) {
         }
         termination_changed.notify();
      }
   }

   [[nodiscard]] bool reset_stream_on_owner(const std::shared_ptr<engine_stream::impl>& stream,
                                             stream_terminal_owner& cleanup) noexcept {
      assert(strand.running_in_this_thread());
      if (!stream || stream->reset || stream->closed) {
         return false;
      }
      // ngtcp2 may synchronously invoke stream_close_cb during shutdown. Claim
      // recovery ownership first so that callback cannot complete our join.
      cleanup = claim_stream_terminal_owner(stream);
      auto shutdown_result = 0;
      auto should_drain = false;
      if (conn != nullptr && !closing && !canceled) {
         shutdown_result = ngtcp2_conn_shutdown_stream(conn, 0, stream->id, 0);
         should_drain = shutdown_result == 0;
      }
      release_queued_stream_writes(stream);
      stream->reset = true;
      publish_stream_terminal(stream);
      if (test_failpoint) {
         try {
            static_cast<void>(test_failpoint("stream_reset_published"));
         } catch (...) {
            // Test instrumentation must not alter a noexcept reset transition.
         }
      }
      wake(stream->read_waiters);
      wake(stream->write_waiters);
      if (!std::exchange(stream->reset_counted, true)) {
         metrics.streams_reset.fetch_add(1, std::memory_order_relaxed);
      }
      update_active_stream_metrics();
      if (shutdown_result != 0) {
         fail_all();
         return false;
      }
      return should_drain;
   }

   boost::asio::awaitable<void>
   async_reset_stream_after_close_failure(const std::shared_ptr<engine_stream::impl>& stream) {
      assert(strand.running_in_this_thread());
      auto cleanup = stream_terminal_owner{nullptr, &release_stream_terminal_owner};
      if (!reset_stream_on_owner(stream, cleanup)) {
         // A reset with no native drain needed must not wait on its own claim.
         cleanup.reset();
         // Another owner may have published reset before it finishes draining
         // the native RESET_STREAM. Join that owner before reporting close.
         if (test_failpoint) {
            static_cast<void>(test_failpoint("stream_reset_before_terminal_join"));
         }
         co_await wait_for_stream_terminal_cleanup(stream);
         co_return;
      }
      try {
         co_await drain_send();
      } catch (...) {
         // reset_stream_on_owner has already made local terminal state visible.
         // A later native send failure follows its existing fail_all path.
      }
   }

   void start_stream_cancel_worker(const std::shared_ptr<engine_stream::impl>& stream) {
      assert(strand.running_in_this_thread());
      if (stream->cancel_worker_started) {
         return;
      }
      auto shared = self.lock();
      if (!shared) {
         throw_engine(engine_error_kind::connection_closed, "QUIC connection expired before stream publication");
      }

      stream->cancel_worker_started = true;
      background_jobs.fetch_add(1, std::memory_order_release);
      try {
         asio::co_spawn(
             strand,
             [shared = std::move(shared), stream]() -> asio::awaitable<void> {
                auto finish = std::unique_ptr<engine_connection::impl, void (*)(engine_connection::impl*)>{
                    shared.get(), [](engine_connection::impl* value) { value->finish_background_job(); }};
                try {
                   static_cast<void>(co_await stream->cancel_requested.async_wait(0));
                } catch (...) {
                   // Failure to arm the waiter terminalizes this stream on its owner.
                }
                auto cleanup = stream_terminal_owner{nullptr, &release_stream_terminal_owner};
                if (!shared->reset_stream_on_owner(stream, cleanup)) {
                   co_return;
                }
                if (shared->test_failpoint && shared->test_failpoint("stream_reset_after_publish_before_drain")) {
                   // Test-only seam: retain the existing cancel worker between
                   // reset publication and its native terminal drain.
                   while (shared->test_failpoint("stream_reset_after_publish_before_drain_wait")) {
                      auto timer = asio::steady_timer{shared->strand};
                      timer.expires_after(std::chrono::milliseconds{1});
                      auto ec = boost::system::error_code{};
                      co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                   }
                }
                try {
                   co_await shared->drain_send();
                } catch (...) {
                   // Local stream reset is terminal; wire RESET_STREAM is best effort.
                }
             },
             asio::detached);
      } catch (...) {
         stream->cancel_worker_started = false;
         finish_background_job();
         throw;
      }
   }

   void wake_and_clear_streams(bool reset_streams) {
      for (auto& [_, stream] : streams) {
         if (reset_streams) {
            stream->reset = true;
         } else {
            stream->closed = true;
         }
         wake(stream->read_waiters);
         wake(stream->write_waiters);
         release_queued_stream_writes(stream);
         stream->cancel_requested.notify();
         finish_stream_terminal_cleanup(stream);
      }
      update_active_stream_metrics();
   }

   void deliver_closed_hook_if_idle() noexcept {
      if (!closed_hook_called || closed_hook_delivered || background_jobs.load(std::memory_order_acquire) != 0 ||
          !closed_hook) {
         return;
      }
      closed_hook_delivered = true;
      if (auto shared = self.lock()) {
         try {
            closed_hook(std::move(shared));
         } catch (...) {
         }
      }
   }

   void notify_closed_once() noexcept {
      if (!closed_hook_called) {
         closed_hook_called = true;
      }
      deliver_closed_hook_if_idle();
   }

   void finish_background_job() noexcept {
      const auto previous = background_jobs.fetch_sub(1, std::memory_order_acq_rel);
      if (previous == 0) {
         background_jobs.store(0, std::memory_order_release);
         return;
      }
      if (previous == 1) {
         wake(background_waiters);
         deliver_closed_hook_if_idle();
      }
   }

   boost::asio::awaitable<void> wait_background_idle() {
      assert(strand.running_in_this_thread());
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      co_await asio::dispatch(strand, asio::use_awaitable);
      while (background_jobs.load(std::memory_order_acquire) != 0) {
         auto timer = std::make_shared<asio::steady_timer>(strand);
         timer->expires_after(std::chrono::minutes{10});
         background_waiters.emplace_back(timer);
         boost::system::error_code ec;
         co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
         co_await asio::dispatch(strand, asio::use_awaitable);
      }
   }

   void complete_close(std::exception_ptr error = {}) noexcept {
      assert(strand.running_in_this_thread());
      if (!close_error && error) {
         close_error = std::move(error);
      }
      close_cleanup_complete = true;
      termination_changed.notify();
   }

   boost::asio::awaitable<void> wait_close_cleanup() {
      assert(strand.running_in_this_thread());
      auto observed = termination_changed.epoch();
      while (!close_cleanup_complete) {
         observed = co_await termination_changed.async_wait(observed);
      }
      if (close_error) {
         std::rethrow_exception(close_error);
      }
   }

   template <typename Operation> void spawn_background(Operation operation) {
      auto shared = self.lock();
      if (!shared) {
         return;
      }
      shared->background_jobs.fetch_add(1, std::memory_order_release);
      try {
         asio::dispatch(strand, [shared, operation = std::move(operation)]() mutable {
            if (shared->closing || shared->canceled) {
               shared->finish_background_job();
               return;
            }
            try {
               asio::co_spawn(
                   shared->strand,
                   [shared, operation = std::move(operation)]() mutable -> asio::awaitable<void> {
                      struct completion_guard {
                         std::shared_ptr<engine_connection::impl> value;

                         ~completion_guard() {
                            value->finish_background_job();
                         }
                      } guard{shared};
                      try {
                         co_await operation(shared);
                      } catch (...) {
                         shared->fail_all();
                      }
                   },
                   asio::detached);
            } catch (...) {
               shared->finish_background_job();
               shared->fail_all();
            }
         });
      } catch (...) {
         shared->finish_background_job();
         throw;
      }
   }

   void cancel_transport_io(bool close_socket) {
      assert(strand.running_in_this_thread());
      boost::system::error_code ignored;
      if (close_socket && socket) {
         send_gate.close();
         socket->cancel(ignored);
         socket->close(ignored);
      }
      try {
         handshake_timer.cancel();
      } catch (...) {
         // Continue draining the remaining transport work.
      }
      try {
         expiry_timer.cancel();
      } catch (...) {
         // Continue draining the remaining transport work.
      }
      try {
         owner_drain_timer.cancel();
      } catch (...) {
         // Continue draining the remaining transport work.
      }
   }

   void signal_terminal() noexcept {
      terminal_signaled.store(true, std::memory_order_release);
      termination_changed.notify();
   }

   void fail_all() noexcept {
      assert(strand.running_in_this_thread());
      if (terminal_cleanup_complete) {
         return;
      }
      signal_terminal();
      canceled = true;
      closing = true;
      pending_client_token.reset();
      metrics.closed.store(true, std::memory_order_relaxed);
      clear_queued_work();
      cancel_transport_io(!server_side);
      wake(handshake_waiters);
      wake(accept_stream_waiters);
      wake(open_stream_waiters);
      wake_and_clear_streams(true);
      complete_drain_requests(drain_request_generation);
      {
         auto lock = std::scoped_lock{inbound_admission_mutex};
         inbound_admission.reset();
      }
      notify_closed_once();
      terminal_cleanup_complete = true;
   }

   void fail_udp(boost::system::error_code error) noexcept {
      assert(strand.running_in_this_thread());
      if (!handshake_done && !closing && !canceled && !handshake_terminal_cause &&
          error != asio::error::operation_aborted) {
         handshake_transport_error = error;
         if (error == asio::error::connection_refused || error == asio::error::connection_reset ||
             error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
             error == asio::error::network_down) {
            handshake_terminal_cause = engine_error_kind::connection_closed;
         } else if (error == asio::error::timed_out) {
            handshake_terminal_cause = engine_error_kind::handshake_timeout;
         } else {
            handshake_terminal_cause = engine_error_kind::internal_error;
         }
      }
      fail_all();
   }

   void close_transport(bool cancel_socket) {
      assert(strand.running_in_this_thread());
      if (terminal_cleanup_complete) {
         return;
      }
      signal_terminal();
      closing = true;
      pending_client_token.reset();
      metrics.closed.store(true, std::memory_order_relaxed);
      clear_queued_work();
      cancel_transport_io(cancel_socket);
      wake(handshake_waiters);
      wake(accept_stream_waiters);
      wake(open_stream_waiters);
      wake_and_clear_streams(false);
      complete_drain_requests(drain_request_generation);
      {
         auto lock = std::scoped_lock{inbound_admission_mutex};
         inbound_admission.reset();
      }
      notify_closed_once();
      terminal_cleanup_complete = true;
   }

   void verify_selected_alpn(std::string_view expected) {
      const unsigned char* selected = nullptr;
      unsigned int selected_len = 0;
      SSL_get0_alpn_selected(ssl.get(), &selected, &selected_len);
      if (selected_len != expected.size() || selected == nullptr ||
          std::memcmp(selected, expected.data(), selected_len) != 0) {
         throw_engine(engine_error_kind::alpn_mismatch, "QUIC ALPN mismatch");
      }
   }

   void verify_peer(const engine_security_options& security) {
      assert(strand.running_in_this_thread());
      peer_certificate_value.reset();
      if (security.verify_peer && server_side && SSL_get_peer_cert_chain(ssl.get()) == nullptr) {
         throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer did not present a certificate");
      }
      auto cert = x509_ptr{SSL_get1_peer_certificate(ssl.get())};
      if (!cert) {
         if (security.verify_peer) {
            throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer did not present a certificate");
         }
         return;
      }
      auto der = der_from_certificate(cert.get());
      auto peer = engine_peer_certificate{.der = std::move(der)};
      peer.sha256_fingerprint = engine_sha256_fingerprint(peer.der);
      if (security.verify_peer) {
         if (security.expected_sha256_fingerprint &&
             peer.sha256_fingerprint != normalize_engine_sha256_fingerprint(*security.expected_sha256_fingerprint)) {
            throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer certificate fingerprint mismatch");
         }
         if (security.verifier && !security.verifier(peer)) {
            throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer verifier rejected certificate");
         }
      }
      peer_certificate_value = std::move(peer);
   }

   void complete_handshake() {
      assert(strand.running_in_this_thread());
      if (handshake_done) {
         return;
      }
      handshake_done = true;
      try {
         handshake_timer.cancel();
      } catch (...) {
      }
      metrics.handshakes_completed.fetch_add(1, std::memory_order_relaxed);
      wake(handshake_waiters);
      if (handshake_completed_hook) {
         handshake_completed_hook();
      }
   }

   void commit_pending_client_token() noexcept {
      assert(strand.running_in_this_thread());
      if (!client_token_store_verified || !pending_client_token || !client_token_store) {
         return;
      }
      auto token = std::move(*pending_client_token);
      pending_client_token.reset();
      try {
         client_token_store(std::move(token));
      } catch (...) {
         // Cache failures must not affect a verified QUIC connection.
      }
   }

   boost::asio::awaitable<void> wait_handshake(std::chrono::milliseconds timeout) {
      assert(strand.running_in_this_thread());
      co_await asio::dispatch(strand, asio::use_awaitable);
      if (handshake_done) {
         co_return;
      }
      if (!closing && !canceled) {
         auto timer = std::make_shared<asio::steady_timer>(strand);
         timer->expires_after(timeout);
         handshake_waiters.emplace_back(timer);
         boost::system::error_code ec;
         co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
      }
      if (handshake_done) {
         co_return;
      }
      if (handshake_terminal_cause) {
         if (test_failpoint) {
            // Observation barrier only: retain the real socket failure and let
            // the outer active_connect arbitration decide the terminal winner.
            static_cast<void>(test_failpoint("handshake_udp_failure_before_report"));
         }
         throw_engine(*handshake_terminal_cause, "QUIC handshake UDP failure: " + handshake_transport_error.message());
      }
      if (canceled && metrics.backpressure_rejections.load(std::memory_order_relaxed) > 0) {
         throw_engine(engine_error_kind::backpressure_rejected,
                      "QUIC handshake stopped by inbound packet backpressure");
      }
      if (canceled) {
         throw_engine(engine_error_kind::canceled, "QUIC handshake was canceled");
      }
      if (closing) {
         throw_engine(engine_error_kind::connection_closed, "QUIC connection closed before handshake completed");
      }
      metrics.handshakes_failed.fetch_add(1, std::memory_order_relaxed);
      metrics.timeouts.fetch_add(1, std::memory_order_relaxed);
      throw_engine(engine_error_kind::handshake_timeout, "QUIC handshake timed out");
   }

   asio::awaitable<boost::system::error_code> send_packet_impl(server_udp_socket::packet packet) {
      if (server_socket) {
         co_return co_await server_socket->async_send(std::move(packet));
      }
      try {
         auto ticket = co_await send_gate.acquire();
         auto ec = boost::system::error_code{};
         const auto written =
             co_await socket->async_send(asio::buffer(packet.bytes), asio::redirect_error(asio::use_awaitable, ec));
         if (!ec && written != packet.bytes.size()) {
            ec = asio::error::message_size;
         }
         co_return ec;
      } catch (const forge::asio::exceptions::canceled&) {
         co_return asio::error::operation_aborted;
      } catch (const forge::asio::exceptions::rejected&) {
         co_return asio::error::operation_aborted;
      }
   }

   asio::awaitable<void> wait_send_terminal() {
      auto observed = termination_changed.epoch();
      while (!terminal_signaled.load(std::memory_order_acquire)) {
         observed = co_await termination_changed.async_wait(observed);
      }
   }

   asio::awaitable<boost::system::error_code> send_packet(server_udp_socket::packet packet,
                                                          bool closing_packet = false) {
      // The dedicated client owns one immutable connected path. Never silently
      // send a packet on a different path than ngtcp2 requested.
      if (!server_socket && (packet.route.local != local_endpoint_value || packet.route.remote != remote_endpoint)) {
         throw_engine(engine_error_kind::internal_error, "QUIC client output changed its fixed UDP path");
      }
      using namespace asio::experimental::awaitable_operators;
      auto deadline = asio::steady_timer{strand};
      deadline.expires_after(detached_write_drain_timeout);
      // Structured races join the canceled branch, including shared-socket gate
      // acquisition/readiness. Never cancel the listener FD for one connection.
      if (closing_packet) {
         auto result = co_await (send_packet_impl(std::move(packet)) || deadline.async_wait(asio::use_awaitable));
         co_return result.index() == 0 ? std::get<0>(result) : asio::error::timed_out;
      }
      auto result = co_await (send_packet_impl(std::move(packet)) || deadline.async_wait(asio::use_awaitable) ||
                              wait_send_terminal());
      if (result.index() == 0) {
         co_return std::get<0>(result);
      }
      co_return result.index() == 1 ? asio::error::timed_out : asio::error::operation_aborted;
   }

   void start_udp_send_loop() {
      if (udp_send_active) {
         return;
      }
      if (self.expired()) {
         return;
      }
      udp_send_active = true;
      spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
         while (!value->closing && !value->canceled) {
            if (value->outbound_datagrams.empty()) {
               break;
            }
            auto packet = std::move(value->outbound_datagrams.front());
            value->outbound_datagrams.pop_front();
            if (value->queued_datagram_bytes >= packet.bytes.size()) {
               value->queued_datagram_bytes -= packet.bytes.size();
            } else {
               value->queued_datagram_bytes = 0;
            }

            const auto packet_size = packet.bytes.size();
            const auto ec = co_await value->send_packet(std::move(packet));
            if (ec) {
               if (!value->closing) {
                  value->fail_udp(ec);
               }
               break;
            }
            value->metrics.packets_sent.fetch_add(1, std::memory_order_relaxed);
            value->metrics.bytes_sent.fetch_add(packet_size, std::memory_order_relaxed);
         }
         value->udp_send_active = false;
         if (!value->outbound_datagrams.empty() && !value->closing && !value->canceled) {
            value->start_udp_send_loop();
         }
      });
      if (closing || canceled) {
         udp_send_active = false;
      }
   }

   void enqueue_datagram(std::span<const std::uint8_t> packet, const ngtcp2_path& path) {
      if (packet.empty()) {
         return;
      }
      if (queued_datagram_bytes + packet.size() > max_queued_datagram_bytes) {
         metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
         fail_all();
         throw_engine(engine_error_kind::backpressure_rejected, "QUIC UDP datagram queue exceeds limit");
      }
      outbound_datagrams.push_back({.bytes = {packet.begin(), packet.end()}, .route = copy_route(path)});
      queued_datagram_bytes += packet.size();
      start_udp_send_loop();
   }

   void request_packet_processing() {
      if (packet_processing_active || drain_active || inbound_packets.empty()) {
         return;
      }
      if (self.expired()) {
         return;
      }
      spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
         try {
            co_await value->process_queued_packets();
         } catch (const engine_failure&) {
            value->fail_all();
         }
      });
   }

   void request_expiry_processing() {
      if (self.expired()) {
         return;
      }
      spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
         try {
            co_await value->handle_expiry_event();
         } catch (const engine_failure&) {
            value->fail_all();
         }
      });
   }

   void schedule_post_ngtcp2_work() {
      if (expiry_event_pending && !drain_active && !packet_processing_active) {
         request_expiry_processing();
      }
      if (!inbound_packets.empty() && !drain_active && !packet_processing_active) {
         request_packet_processing();
      }
   }

   void schedule_expiry() {
      assert(strand.running_in_this_thread());
      if (conn == nullptr || closing) {
         return;
      }
      const auto expiry = ngtcp2_conn_get_expiry(conn);
      if (expiry == (std::numeric_limits<ngtcp2_tstamp>::max)()) {
         expiry_timer.cancel();
         return;
      }
      const auto now = timestamp();
      const auto delay = expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};
      expiry_timer.expires_after(delay);
      auto shared = self.lock();
      if (!shared) {
         return;
      }
      background_jobs.fetch_add(1, std::memory_order_release);
      try {
         expiry_timer.async_wait([shared](boost::system::error_code ec) {
            if (ec) {
               shared->finish_background_job();
               return;
            }
            try {
               asio::co_spawn(
                   shared->strand,
                   [shared]() -> asio::awaitable<void> {
                      struct completion_guard {
                         std::shared_ptr<impl> value;

                         ~completion_guard() {
                            value->finish_background_job();
                         }
                      } guard{shared};
                      try {
                         if (shared->test_failpoint && shared->test_failpoint("expiry_worker_failure")) {
                            throw std::runtime_error{"QUIC test failpoint: expiry worker failure"};
                         }
                         co_await shared->handle_expiry_event();
                      } catch (...) {
                         shared->fail_all();
                      }
                   },
                   asio::detached);
            } catch (...) {
               shared->finish_background_job();
               shared->fail_all();
            }
         });
      } catch (...) {
         finish_background_job();
         fail_all();
      }
   }

   boost::asio::awaitable<void> handle_expiry_event() {
      assert(strand.running_in_this_thread());
      co_await asio::dispatch(strand, asio::use_awaitable);
      if (conn == nullptr || closing) {
         co_return;
      }
      if (drain_active || packet_processing_active) {
         expiry_event_pending = true;
         co_return;
      }
      expiry_event_pending = false;
      const auto rv = ngtcp2_conn_handle_expiry(conn, timestamp());
      if (rv == NGTCP2_ERR_IDLE_CLOSE) {
         close_transport(!server_side);
         co_return;
      }
      if (rv != 0) {
         fail_all();
         co_return;
      }
      co_await drain_send();
   }

   [[nodiscard]] std::vector<std::shared_ptr<engine_stream::impl>> writable_streams() {
      auto out = std::vector<std::shared_ptr<engine_stream::impl>>{};
      out.reserve(streams.size());
      for (auto& [_, stream] : streams) {
         if (!stream->outbound.empty() && !stream->local_write_closed && !stream->reset) {
            out.push_back(stream);
         }
      }
      std::ranges::sort(out, {}, &engine_stream::impl::id);
      const auto next = std::ranges::upper_bound(out, last_writable_stream_id, {}, &engine_stream::impl::id);
      std::ranges::rotate(out, next);
      return out;
   }

   void reject_unwritable_stream(const std::shared_ptr<engine_stream::impl>& stream, ngtcp2_ssize error) {
      if (!stream) {
         return;
      }
      release_queued_stream_writes(stream);
      stream->local_write_closed = true;
      if (error == NGTCP2_ERR_STREAM_NOT_FOUND) {
         stream->closed = true;
      }
      finish_stream_terminal_cleanup(stream);
      wake(stream->write_waiters);
      update_active_stream_metrics();
   }

   void mark_stream_data_submitted(std::shared_ptr<engine_stream::impl>& stream, ngtcp2_ssize data_len) {
      if (!stream || data_len <= 0) {
         return;
      }
      auto& write = stream->outbound.front();
      if (!write.base_offset_set) {
         write.base_offset = stream->send_next_offset;
         write.base_offset_set = true;
      }
      write.submitted += static_cast<std::size_t>(data_len);
      stream->send_next_offset += static_cast<std::uint64_t>(data_len);
      complete_submitted_writes(stream);
   }

   void complete_submitted_writes(std::shared_ptr<engine_stream::impl>& stream) {
      while (!stream->outbound.empty()) {
         auto& write = stream->outbound.front();
         if (write.submitted < write.data.size() || (!write.fin && write.data.empty()) ||
             (write.fin && write.data.empty() && !write.base_offset_set)) {
            break;
         }
         if (!write.data.empty()) {
            stream->retained.push_back(engine_stream::impl::retained_write{
                .data = std::move(write.data),
                .lifetime = std::move(write.lifetime),
                .base_offset = write.base_offset,
                .fin = write.fin,
            });
         }
         if (write.fin) {
            stream->local_write_closed = true;
            finish_stream_terminal_cleanup(stream);
         }
         wake(write.waiters);
         stream->outbound.pop_front();
      }
      wake(stream->write_waiters);
      update_active_stream_metrics();
   }

   void complete_drain_requests(std::uint64_t generation) noexcept {
      assert(strand.running_in_this_thread());
      if (generation <= drain_completed_generation) {
         return;
      }
      drain_completed_generation = generation;
      drain_completion_changed.notify();
   }

   boost::asio::awaitable<void> wait_for_drain_requests(std::uint64_t generation) {
      assert(strand.running_in_this_thread());
      while (drain_completed_generation < generation) {
         const auto observed = drain_completion_changed.epoch();
         if (drain_completed_generation < generation) {
            static_cast<void>(co_await drain_completion_changed.async_wait(observed));
         }
      }
   }

   void finish_active_drain() noexcept {
      assert(strand.running_in_this_thread());
      drain_active = false;
      // A connection-level terminal state supersedes native stream draining.
      complete_drain_requests(drain_request_generation);
   }

   boost::asio::awaitable<void> drain_send() {
      assert(strand.running_in_this_thread());
      co_await asio::dispatch(strand, asio::use_awaitable);
      if (closing || canceled || conn == nullptr) {
         co_return;
      }
      if (drain_active) {
         const auto generation = ++drain_request_generation;
         drain_requested = true;
         if (test_failpoint) {
            static_cast<void>(test_failpoint("drain_request_before_completion_wait"));
         }
         co_await wait_for_drain_requests(generation);
         co_return;
      }

      drain_active = true;
      auto clear = std::unique_ptr<void, void (*)(void*)>{
          this, [](void* ptr) { static_cast<impl*>(ptr)->finish_active_drain(); }};
      if (test_failpoint && test_failpoint("drain_after_owner_claim_before_native_write")) {
         // Test-only seam: pause the existing drain owner after its claim.
         while (test_failpoint("drain_after_owner_claim_before_native_write_wait")) {
            auto timer = asio::steady_timer{strand};
            timer.expires_after(std::chrono::milliseconds{1});
            auto ec = boost::system::error_code{};
            co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
         }
      }

      do {
         if (closing || canceled || conn == nullptr) {
            break;
         }
         drain_requested = false;
         const auto completed_generation = drain_request_generation;
         auto packets_this_drain = std::size_t{0};
         for (;;) {
            auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
            auto ps = ngtcp2_path_storage{};
            ngtcp2_path_storage_zero(&ps);
            auto pi = ngtcp2_pkt_info{};
            const auto packet_ts = timestamp();
            auto nwrite = ngtcp2_ssize{0};
            auto selected = std::shared_ptr<engine_stream::impl>{};
            auto data_len = ngtcp2_ssize{0};
            auto candidates = writable_streams();
            auto candidate = candidates.begin();

            for (;;) {
               data_len = 0;
               auto flags = std::uint32_t{0};
               auto stream_id = std::int64_t{-1};
               auto datav = ngtcp2_vec{};
               auto datavcnt = std::size_t{0};
               selected = candidate == candidates.end() ? std::shared_ptr<engine_stream::impl>{} : *candidate++;
               if (selected) {
                  last_writable_stream_id = selected->id;
                  auto& write = selected->outbound.front();
                  stream_id = selected->id;
                  const auto remaining = write.data.size() - write.submitted;
                  datav.base = remaining == 0 ? nullptr : write.data.data() + write.submitted;
                  datav.len = remaining;
                  datavcnt = (remaining > 0 || write.fin) ? 1 : 0;
                  if (write.fin && remaining == 0) {
                     flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
                  }
               }

               nwrite = ngtcp2_conn_writev_stream(conn, &ps.path, &pi, packet.data(), packet.size(), &data_len, flags,
                                                  stream_id, datavcnt == 0 ? nullptr : &datav, datavcnt, packet_ts);

               if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
                  continue;
               }
               if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR || nwrite == NGTCP2_ERR_STREAM_NOT_FOUND) {
                  reject_unwritable_stream(selected, nwrite);
                  continue;
               }
               if (nwrite != NGTCP2_ERR_WRITE_MORE) {
                  break;
               }

               mark_stream_data_submitted(selected, data_len);
            }

            if (nwrite < 0) {
               fail_all();
               throw_engine(engine_error_kind::internal_error, std::string{"ngtcp2_conn_writev_stream failed: "} +
                                                                   ngtcp2_strerror(static_cast<int>(nwrite)));
            }
            if (nwrite == 0) {
               break;
            }
            if (selected && data_len >= 0) {
               if (data_len == 0 && !selected->outbound.empty()) {
                  auto& write = selected->outbound.front();
                  if (write.fin) {
                     write.base_offset = selected->send_next_offset;
                     write.base_offset_set = true;
                     write.submitted = write.data.size();
                  }
               }
               mark_stream_data_submitted(selected, data_len);
               complete_submitted_writes(selected);
            }
            ngtcp2_conn_update_pkt_tx_time(conn, timestamp());
            enqueue_datagram({packet.data(), static_cast<std::size_t>(nwrite)}, ps.path);
            ++packets_this_drain;
            if (packets_this_drain >= max_packets_per_drain) {
               drain_requested = true;
               co_await asio::post(strand, asio::use_awaitable);
               if (closing || canceled || conn == nullptr) {
                  drain_requested = false;
               }
               break;
            }
         }
         complete_drain_requests(completed_generation);
      } while (drain_requested && !closing && !canceled && conn != nullptr);

      clear.reset();
      if (!closing && !canceled && conn != nullptr) {
         schedule_expiry();
         schedule_post_ngtcp2_work();
      }
   }

   boost::asio::awaitable<void> handle_packet(std::vector<std::uint8_t> packet,
                                              forge::net::transport::datagram_io::received route) {
      assert(strand.running_in_this_thread());
      co_await asio::dispatch(strand, asio::use_awaitable);
      start_cancel_request_worker();
      if (closing || canceled) {
         co_return;
      }
      const auto packet_size = packet.size();
      if (inbound_packets.size() >= limits.max_inbound_queued_packets ||
          packet_size > limits.max_inbound_queued_bytes ||
          queued_inbound_packet_bytes > limits.max_inbound_queued_bytes - packet_size) {
         metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
         fail_all();
         throw_engine(engine_error_kind::backpressure_rejected, "QUIC inbound packet queue exceeds limit");
      }
      queued_inbound_packet_bytes += packet_size;
      inbound_packets.push_back(queued_packet{.bytes = std::move(packet), .route = std::move(route)});
      if (!drain_active && !packet_processing_active) {
         co_await process_queued_packets();
      }
   }

   boost::asio::awaitable<void> process_queued_packets() {
      assert(strand.running_in_this_thread());
      co_await asio::dispatch(strand, asio::use_awaitable);
      if (packet_processing_active || drain_active) {
         co_return;
      }
      packet_processing_active = true;
      auto clear = std::unique_ptr<void, void (*)(void*)>{
          this, [](void* ptr) { static_cast<impl*>(ptr)->packet_processing_active = false; }};
      while (!inbound_packets.empty() && !closing && !canceled) {
         auto queued = std::move(inbound_packets.front());
         inbound_packets.pop_front();
         if (queued_inbound_packet_bytes >= queued.bytes.size()) {
            queued_inbound_packet_bytes -= queued.bytes.size();
         } else {
            queued_inbound_packet_bytes = 0;
         }
         auto path = make_path(queued.route.local, queued.route.remote);
         auto pi = ngtcp2_pkt_info{};
         const auto rv =
             ngtcp2_conn_read_pkt(conn, &path.path, &pi, queued.bytes.data(), queued.bytes.size(), timestamp());
         if (rv == NGTCP2_ERR_DRAINING || rv == NGTCP2_ERR_CLOSING) {
            fail_all();
            co_return;
         }
         if (rv == NGTCP2_ERR_RETRY || rv == NGTCP2_ERR_DROP_CONN) {
            report_accept_failure = false;
            fail_all();
            co_return;
         }
         if (rv != 0) {
            auto message = ngtcp2_read_error_message(conn, rv);
            message += "; packet_size=";
            message += std::to_string(queued.bytes.size());
            fail_all();
            throw_engine(engine_error_kind::internal_error, std::move(message));
         }
         metrics.packets_received.fetch_add(1, std::memory_order_relaxed);
         metrics.bytes_received.fetch_add(queued.bytes.size(), std::memory_order_relaxed);
         co_await drain_send();
      }

      clear.reset();
      schedule_post_ngtcp2_work();
   }

   void start_client_receive_loop() {
      if (receive_loop_started) {
         return;
      }
      if (self.expired()) {
         return;
      }
      receive_loop_started = true;
      spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
         try {
            while (!value->closing && !value->canceled) {
               auto packet = std::vector<std::uint8_t>(65536);
               boost::system::error_code ec;
               const auto nread = co_await value->socket->async_receive(asio::buffer(packet),
                                                                        asio::redirect_error(asio::use_awaitable, ec));
               if (ec) {
                  if (ec != asio::error::operation_aborted && !value->closing) {
                     value->fail_udp(ec);
                  }
                  co_return;
               }
               packet.resize(nread);
               co_await value->handle_packet(
                   std::move(packet),
                   {.size = nread, .remote = value->remote_endpoint, .local = value->local_endpoint_value});
            }
         } catch (...) {
            if (!value->closing && !value->canceled) {
               value->fail_all();
            }
            co_return;
         }
      });
   }

   void start_cancel_request_worker() {
      if (cancel_request_worker_started || self.expired()) {
         return;
      }
      cancel_request_worker_started = true;
      spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
         auto observed = value->termination_changed.epoch();
         while (!value->terminal_signaled.load(std::memory_order_acquire)) {
            if (value->cancellation_requested.load(std::memory_order_acquire)) {
               if (!value->closing && !value->canceled) {
                  value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                  value->fail_all();
               }
               co_return;
            }
            if (value->owner_released.load(std::memory_order_acquire)) {
               if (value->metrics.queued_bytes.load(std::memory_order_relaxed) == 0 ||
                   value->owner_drain_timer_expired) {
                  if (!value->closing && !value->canceled) {
                     value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                     value->fail_all();
                  }
                  co_return;
               }
               if (!value->owner_drain_timer_started) {
                  const auto released_at = value->owner_released_at.load(std::memory_order_acquire);
                  const auto now = timestamp();
                  const auto elapsed = now >= released_at ? now - released_at : ngtcp2_tstamp{0};
                  const auto timeout = static_cast<ngtcp2_tstamp>(
                      std::chrono::duration_cast<std::chrono::nanoseconds>(detached_write_drain_timeout).count());
                  if (elapsed >= timeout) {
                     value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                     value->fail_all();
                     co_return;
                  }
                  value->owner_drain_timer_started = true;
                  value->owner_drain_timer.expires_after(std::chrono::nanoseconds{
                      static_cast<std::chrono::nanoseconds::rep>(timeout - elapsed)});
                  value->spawn_background(
                      [](const std::shared_ptr<impl>& connection) -> asio::awaitable<void> {
                         auto error = boost::system::error_code{};
                         co_await connection->owner_drain_timer.async_wait(
                             asio::redirect_error(asio::use_awaitable, error));
                         connection->owner_drain_timer_started = false;
                         connection->owner_drain_timer_expired = !error;
                         connection->termination_changed.notify();
                      });
               }
            }
            observed = co_await value->termination_changed.async_wait(observed);
         }
      });
   }
};

namespace {

int get_new_connection_id_cb(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen,
                             void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (connection == nullptr) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   cid->datalen = cidlen;
   if (!fill_random({cid->data, cidlen})) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   if (ngtcp2_crypto_generate_stateless_reset_token(token->data, connection->reset_secret.data(),
                                                    connection->reset_secret.size(), cid) != 0) {
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   if (connection->local_connection_id_issued_hook) {
      try {
         connection->local_connection_id_issued_hook(*cid);
      } catch (...) {
         return NGTCP2_ERR_CALLBACK_FAILURE;
      }
   }
   return 0;
}

int remove_connection_id_cb(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (connection == nullptr || cid == nullptr) {
      return 0;
   }
   if (connection->local_connection_id_retired_hook) {
      try {
         connection->local_connection_id_retired_hook(*cid);
      } catch (...) {
         return NGTCP2_ERR_CALLBACK_FAILURE;
      }
   }
   return 0;
}

ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* conn_ref) {
   auto* connection = static_cast<engine_connection::impl*>(conn_ref->user_data);
   return connection->conn;
}

int handshake_completed_cb(ngtcp2_conn*, void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   try {
      if (connection->server_side && !connection->handshake_done) {
         connection->verify_peer(connection->peer_security);
      }
   } catch (const engine_failure&) {
      connection->fail_all();
      return NGTCP2_ERR_CALLBACK_FAILURE;
   } catch (...) {
      connection->fail_all();
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   if (connection->server_side && !connection->handshake_done && connection->issue_new_token) {
      try {
         connection->issue_new_token(*connection);
      } catch (...) {
         // Address-token issuance is best effort after peer verification.
      }
   }
   connection->complete_handshake();
   return 0;
}

int recv_retry_cb(ngtcp2_conn* connection, const ngtcp2_pkt_hd* header, void* user_data) {
   auto* value = static_cast<engine_connection::impl*>(user_data);
   if (value != nullptr) {
      value->metrics.retry_packets_received.fetch_add(1, std::memory_order_relaxed);
   }
   return ngtcp2_crypto_recv_retry_cb(connection, header, user_data);
}

int recv_new_token_cb(ngtcp2_conn*, const std::uint8_t* token, std::size_t token_length, void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (connection == nullptr || token == nullptr || token_length == 0 || token_length > max_client_token_bytes) {
      return 0;
   }
   try {
      connection->pending_client_token = std::vector<std::uint8_t>{token, token + token_length};
      connection->metrics.new_tokens_received.fetch_add(1, std::memory_order_relaxed);
      connection->commit_pending_client_token();
   } catch (...) {
      // A received token is an optimization and must never fail the connection.
   }
   return 0;
}

int stream_open_cb(ngtcp2_conn* conn, std::int64_t stream_id, void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   auto stream = connection->ensure_stream(stream_id);
   connection->accepted_streams.push_back(stream);
   connection->metrics.streams_accepted.fetch_add(1, std::memory_order_relaxed);
   (void)ngtcp2_conn_set_stream_user_data(conn, stream_id, stream.get());
   wake(connection->accept_stream_waiters);
   return 0;
}

int recv_stream_data_cb(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id, std::uint64_t offset,
                        const std::uint8_t* data, std::size_t datalen, void* user_data, void* stream_user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   auto stream = stream_user_data == nullptr ? connection->ensure_stream(stream_id) : connection->streams.at(stream_id);
   if (datalen > 0) {
      stream->inbound_segments[offset] = std::vector<std::uint8_t>{data, data + datalen};
      while (true) {
         auto it = stream->inbound_segments.find(stream->recv_next_offset);
         if (it == stream->inbound_segments.end()) {
            break;
         }
         stream->recv_next_offset += it->second.size();
         stream->inbound_ready.push_back(std::move(it->second));
         stream->inbound_segments.erase(it);
      }
      ngtcp2_conn_extend_max_stream_offset(conn, stream_id, datalen);
      ngtcp2_conn_extend_max_offset(conn, datalen);
      connection->metrics.bytes_received.fetch_add(datalen, std::memory_order_relaxed);
      connection->metrics.frames_received.fetch_add(1, std::memory_order_relaxed);
   }
   if ((flags & NGTCP2_STREAM_DATA_FLAG_FIN) != 0) {
      stream->remote_read_closed = true;
      connection->update_active_stream_metrics();
   }
   wake(stream->read_waiters);
   return 0;
}

int acked_stream_data_offset_cb(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t offset, std::uint64_t datalen,
                                void* user_data, void*) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   auto it = connection->streams.find(stream_id);
   if (it == connection->streams.end()) {
      return 0;
   }
   auto& stream = it->second;
   try {
      stream->acknowledged.add(offset, datalen);
      while (!stream->retained.empty()) {
         auto& write = stream->retained.front();
         if (!stream->acknowledged.covers(write.base_offset, write.data.size())) {
            break;
         }
         const auto end = write.base_offset + write.data.size();
         const auto queued_bytes = connection->metrics.queued_bytes.load(std::memory_order_relaxed);
         if (queued_bytes >= write.data.size()) {
            connection->metrics.queued_bytes.store(queued_bytes - write.data.size(), std::memory_order_relaxed);
         } else {
            connection->metrics.queued_bytes.store(0, std::memory_order_relaxed);
         }
         stream->retained.pop_front();
         stream->acknowledged.discard_before(end);
      }
      if (stream->retained.empty() && stream->outbound.empty()) {
         stream->acknowledged.clear();
      }
      if (connection->owner_released.load(std::memory_order_acquire) &&
          connection->metrics.queued_bytes.load(std::memory_order_relaxed) == 0) {
         static_cast<void>(connection->owner_drain_timer.cancel());
         connection->termination_changed.notify();
      }
   } catch (...) {
      connection->fail_all();
      return NGTCP2_ERR_CALLBACK_FAILURE;
   }
   return 0;
}

int stream_close_cb(ngtcp2_conn* conn, std::uint32_t, std::int64_t stream_id, std::uint64_t, void* user_data, void*) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (auto it = connection->streams.find(stream_id); it != connection->streams.end()) {
      auto stream = std::move(it->second);
      connection->streams.erase(it);
      connection->release_queued_stream_writes(stream);
      stream->closed = true;
      finish_stream_terminal_cleanup(stream);
      if (connection->test_failpoint) {
         try {
            static_cast<void>(connection->test_failpoint("stream_native_close_after_terminal_cleanup"));
         } catch (...) {
            // Observation only: the native callback must keep its terminal transition.
         }
      }
      stream->cancel_requested.notify();
      if (ngtcp2_is_bidi_stream(stream_id) && ngtcp2_conn_is_local_stream(conn, stream_id) == 0) {
         ngtcp2_conn_extend_max_streams_bidi(conn, 1);
      }
      wake(stream->read_waiters);
      wake(stream->write_waiters);
      connection->update_active_stream_metrics();
   }
   return 0;
}

int stream_reset_cb(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t, void* user_data, void*) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (auto it = connection->streams.find(stream_id); it != connection->streams.end()) {
      auto& stream = it->second;
      stream->remote_read_reset = true;
      if (!std::exchange(stream->reset_counted, true)) {
         connection->metrics.streams_reset.fetch_add(1, std::memory_order_relaxed);
      }
      wake(stream->read_waiters);
      connection->update_active_stream_metrics();
   }
   return 0;
}

int extend_max_local_streams_bidi_cb(ngtcp2_conn*, std::uint64_t, void* user_data) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   wake(connection->open_stream_waiters);
   return 0;
}

[[nodiscard]] ngtcp2_callbacks client_callbacks() {
   return ngtcp2_callbacks{
       .client_initial = ngtcp2_crypto_client_initial_cb,
       .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
       // Local TLS completion can precede transmission of the final flight.
       // Only confirmation below makes the client ready for publication.
       .handshake_completed = nullptr,
       .encrypt = ngtcp2_crypto_encrypt_cb,
       .decrypt = ngtcp2_crypto_decrypt_cb,
       .hp_mask = ngtcp2_crypto_hp_mask_cb,
       .recv_stream_data = recv_stream_data_cb,
       .acked_stream_data_offset = acked_stream_data_offset_cb,
       .stream_open = stream_open_cb,
       .stream_close = stream_close_cb,
       .recv_retry = recv_retry_cb,
       .extend_max_local_streams_bidi = extend_max_local_streams_bidi_cb,
       .rand = rand_cb,
       .remove_connection_id = remove_connection_id_cb,
       .update_key = ngtcp2_crypto_update_key_cb,
       .stream_reset = stream_reset_cb,
       .handshake_confirmed = handshake_completed_cb,
       .recv_new_token = recv_new_token_cb,
       .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
       .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
       .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
       .get_new_connection_id2 = get_new_connection_id_cb,
       .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
   };
}

[[nodiscard]] ngtcp2_callbacks server_callbacks() {
   return ngtcp2_callbacks{
       .recv_client_initial = ngtcp2_crypto_recv_client_initial_cb,
       .recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb,
       .handshake_completed = handshake_completed_cb,
       .encrypt = ngtcp2_crypto_encrypt_cb,
       .decrypt = ngtcp2_crypto_decrypt_cb,
       .hp_mask = ngtcp2_crypto_hp_mask_cb,
       .recv_stream_data = recv_stream_data_cb,
       .acked_stream_data_offset = acked_stream_data_offset_cb,
       .stream_open = stream_open_cb,
       .stream_close = stream_close_cb,
       .extend_max_local_streams_bidi = extend_max_local_streams_bidi_cb,
       .rand = rand_cb,
       .remove_connection_id = remove_connection_id_cb,
       .update_key = ngtcp2_crypto_update_key_cb,
       .stream_reset = stream_reset_cb,
       .handshake_confirmed = handshake_completed_cb,
       .delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb,
       .delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb,
       .version_negotiation = ngtcp2_crypto_version_negotiation_cb,
       .get_new_connection_id2 = get_new_connection_id_cb,
       .get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb,
   };
}

void configure_settings(ngtcp2_settings& settings) {
   ngtcp2_settings_default(&settings);
   settings.initial_ts = timestamp();
   settings.cc_algo = NGTCP2_CC_ALGO_CUBIC;
   settings.max_tx_udp_payload_size = max_udp_payload_size;
}

void validate_packet_buffer(ngtcp2_conn* connection) {
   if (ngtcp2_conn_get_max_tx_udp_payload_size(connection) > max_udp_payload_size) {
      throw_engine(engine_error_kind::internal_error, "QUIC packet buffer is smaller than ngtcp2 max TX UDP payload");
   }
}

void configure_params(ngtcp2_transport_params& params, const engine_transport_limits& limits,
                      std::chrono::milliseconds idle_timeout) {
   ngtcp2_transport_params_default(&params);
   params.initial_max_stream_data_bidi_local = 16 * 1024 * 1024;
   params.initial_max_stream_data_bidi_remote = 16 * 1024 * 1024;
   params.initial_max_stream_data_uni = 16 * 1024 * 1024;
   params.initial_max_data = 64 * 1024 * 1024;
   params.initial_max_streams_bidi = limits.max_streams_per_connection;
   params.initial_max_streams_uni = 16;
   params.max_udp_payload_size = max_udp_payload_size;
   params.max_idle_timeout =
       static_cast<ngtcp2_duration>(std::chrono::duration_cast<std::chrono::nanoseconds>(idle_timeout).count());
   params.active_connection_id_limit = 2;
   params.disable_active_migration = 1;
   params.grease_quic_bit = 1;
}

[[nodiscard]] ngtcp2_cid random_cid(std::size_t len = cid_length) {
   auto cid = ngtcp2_cid{};
   cid.datalen = len;
   if (!fill_random({cid.data, cid.datalen})) {
      throw_engine(engine_error_kind::tls_failed, "failed to generate QUIC connection id");
   }
   return cid;
}

void configure_client_tls(engine_connection::impl& connection, const engine_endpoint& remote,
                          const engine_client_options& options) {
   connection.peer_security = options.security;
   connection.ssl_ctx.reset(SSL_CTX_new(TLS_client_method()));
   if (!connection.ssl_ctx) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   if (options.security.verify_peer && !options.security.expected_sha256_fingerprint && !options.security.verifier) {
      SSL_CTX_set_verify(connection.ssl_ctx.get(), SSL_VERIFY_PEER, nullptr);
      configure_default_trust(connection.ssl_ctx.get(), options.security);
   } else {
      SSL_CTX_set_verify(connection.ssl_ctx.get(), SSL_VERIFY_NONE, nullptr);
   }
   if (!options.certificate_pem.empty()) {
      auto cert = load_certificate(options.certificate_pem);
      auto key = load_private_key(options.private_key_pem);
      if (SSL_CTX_use_certificate(connection.ssl_ctx.get(), cert.get()) != 1 ||
          SSL_CTX_use_PrivateKey(connection.ssl_ctx.get(), key.get()) != 1 ||
          SSL_CTX_check_private_key(connection.ssl_ctx.get()) != 1) {
         throw_engine(engine_error_kind::tls_failed, openssl_error());
      }
   }
   connection.ssl.reset(SSL_new(connection.ssl_ctx.get()));
   if (!connection.ssl) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   if (ngtcp2_crypto_ossl_init() != 0) {
      throw_engine(engine_error_kind::tls_failed, "ngtcp2 OpenSSL crypto backend initialization failed");
   }

   if (ngtcp2_crypto_ossl_ctx_new(&connection.ossl_ctx, nullptr) != 0) {
      throw_engine(engine_error_kind::tls_failed, "failed to allocate ngtcp2 OpenSSL context");
   }
   ngtcp2_crypto_ossl_ctx_set_ssl(connection.ossl_ctx, connection.ssl.get());
   if (ngtcp2_crypto_ossl_configure_client_session(connection.ssl.get()) != 0) {
      throw_engine(engine_error_kind::tls_failed,
                   "failed to configure QUIC OpenSSL client session: " + openssl_error());
   }

   connection.conn_ref = ngtcp2_crypto_conn_ref{.get_conn = get_conn_cb, .user_data = &connection};
   SSL_set_app_data(connection.ssl.get(), &connection.conn_ref);
   SSL_set_connect_state(connection.ssl.get());
   const auto alpn = length_prefixed_alpn(options.alpn);
   if (SSL_set_alpn_protos(connection.ssl.get(), alpn.data(), static_cast<unsigned>(alpn.size())) != 0) {
      throw_engine(engine_error_kind::tls_failed, "failed to set QUIC ALPN");
   }
   const auto use_sni = configure_verify_peer_name(connection.ssl.get(), remote.host);
   if (use_sni) {
      const auto* sni = remote.host.empty() ? "localhost" : remote.host.c_str();
      SSL_set_tlsext_host_name(connection.ssl.get(), const_cast<char*>(sni));
   }
}

void configure_server_tls(engine_connection::impl& connection, const engine_server_options& options) {
   connection.peer_security = options.security;
   connection.ssl_ctx.reset(SSL_CTX_new(TLS_server_method()));
   if (!connection.ssl_ctx) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   SSL_CTX_set_max_early_data(connection.ssl_ctx.get(), UINT32_MAX);

   auto cert = load_certificate(options.certificate_pem);
   auto key = load_private_key(options.private_key_pem);
   if (SSL_CTX_use_certificate(connection.ssl_ctx.get(), cert.get()) != 1 ||
       SSL_CTX_use_PrivateKey(connection.ssl_ctx.get(), key.get()) != 1 ||
       SSL_CTX_check_private_key(connection.ssl_ctx.get()) != 1) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }

   SSL_CTX_set_alpn_select_cb(connection.ssl_ctx.get(), select_alpn_cb, const_cast<std::string*>(&options.alpn));
   if (options.security.verify_peer) {
      const auto verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
      if (options.security.expected_sha256_fingerprint || options.security.verifier) {
         SSL_CTX_set_verify(connection.ssl_ctx.get(), verify_mode, accept_any_certificate_cb);
      } else {
         SSL_CTX_set_verify(connection.ssl_ctx.get(), verify_mode, nullptr);
         configure_default_trust(connection.ssl_ctx.get(), options.security);
      }
   } else {
      SSL_CTX_set_verify(connection.ssl_ctx.get(), SSL_VERIFY_NONE, nullptr);
   }

   connection.ssl.reset(SSL_new(connection.ssl_ctx.get()));
   if (!connection.ssl) {
      throw_engine(engine_error_kind::tls_failed, openssl_error());
   }
   if (ngtcp2_crypto_ossl_init() != 0) {
      throw_engine(engine_error_kind::tls_failed, "ngtcp2 OpenSSL crypto backend initialization failed");
   }
   if (ngtcp2_crypto_ossl_ctx_new(&connection.ossl_ctx, nullptr) != 0) {
      throw_engine(engine_error_kind::tls_failed, "failed to allocate ngtcp2 OpenSSL context");
   }
   ngtcp2_crypto_ossl_ctx_set_ssl(connection.ossl_ctx, connection.ssl.get());
   if (ngtcp2_crypto_ossl_configure_server_session(connection.ssl.get()) != 0) {
      throw_engine(engine_error_kind::tls_failed,
                   "failed to configure QUIC OpenSSL server session: " + openssl_error());
   }
   if (options.security.verify_peer) {
      const auto verify_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
      SSL_set_verify(connection.ssl.get(), verify_mode,
                     (options.security.expected_sha256_fingerprint || options.security.verifier)
                         ? accept_any_certificate_cb
                         : nullptr);
   } else {
      SSL_set_verify(connection.ssl.get(), SSL_VERIFY_NONE, nullptr);
   }

   connection.conn_ref = ngtcp2_crypto_conn_ref{.get_conn = get_conn_cb, .user_data = &connection};
   SSL_set_app_data(connection.ssl.get(), &connection.conn_ref);
   SSL_set_accept_state(connection.ssl.get());
}

} // namespace

engine_stream::engine_stream(std::shared_ptr<impl> impl_value) : impl_(std::move(impl_value)) {}

std::int64_t engine_stream::id() const noexcept {
   return impl_ ? impl_->id : -1;
}

boost::asio::awaitable<void> engine_stream::async_write(std::span<const std::uint8_t> bytes) {
   co_await async_write(bytes, {});
}

boost::asio::awaitable<void> engine_stream::async_write(std::span<const std::uint8_t> bytes,
                                                        std::shared_ptr<void> lifetime) {
   co_await async_write(std::vector<std::uint8_t>{bytes.begin(), bytes.end()}, std::move(lifetime));
}

boost::asio::awaitable<void> engine_stream::async_write(std::vector<std::uint8_t> bytes,
                                                        std::shared_ptr<void> lifetime) {
   if (!impl_) {
      throw_engine(engine_error_kind::stream_closed, "invalid QUIC stream");
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
   }
   co_await asio::co_spawn(
       connection->strand,
       [connection, stream = impl_, owned = std::move(bytes),
        lifetime = std::move(lifetime)]() mutable -> asio::awaitable<void> {
          if (stream->reset) {
             throw_engine(engine_error_kind::stream_reset, "QUIC stream is reset");
          }
          if (stream->local_write_closed || stream->closed) {
             throw_engine(engine_error_kind::stream_closed, "QUIC stream write side is closed");
          }
          if (owned.empty()) {
             co_return;
          }
          const auto queued = connection->metrics.queued_bytes.load(std::memory_order_relaxed);
          if (owned.size() > connection->limits.max_queued_bytes ||
              queued > connection->limits.max_queued_bytes - owned.size()) {
             connection->metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
             throw_engine(engine_error_kind::backpressure_rejected, "QUIC stream write queue exceeds max_queued_bytes");
          }
          const auto size = owned.size();
          stream->outbound.push_back(engine_stream::impl::pending_write{
              .data = std::move(owned),
              .lifetime = std::move(lifetime),
          });
          connection->metrics.queued_bytes.fetch_add(size, std::memory_order_relaxed);
          connection->metrics.frames_sent.fetch_add(1, std::memory_order_relaxed);
          connection->spawn_background(
              [](const std::shared_ptr<engine_connection::impl>& value) -> asio::awaitable<void> {
                 if (value->test_failpoint && value->test_failpoint("background_stream_drain_failure")) {
                    throw std::runtime_error{"QUIC test failpoint: background stream drain failure"};
                 }
                 try {
                    co_await value->drain_send();
                 } catch (const engine_failure&) {
                    value->fail_all();
                 }
              });
          co_await asio::post(connection->strand, asio::use_awaitable);
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::vector<std::uint8_t>> engine_stream::async_read() {
   if (!impl_) {
      throw_engine(engine_error_kind::stream_closed, "invalid QUIC stream");
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
   }
   co_return co_await asio::co_spawn(
       connection->strand,
       [connection, stream = impl_]() -> asio::awaitable<std::vector<std::uint8_t>> {
          while (stream->inbound_ready.empty() && !stream->remote_read_closed && !stream->remote_read_reset &&
                 !stream->reset && !stream->closed && !connection->closing && !connection->canceled) {
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             stream->read_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (!stream->inbound_ready.empty()) {
             auto out = std::move(stream->inbound_ready.front());
             stream->inbound_ready.pop_front();
             co_return out;
          }
          if (stream->reset || stream->remote_read_reset) {
             throw_engine(engine_error_kind::stream_reset, "QUIC stream was reset while reading");
          }
          if (connection->canceled) {
             throw_engine(engine_error_kind::canceled, "QUIC connection was canceled while reading");
          }
          if (connection->closing) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed while reading");
          }
          throw_engine(engine_error_kind::stream_closed, "QUIC stream read side is closed");
       },
       asio::use_awaitable);
}

boost::asio::awaitable<void> engine_stream::async_close() {
   if (!impl_) {
      co_return;
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      co_return;
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   auto primary_error = std::exception_ptr{};
   try {
      if (connection->test_failpoint && connection->test_failpoint("stream_close_before_owner_spawn")) {
         throw std::bad_alloc{};
      }
      co_await asio::co_spawn(
          connection->strand,
          [connection, stream = impl_]() -> asio::awaitable<void> {
             auto error = std::exception_ptr{};
             try {
                if (connection->test_failpoint && connection->test_failpoint("stream_close_before_state_check")) {
                   throw std::bad_alloc{};
                }
                if (connection->closing || connection->canceled) {
                   co_return;
                }
                if (!stream->local_write_closed && !stream->reset && !stream->closed) {
                   if (!stream->fin_queued) {
                      if (connection->test_failpoint && connection->test_failpoint("stream_close_before_fin")) {
                         throw std::bad_alloc{};
                      }
                      stream->outbound.push_back(engine_stream::impl::pending_write{.fin = true});
                      stream->fin_queued = true;
                   }
                   co_await connection->drain_send();
                   while (!stream->local_write_closed && !stream->reset && !stream->closed && !connection->closing &&
                          !connection->canceled) {
                      auto timer = std::make_shared<asio::steady_timer>(connection->strand);
                      timer->expires_after(std::chrono::minutes{10});
                      stream->write_waiters.emplace_back(timer);
                      auto ec = boost::system::error_code{};
                      co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
                   }
                   connection->update_active_stream_metrics();
                }
             } catch (...) {
                error = std::current_exception();
             }
             if (error) {
                co_await connection->async_reset_stream_after_close_failure(stream);
                std::rethrow_exception(error);
             }
          },
          asio::use_awaitable);
   } catch (...) {
      primary_error = std::current_exception();
   }
   if (primary_error) {
      // The cancel worker is started before an engine stream is published.
      // This non-throwing handoff also covers a failure starting co_spawn.
      impl_->cancel_requested.notify();
      co_await wait_for_stream_terminal_cleanup(impl_);
      std::rethrow_exception(primary_error);
   }
   // A concurrent cancel worker may already own the native stream reset.
   // Success is observable only after that owner publishes its terminal drain.
   if (connection->test_failpoint) {
      static_cast<void>(connection->test_failpoint("stream_close_before_terminal_join"));
   }
   co_await wait_for_stream_terminal_cleanup(impl_);
}

void engine_stream::cancel_write() {
   if (!impl_) {
      return;
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      return;
   }
   auto stream = impl_;
   asio::dispatch(connection->strand, [connection, stream] {
      if (stream->local_write_closed || stream->reset || stream->closed) {
         return;
      }
      auto cleanup = claim_stream_terminal_owner(stream);
      auto shutdown_result = 0;
      auto should_drain = false;
      if (connection->conn != nullptr && !connection->closing && !connection->canceled) {
         shutdown_result = ngtcp2_conn_shutdown_stream_write(connection->conn, 0, stream->id, 0);
         should_drain = shutdown_result == 0;
      }
      connection->release_queued_stream_writes(stream);
      stream->local_write_closed = true;
      publish_stream_terminal(stream);
      wake(stream->write_waiters);
      if (!std::exchange(stream->reset_counted, true)) {
         connection->metrics.streams_reset.fetch_add(1, std::memory_order_relaxed);
      }
      connection->update_active_stream_metrics();
      if (shutdown_result != 0) {
         connection->fail_all();
         return;
      }
      if (!should_drain) {
         return;
      }
      // The pair destroys the claim before its strong stream owner, including
      // when spawn_background drops the work before invoking the coroutine.
      connection->spawn_background([owned = std::pair{stream, std::move(cleanup)}](
                                       const std::shared_ptr<engine_connection::impl>& value) mutable -> asio::awaitable<void> {
         try {
            co_await value->drain_send();
         } catch (const engine_failure&) {
            value->fail_all();
         }
         owned.second.reset();
      });
   });
}

void engine_stream::cancel() {
   request_cancel();
}

void engine_stream::request_cancel() noexcept {
   if (!impl_) {
      return;
   }
   impl_->cancel_requested.notify();
}

engine_connection::engine_connection(std::shared_ptr<impl> impl_value) : impl_(std::move(impl_value)) {}

engine_connection::~engine_connection() {
   if (impl_) {
      impl_->owner_released_at.store(timestamp(), std::memory_order_release);
      impl_->owner_released.store(true, std::memory_order_release);
      impl_->termination_changed.notify();
   }
}

engine_connection_metrics engine_connection::metrics() const {
   return impl_ ? impl_->metrics.snapshot() : engine_connection_metrics{};
}

engine_endpoint engine_connection::local_endpoint() const {
   return impl_ ? from_udp_endpoint(impl_->local_endpoint()) : engine_endpoint{};
}

engine_endpoint engine_connection::remote_endpoint() const {
   return impl_ ? from_udp_endpoint(impl_->remote_endpoint) : engine_endpoint{};
}

std::optional<engine_peer_certificate> engine_connection::peer_certificate() const {
   return impl_ ? impl_->peer_certificate_value : std::nullopt;
}

std::shared_ptr<void> engine_connection::take_inbound_admission() noexcept {
   if (!impl_) {
      return {};
   }
   auto lock = std::scoped_lock{impl_->inbound_admission_mutex};
   return std::move(impl_->inbound_admission);
}

boost::asio::awaitable<std::shared_ptr<engine_stream>> engine_connection::async_open_stream() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC connection");
   }
   co_return co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_]() -> asio::awaitable<std::shared_ptr<engine_stream>> {
          auto cancellation = co_await asio::this_coro::cancellation_state;
          if (connection->closing || connection->canceled) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
          }
          if (connection->active_stream_count() >= connection->limits.max_streams_per_connection) {
             connection->metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
             throw_engine(engine_error_kind::backpressure_rejected, "QUIC max streams exceeded");
          }
          auto stream = std::make_shared<engine_stream::impl>(-1);
          stream->connection = connection;
          auto stream_id = std::int64_t{-1};
          while (true) {
             if (cancellation.cancelled() != asio::cancellation_type::none) {
                connection->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                throw_engine(engine_error_kind::canceled, "QUIC stream open canceled");
             }
             const auto rv = ngtcp2_conn_open_bidi_stream(connection->conn, &stream_id, stream.get());
             if (rv == 0) {
                break;
             }
             if (rv != NGTCP2_ERR_STREAM_ID_BLOCKED) {
                throw_engine(engine_error_kind::backpressure_rejected,
                             std::string{"ngtcp2_conn_open_bidi_stream failed: "} + ngtcp2_strerror(rv));
             }
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             std::erase_if(connection->open_stream_waiters, [](const auto& weak) { return weak.expired(); });
             connection->open_stream_waiters.emplace_back(timer);
             auto error = boost::system::error_code{};
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
             remove_waiter(connection->open_stream_waiters, timer);
             if (cancellation.cancelled() != asio::cancellation_type::none) {
                connection->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                throw_engine(engine_error_kind::canceled, "QUIC stream open canceled while waiting for credit");
             }
             if (connection->closing || connection->canceled) {
                throw_engine(engine_error_kind::connection_closed,
                             "QUIC connection closed while waiting for stream credit");
             }
          }

          // Once ngtcp2 allocates the stream ID, completion owns that stream. Make
          // cancellation completion-wins so no unreturned native stream can leak.
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          stream->id = stream_id;
          connection->streams.emplace(stream_id, stream);
          connection->update_active_stream_metrics();
          connection->metrics.streams_opened.fetch_add(1, std::memory_order_relaxed);
          auto cancel_worker_failure = std::exception_ptr{};
          try {
             connection->start_stream_cancel_worker(stream);
          } catch (...) {
             cancel_worker_failure = std::current_exception();
          }
          if (cancel_worker_failure) {
             co_await connection->async_reset_stream_after_close_failure(stream);
             std::rethrow_exception(cancel_worker_failure);
          }
          co_await connection->drain_send();
          if (connection->closing || connection->canceled) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed while opening stream");
          }
          co_return std::shared_ptr<engine_stream>{new engine_stream{std::move(stream)}};
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::shared_ptr<engine_stream>> engine_connection::async_accept_stream() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC connection");
   }
   co_return co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_]() -> asio::awaitable<std::shared_ptr<engine_stream>> {
          while (connection->accepted_streams.empty() && !connection->closing && !connection->canceled) {
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             connection->accept_stream_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (connection->accepted_streams.empty()) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed before accepting stream");
          }
          auto stream = std::move(connection->accepted_streams.front());
          connection->accepted_streams.pop_front();
          auto cancel_worker_failure = std::exception_ptr{};
          try {
             connection->start_stream_cancel_worker(stream);
          } catch (...) {
             cancel_worker_failure = std::current_exception();
          }
          if (cancel_worker_failure) {
             co_await connection->async_reset_stream_after_close_failure(stream);
             std::rethrow_exception(cancel_worker_failure);
          }
          co_return std::shared_ptr<engine_stream>{new engine_stream{std::move(stream)}};
       },
       asio::use_awaitable);
}

boost::asio::awaitable<void> engine_connection::async_close() {
   if (!impl_) {
      co_return;
   }
   co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_]() -> asio::awaitable<void> {
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          if (connection->close_started) {
             co_await connection->wait_close_cleanup();
             co_return;
          }
          connection->close_started = true;
          auto primary_error = std::exception_ptr{};
          if (!connection->terminal_cleanup_complete) {
             connection->metrics.connections_closed.fetch_add(1, std::memory_order_relaxed);
             connection->closing = true;
             connection->signal_terminal();
             try {
                if (connection->test_failpoint) {
                   static_cast<void>(connection->test_failpoint("async_close_before_send"));
                }
                if (connection->conn != nullptr) {
                   auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
                   auto path = ngtcp2_path_storage{};
                   ngtcp2_path_storage_zero(&path);
                   auto packet_info = ngtcp2_pkt_info{};
                   auto close_error = ngtcp2_ccerr{};
                   ngtcp2_ccerr_default(&close_error);
                   ngtcp2_ccerr_set_application_error(&close_error, 0, nullptr, 0);
                   const auto written = ngtcp2_conn_write_connection_close(
                       connection->conn, &path.path, &packet_info, packet.data(), packet.size(), &close_error, timestamp());
                   if (written > 0) {
                      const auto packet_size = static_cast<std::size_t>(written);
                      const auto send_error = co_await connection->send_packet(
                          {.bytes = {packet.begin(), packet.begin() + written}, .route = copy_route(path.path)}, true);
                      if (!send_error) {
                         connection->metrics.packets_sent.fetch_add(1, std::memory_order_relaxed);
                         connection->metrics.bytes_sent.fetch_add(packet_size, std::memory_order_relaxed);
                      }
                   }
                }
                connection->close_transport(!connection->server_side);
             } catch (...) {
                primary_error = std::current_exception();
                connection->fail_all();
             }
          }
          try {
             co_await connection->wait_background_idle();
          } catch (...) {
             if (!primary_error) {
                primary_error = std::current_exception();
             }
             connection->fail_all();
          }
          connection->complete_close(primary_error);
          if (primary_error) {
             std::rethrow_exception(primary_error);
          }
       },
       asio::use_awaitable);
}

void engine_connection::cancel() {
   if (!impl_) {
      return;
   }
   asio::post(impl_->strand, [impl = impl_] {
      impl->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
      impl->fail_all();
   });
}

void engine_connection::request_cancel() noexcept {
   if (impl_) {
      impl_->cancellation_requested.store(true, std::memory_order_release);
      impl_->termination_changed.notify();
   }
}

struct engine_connector::impl {
   struct active_connect {
      enum class state_value : std::uint8_t {
         pending,
         completed,
         timed_out,
         canceled,
      };

      std::atomic<state_value> state{state_value::pending};
      std::mutex mutex;
      std::shared_ptr<udp::resolver> resolver;
      forge::asio::notification resolution_changed;
      std::optional<udp::resolver::results_type> resolution_results;
      boost::system::error_code resolution_error;
      bool resolution_completed = false;
      std::weak_ptr<udp::socket> socket;
      std::weak_ptr<engine_connection::impl> connection;

      [[nodiscard]] bool mark_timed_out() noexcept {
         auto expected = state_value::pending;
         return state.compare_exchange_strong(expected, state_value::timed_out, std::memory_order_acq_rel);
      }

      [[nodiscard]] bool mark_canceled() noexcept {
         auto expected = state_value::pending;
         return state.compare_exchange_strong(expected, state_value::canceled, std::memory_order_acq_rel);
      }

      [[nodiscard]] bool finish() noexcept {
         auto expected = state_value::pending;
         return state.compare_exchange_strong(expected, state_value::completed, std::memory_order_acq_rel);
      }

      [[nodiscard]] bool timed_out() const noexcept {
         return state.load(std::memory_order_acquire) == state_value::timed_out;
      }

      [[nodiscard]] bool canceled() const noexcept {
         return state.load(std::memory_order_acquire) == state_value::canceled;
      }

      void complete_resolution(boost::system::error_code error, udp::resolver::results_type results) noexcept {
         {
            auto lock = std::scoped_lock{mutex};
            resolver.reset();
            resolution_error = error;
            try {
               resolution_results.emplace(std::move(results));
            } catch (...) {
               resolution_error = asio::error::no_memory;
               resolution_results.reset();
            }
            resolution_completed = true;
         }
         resolution_changed.notify();
      }

      [[nodiscard]] bool take_resolution(boost::system::error_code& error, udp::resolver::results_type& results) {
         auto lock = std::scoped_lock{mutex};
         if (!resolution_completed) {
            return false;
         }
         error = resolution_error;
         if (resolution_results) {
            results = std::move(*resolution_results);
            resolution_results.reset();
         }
         return true;
      }

      void release_resolver() noexcept {
         auto lock = std::scoped_lock{mutex};
         resolver.reset();
      }

      static void cancel_resolver(const std::shared_ptr<udp::resolver>& value) noexcept {
         if (!value) {
            return;
         }
         try {
            asio::dispatch(value->get_executor(), [value] {
               try {
                  value->cancel();
               } catch (...) {
                  // A terminal result is still authoritative if resolver cancellation fails.
               }
            });
         } catch (...) {
            // A terminal result is still authoritative if executor teardown rejects dispatch.
         }
      }

      static void cancel_socket(const std::shared_ptr<udp::socket>& value) noexcept {
         if (!value) {
            return;
         }
         try {
            asio::post(value->get_executor(), [value] {
               auto ignored = boost::system::error_code{};
               value->cancel(ignored);
               value->close(ignored);
            });
         } catch (...) {
            // A terminal result is still authoritative if executor teardown rejects the post.
         }
      }

      void cancel_io() noexcept {
         if (!mark_canceled()) {
            return;
         }
         auto resolver_value = std::shared_ptr<udp::resolver>{};
         auto socket_value = std::shared_ptr<udp::socket>{};
         auto connection_value = std::shared_ptr<engine_connection::impl>{};
         {
            auto lock = std::scoped_lock{mutex};
            resolver_value = resolver;
            socket_value = socket.lock();
            connection_value = connection.lock();
         }
         cancel_resolver(resolver_value);
         if (connection_value) {
            try {
               asio::post(connection_value->strand, [connection_value] {
                  connection_value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                  connection_value->fail_all();
               });
            } catch (...) {
               // A terminal result is still authoritative if executor teardown rejects the post.
            }
         } else if (socket_value) {
            cancel_socket(socket_value);
         }
         resolution_changed.notify();
      }

      void timeout_io() noexcept {
         if (!mark_timed_out()) {
            return;
         }
         auto resolver_value = std::shared_ptr<udp::resolver>{};
         auto socket_value = std::shared_ptr<udp::socket>{};
         auto connection_value = std::shared_ptr<engine_connection::impl>{};
         {
            auto lock = std::scoped_lock{mutex};
            resolver_value = resolver;
            socket_value = socket.lock();
            connection_value = connection.lock();
         }
         cancel_resolver(resolver_value);
         if (connection_value) {
            try {
               asio::post(connection_value->strand, [connection_value] {
                  connection_value->metrics.timeouts.fetch_add(1, std::memory_order_relaxed);
                  connection_value->fail_all();
               });
            } catch (...) {
               // A terminal result is still authoritative if executor teardown rejects the post.
            }
         } else if (socket_value) {
            cancel_socket(socket_value);
         }
         resolution_changed.notify();
      }
   };

   explicit impl(boost::asio::io_context& context_value) : context(context_value) {}

   [[nodiscard]] bool valid() const noexcept {
      return !canceled.load(std::memory_order_acquire);
   }

   [[nodiscard]] std::shared_ptr<active_connect> track_connect(std::shared_ptr<udp::resolver> resolver) {
      auto connect = std::make_shared<active_connect>();
      {
         auto lock = std::scoped_lock{connect->mutex};
         connect->resolver = std::move(resolver);
      }
      auto lock = std::scoped_lock{mutex};
      if (!valid()) {
         throw_engine(engine_error_kind::canceled, "QUIC connector is canceled");
      }
      active.erase(std::remove_if(active.begin(), active.end(), [](const auto& value) { return value.expired(); }),
                   active.end());
      active.push_back(connect);
      return connect;
   }

   void cancel() {
      auto connections = std::vector<std::shared_ptr<active_connect>>{};
      {
         auto lock = std::scoped_lock{mutex};
         canceled.store(true, std::memory_order_release);
         connections.reserve(active.size());
         for (auto& value : active) {
            if (auto connect = value.lock()) {
               connections.push_back(std::move(connect));
            }
         }
      }
      for (auto& connect : connections) {
         connect->cancel_io();
      }
   }

   boost::asio::io_context& context;
   std::mutex mutex;
   std::vector<std::weak_ptr<active_connect>> active;
   std::atomic_bool canceled = false;
};

engine_connector::engine_connector(boost::asio::io_context& context) : impl_(std::make_shared<impl>(context)) {}

boost::asio::awaitable<std::shared_ptr<engine_connection>>
engine_connector::async_connect(engine_endpoint remote, engine_client_options options) {
   if (!impl_ || !impl_->valid()) {
      throw_engine(engine_error_kind::canceled, "QUIC connector is canceled");
   }
   const auto executor = co_await asio::this_coro::executor;
   const auto inherited_cancellation = co_await asio::this_coro::cancellation_state;
   const auto connect_started = std::chrono::steady_clock::now();
   auto resolver = std::make_shared<udp::resolver>(executor);
   auto connect_timer = std::make_shared<asio::steady_timer>(executor);
   auto active_connect = impl_->track_connect(resolver);
   auto cancellation_slot = inherited_cancellation.slot();
   if (cancellation_slot.is_connected()) {
      cancellation_slot.assign([active_connect](asio::cancellation_type) { active_connect->cancel_io(); });
   }
   const auto clear_cancellation_slot = [](asio::cancellation_slot* slot) noexcept { slot->clear(); };
   auto cancellation_slot_cleanup = std::unique_ptr<asio::cancellation_slot, decltype(clear_cancellation_slot)>{
       &cancellation_slot, clear_cancellation_slot};
   connect_timer->expires_at(connect_started + options.connect_timeout);
   connect_timer->async_wait([connect_timer, active_connect](boost::system::error_code ec) {
      if (ec) {
         return;
      }
      active_connect->timeout_io();
   });
   const auto throw_if_terminal = [&] {
      if (inherited_cancellation.cancelled() != asio::cancellation_type::none) {
         active_connect->cancel_io();
      }
      if (active_connect->canceled()) {
         connect_timer->cancel();
         active_connect->release_resolver();
         throw_engine(engine_error_kind::canceled, "QUIC client connect canceled");
      }
      if (active_connect->timed_out()) {
         connect_timer->cancel();
         active_connect->release_resolver();
         throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
      }
   };
   const auto request_inherited_cancellation = [&] {
      if (inherited_cancellation.cancelled() != asio::cancellation_type::none) {
         active_connect->cancel_io();
      }
   };
   const auto throw_if_timed_out = [&] {
      if (active_connect->timed_out()) {
         connect_timer->cancel();
         active_connect->release_resolver();
         throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
      }
   };
   auto finish_connect_or_throw = [&] {
      if (inherited_cancellation.cancelled() != asio::cancellation_type::none) {
         active_connect->cancel_io();
      }
      if (connect_failpoint_enabled(options, "timeout_before_pre_connection_error_finish")) {
         (void)active_connect->mark_timed_out();
      }
      if (!active_connect->finish()) {
         connect_timer->cancel();
         if (active_connect->canceled()) {
            throw_engine(engine_error_kind::canceled, "QUIC client connect canceled");
         }
         throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
      }
      connect_timer->cancel();
   };

   throw_if_terminal();
   try {
      auto literal_error = boost::system::error_code{};
      static_cast<void>(asio::ip::make_address(remote.host, literal_error));
      if (!literal_error || !remote.zone.empty() || remote.host.find(':') != std::string::npos ||
          remote.host.find('%') != std::string::npos) {
         const auto address = literal_address(remote);
         if ((remote.family == engine_endpoint::address_family::ipv4 && !address.is_v4()) ||
             (remote.family == engine_endpoint::address_family::ipv6 && !address.is_v6())) {
            throw_engine(engine_error_kind::invalid_endpoint, "QUIC literal address family mismatch");
         }
         active_connect->complete_resolution(
             {}, udp::resolver::results_type::create(udp::endpoint{address, remote.port}, remote.host,
                                                     std::to_string(remote.port)));
      } else {
         switch (remote.family) {
         case engine_endpoint::address_family::any:
            resolver->async_resolve(
                remote.host, std::to_string(remote.port),
                [active_connect](boost::system::error_code error, udp::resolver::results_type results) mutable {
                   active_connect->complete_resolution(error, std::move(results));
                });
            break;
         case engine_endpoint::address_family::ipv4:
            resolver->async_resolve(
                udp::v4(), remote.host, std::to_string(remote.port),
                [active_connect](boost::system::error_code error, udp::resolver::results_type results) mutable {
                   active_connect->complete_resolution(error, std::move(results));
                });
            break;
         case engine_endpoint::address_family::ipv6:
            resolver->async_resolve(
                udp::v6(), remote.host, std::to_string(remote.port),
                [active_connect](boost::system::error_code error, udp::resolver::results_type results) mutable {
                   active_connect->complete_resolution(error, std::move(results));
                });
            break;
         }
      }
   } catch (...) {
      active_connect->release_resolver();
      throw_if_terminal();
      finish_connect_or_throw();
      throw;
   }

   auto resolution_error = boost::system::error_code{};
   auto resolution_results = udp::resolver::results_type{};
   auto observed_resolution = active_connect->resolution_changed.epoch();
   while (!active_connect->take_resolution(resolution_error, resolution_results)) {
      request_inherited_cancellation();
      // getaddrinfo may not be interruptible after it starts. Timeout owns a callback-safe state and returns now;
      // inherited cancellation instead joins the resolver callback before reporting cancellation to its caller.
      throw_if_timed_out();
      observed_resolution = co_await asio::co_spawn(
          executor,
          [active_connect, observed_resolution]() -> asio::awaitable<forge::asio::notification::epoch_type> {
             co_return co_await active_connect->resolution_changed.async_wait(observed_resolution);
          },
          asio::bind_cancellation_slot(asio::cancellation_slot{}, asio::use_awaitable));
   }

   // Terminal cancellation wakes this loop, but completion stays owned until the resolver reports back.
   static_cast<void>(connect_failpoint_enabled(options, "after_resolution_completion"));
   throw_if_terminal();
   if (resolution_error || resolution_results.empty()) {
      finish_connect_or_throw();
      throw_engine(engine_error_kind::invalid_endpoint,
                   "failed to resolve QUIC endpoint: " + resolution_error.message());
   }
   auto remote_endpoint = *resolution_results.begin();
   auto ec = boost::system::error_code{};
   auto socket = std::make_shared<udp::socket>(impl_->context);
   socket->open(remote_endpoint.endpoint().protocol(), ec);
   if (ec) {
      finish_connect_or_throw();
      throw_engine(engine_error_kind::internal_error, "failed to open QUIC UDP socket: " + ec.message());
   }
   socket->bind(udp::endpoint{remote_endpoint.endpoint().protocol(), 0}, ec);
   if (ec) {
      finish_connect_or_throw();
      throw_engine(engine_error_kind::internal_error, "failed to bind QUIC UDP socket: " + ec.message());
   }
   // A dedicated client owns a fixed remote tuple for its whole lifetime.
   // Connecting this same FD selects a concrete local address before ngtcp2
   // records its path, and lets the kernel filter datagrams from other peers.
   socket->connect(remote_endpoint.endpoint(), ec);
   if (ec) {
      finish_connect_or_throw();
      throw_engine(engine_error_kind::invalid_endpoint, "failed to connect QUIC UDP socket: " + ec.message());
   }
   const auto local_endpoint = socket->local_endpoint(ec);
   if (ec || local_endpoint.address().is_unspecified() ||
       local_endpoint.protocol() != remote_endpoint.endpoint().protocol()) {
      finish_connect_or_throw();
      throw_engine(engine_error_kind::internal_error, "failed to read QUIC UDP socket endpoint: " + ec.message());
   }
   if (active_connect->canceled()) {
      connect_timer->cancel();
      throw_engine(engine_error_kind::canceled, "QUIC client connect canceled");
   }
   if (active_connect->timed_out()) {
      connect_timer->cancel();
      throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
   }

   auto connection_impl = std::make_shared<engine_connection::impl>(impl_->context, socket, local_endpoint,
                                                                    remote_endpoint.endpoint(), options.limits);
   // Retain the opaque owner in this operation as well: fail_all() releases
   // native admission before the asynchronous background-work join completes.
   connection_impl->inbound_admission = options.connection_lifetime;
   connection_impl->self = connection_impl;
   connection_impl->test_failpoint = options.test_failpoint;
   connection_impl->metrics.connections_opened.store(1, std::memory_order_relaxed);
   connection_impl->metrics.handshakes_started.store(1, std::memory_order_relaxed);
   connection_impl->server_side = false;
   if (options.client_tokens && options.client_tokens->store) {
      connection_impl->client_token_store = options.client_tokens->store;
   }
   {
      auto lock = std::scoped_lock{active_connect->mutex};
      active_connect->socket = socket;
      active_connect->connection = connection_impl;
   }
   auto connect_error = std::exception_ptr{};
   auto handshake_limited_by_connect_deadline = false;
   try {
      throw_if_terminal();
      co_await asio::co_spawn(
          connection_impl->strand,
          [&]() -> asio::awaitable<void> {
             auto callbacks = client_callbacks();
             auto settings = ngtcp2_settings{};
             auto params = ngtcp2_transport_params{};
             configure_settings(settings);
             configure_params(params, options.limits, options.idle_timeout);

             const auto dcid = random_cid(NGTCP2_MIN_INITIAL_DCIDLEN);
             const auto scid = random_cid(cid_length);
             auto path = make_path(local_endpoint, remote_endpoint.endpoint());
             auto initial_token = std::vector<std::uint8_t>{};
             if (options.client_tokens && options.client_tokens->take) {
                try {
                   if (auto token = options.client_tokens->take();
                       token && !token->empty() && token->size() <= max_client_token_bytes) {
                      initial_token = std::move(*token);
                   }
                } catch (...) {
                   // Token cache failures must not affect a connection attempt.
                }
             }
             if (!initial_token.empty()) {
                settings.token = initial_token.data();
                settings.tokenlen = initial_token.size();
                settings.token_type = NGTCP2_TOKEN_TYPE_NEW_TOKEN;
             }
             const auto rv =
                 ngtcp2_conn_client_new(&connection_impl->conn, &dcid, &scid, &path.path, NGTCP2_PROTO_VER_V1,
                                        &callbacks, &settings, &params, nullptr, connection_impl.get());
             if (rv != 0) {
                throw_engine(engine_error_kind::internal_error,
                             std::string{"ngtcp2_conn_client_new failed: "} + ngtcp2_strerror(rv));
             }
             validate_packet_buffer(connection_impl->conn);

             configure_client_tls(*connection_impl, remote, options);
             ngtcp2_conn_set_tls_native_handle(connection_impl->conn, connection_impl->ossl_ctx);
             connection_impl->start_client_receive_loop();
             connection_impl->spawn_background(
                 [](const std::shared_ptr<engine_connection::impl>& value) -> asio::awaitable<void> {
                    try {
                       co_await value->drain_send();
                    } catch (const engine_failure&) {
                       value->fail_all();
                    }
                 });
             const auto remaining_connect_timeout = remaining_timeout_budget(connect_started, options.connect_timeout);
             if (remaining_connect_timeout.count() <= 0) {
                throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
             }
             handshake_limited_by_connect_deadline = remaining_connect_timeout < options.handshake_timeout;
             co_await connection_impl->wait_handshake(std::min(options.handshake_timeout, remaining_connect_timeout));
             if (active_connect->canceled()) {
                throw_engine(engine_error_kind::canceled, "QUIC client connect canceled");
             }
             if (active_connect->timed_out()) {
                throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
             }
             connection_impl->verify_selected_alpn(options.alpn);
             connection_impl->verify_peer(options.security);
             connection_impl->client_token_store_verified = true;
             connection_impl->commit_pending_client_token();
          },
          asio::use_awaitable);
      finish_connect_or_throw();
   } catch (const engine_failure& error) {
      request_inherited_cancellation();
      if (active_connect->canceled()) {
         connect_error =
             std::make_exception_ptr(engine_failure{engine_error_kind::canceled, "QUIC client connect canceled"});
      } else if (active_connect->timed_out() ||
                 (error.kind() == engine_error_kind::handshake_timeout && handshake_limited_by_connect_deadline)) {
         connect_error = std::make_exception_ptr(
             engine_failure{engine_error_kind::connect_timeout, "QUIC client connect timed out"});
      } else {
         connect_error = std::current_exception();
      }
   } catch (...) {
      connect_error = std::current_exception();
   }
   if (connect_error) {
      (void)active_connect->finish();
      connect_timer->cancel();
      // The slot belongs to the old cancellation state; clear it before reset.
      // Terminal cleanup must run even when cancellation caused the failure.
      cancellation_slot_cleanup.reset();
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      co_await asio::co_spawn(
          connection_impl->strand,
          [connection_impl]() -> asio::awaitable<void> {
             connection_impl->fail_all();
             co_await connection_impl->wait_background_idle();
          },
          asio::use_awaitable);
      std::rethrow_exception(connect_error);
   }
   co_return std::shared_ptr<engine_connection>{new engine_connection{std::move(connection_impl)}};
}

void engine_connector::cancel() {
   if (impl_) {
      impl_->cancel();
   }
}

struct engine_listener::impl {
   impl(boost::asio::io_context& context_value, engine_endpoint endpoint_value, engine_server_options options_value)
       : context(context_value), strand(asio::make_strand(context_value)),
         server_socket(std::make_shared<server_udp_socket>(strand)), bind_endpoint(std::move(endpoint_value)),
         options(std::move(options_value)) {}

   boost::asio::io_context& context;
   asio::strand<asio::io_context::executor_type> strand;
   std::shared_ptr<server_udp_socket> server_socket;
   engine_endpoint bind_endpoint;
   engine_server_options options;
   stateless_reset_secret reset_secret = random_stateless_reset_secret();
   initial_token_validator initial_tokens{random_initial_token_secret(), retry_token_lifetime, regular_token_lifetime};
   std::mutex cid_mutex;
   std::unordered_map<std::string, std::shared_ptr<engine_connection::impl>> connections_by_cid;
   std::unordered_map<engine_connection::impl*, std::vector<std::string>> cids_by_connection;
   std::deque<std::shared_ptr<engine_connection>> accepted;
   std::vector<std::weak_ptr<asio::steady_timer>> accept_waiters;
   std::optional<engine_error_kind> pending_accept_error;
   std::string pending_accept_failure_text;
   std::weak_ptr<impl> self;
   std::vector<std::weak_ptr<asio::steady_timer>> operation_waiters;
   mutable std::mutex shutdown_mutex;
   std::vector<std::shared_ptr<asio::steady_timer>> shutdown_waiters;
   std::exception_ptr shutdown_error;
   bool stopped = false;
   bool receive_started = false;
   bool shutdown_started = false;
   bool shutdown_complete = false;
   std::size_t active_operations = 0;

   enum class shutdown_action : std::uint8_t {
      run,
      wait,
      done,
   };

   [[nodiscard]] std::vector<std::shared_ptr<engine_connection::impl>> connections() {
      auto out = std::vector<std::shared_ptr<engine_connection::impl>>{};
      const auto append = [&](std::shared_ptr<engine_connection::impl> connection) {
         if (connection &&
             std::ranges::none_of(out, [&](const auto& current) { return current.get() == connection.get(); })) {
            out.push_back(std::move(connection));
         }
      };
      {
         auto lock = std::scoped_lock{cid_mutex};
         out.reserve(cids_by_connection.size() + accepted.size());
         for (const auto& [_, keys] : cids_by_connection) {
            if (keys.empty()) {
               continue;
            }
            if (auto it = connections_by_cid.find(keys.front()); it != connections_by_cid.end()) {
               append(it->second);
            }
         }
      }
      for (const auto& connection : accepted) {
         if (connection) {
            append(connection->impl_);
         }
      }
      return out;
   }

   void finish_operation() {
      if (active_operations == 0) {
         return;
      }
      --active_operations;
      if (active_operations == 0) {
         wake(operation_waiters);
      }
   }

   boost::asio::awaitable<void> wait_operations_idle() {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      while (active_operations != 0) {
         auto timer = std::make_shared<asio::steady_timer>(strand);
         timer->expires_after(std::chrono::minutes{10});
         operation_waiters.emplace_back(timer);
         boost::system::error_code ec;
         co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
         co_await asio::dispatch(strand, asio::use_awaitable);
      }
   }

   [[nodiscard]] shutdown_action begin_shutdown() {
      auto lock = std::scoped_lock{shutdown_mutex};
      if (shutdown_complete) {
         return shutdown_action::done;
      }
      if (shutdown_started) {
         return shutdown_action::wait;
      }
      shutdown_started = true;
      return shutdown_action::run;
   }

   [[nodiscard]] std::exception_ptr shutdown_failure() const {
      auto lock = std::scoped_lock{shutdown_mutex};
      return shutdown_error;
   }

   boost::asio::awaitable<void> wait_shutdown_complete() {
      co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
      for (;;) {
         auto timer = std::make_shared<asio::steady_timer>(strand);
         timer->expires_at(asio::steady_timer::time_point::max());
         auto ready = false;
         {
            auto lock = std::scoped_lock{shutdown_mutex};
            ready = shutdown_complete;
            if (!ready) {
               shutdown_waiters.push_back(timer);
            }
         }
         if (ready) {
            co_return;
         }
         boost::system::error_code ec;
         co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
      }
   }

   void finish_shutdown(std::exception_ptr error = {}) noexcept {
      auto ready = std::vector<std::shared_ptr<asio::steady_timer>>{};
      {
         auto lock = std::scoped_lock{shutdown_mutex};
         if (shutdown_complete) {
            return;
         }
         shutdown_error = std::move(error);
         shutdown_complete = true;
         ready.swap(shutdown_waiters);
      }
      for (const auto& timer : ready) {
         wake(timer);
      }
   }

   void clear_connection_registry() {
      {
         auto lock = std::scoped_lock{cid_mutex};
         connections_by_cid.clear();
         cids_by_connection.clear();
      }
      accepted.clear();
      pending_accept_error.reset();
      pending_accept_failure_text.clear();
   }

   void stop() {
      if (stopped) {
         return;
      }
      stopped = true;
      server_socket->stop();
      wake(accept_waiters);
      for (auto& connection : connections()) {
         asio::post(connection->strand, [connection] { connection->fail_all(); });
      }
   }

   void start() {
      if (receive_started) {
         return;
      }
      receive_started = true;
      auto self = this->self.lock();
      if (!self) {
         return;
      }
      ++active_operations;
      try {
         asio::co_spawn(
             strand,
             [self]() -> asio::awaitable<void> {
                struct completion_guard {
                   std::shared_ptr<engine_listener::impl> value;

                   ~completion_guard() {
                      value->finish_operation();
                   }
                } guard{self};
                while (!self->stopped) {
                   auto received = server_udp_socket::packet{};
                   try {
                      received = co_await self->server_socket->async_receive();
                   } catch (const boost::system::system_error& error) {
                      if (error.code() == asio::error::message_size || error.code() == asio::error::invalid_argument) {
                         // datagram_io consumed a malformed/truncated or wrong-interface packet.
                         continue;
                      }
                      co_return;
                   }
                   if (self->stopped) {
                      co_return;
                   }
                   try {
                      co_await self->handle_packet(std::move(received.bytes), std::move(received.route));
                   } catch (const engine_failure&) {
                      // Malformed/adversarial packets must not permanently stop the listener.
                   }
                }
             },
             asio::detached);
      } catch (...) {
         finish_operation();
         throw;
      }
   }

   [[nodiscard]] std::shared_ptr<engine_connection::impl> find_connection_by_cid(const std::string& key) {
      auto lock = std::scoped_lock{cid_mutex};
      if (auto it = connections_by_cid.find(key); it != connections_by_cid.end()) {
         return it->second;
      }
      return {};
   }

   [[nodiscard]] std::size_t connection_count() {
      auto lock = std::scoped_lock{cid_mutex};
      return cids_by_connection.size();
   }

   void register_connection_cid(const std::shared_ptr<engine_connection::impl>& connection, std::string key) {
      auto lock = std::scoped_lock{cid_mutex};
      connections_by_cid[key] = connection;
      auto& keys = cids_by_connection[connection.get()];
      if (std::ranges::find(keys, key) == keys.end()) {
         keys.push_back(std::move(key));
      }
   }

   void unregister_connection_cid(engine_connection::impl* connection, std::string key) {
      auto lock = std::scoped_lock{cid_mutex};
      auto cid = connections_by_cid.find(key);
      if (cid != connections_by_cid.end() && cid->second.get() == connection) {
         connections_by_cid.erase(cid);
      }
      auto it = cids_by_connection.find(connection);
      if (it == cids_by_connection.end()) {
         return;
      }
      std::erase(it->second, key);
      if (it->second.empty()) {
         cids_by_connection.erase(it);
      }
   }

   [[nodiscard]] bool release_connection_slot(engine_connection::impl* connection) {
      auto had_connection_ids = false;
      auto lock = std::scoped_lock{cid_mutex};
      auto it = cids_by_connection.find(connection);
      if (it == cids_by_connection.end()) {
         return false;
      }
      had_connection_ids = true;
      for (const auto& key : it->second) {
         auto cid = connections_by_cid.find(key);
         if (cid != connections_by_cid.end() && cid->second.get() == connection) {
            connections_by_cid.erase(cid);
         }
      }
      cids_by_connection.erase(it);
      return had_connection_ids;
   }

   void cleanup_connection(const std::shared_ptr<engine_connection::impl>& connection, bool had_connection_ids) {
      const auto has_accept_waiter = std::ranges::any_of(
          accept_waiters, [](const std::weak_ptr<asio::steady_timer>& waiter) { return !waiter.expired(); });
      const auto failed_before_accept = has_accept_waiter && connection->report_accept_failure &&
                                        !connection->handshake_done && !connection->listener_accept_notified &&
                                        !stopped;
      if (failed_before_accept && had_connection_ids) {
         const auto timed_out = connection->metrics.timeouts.load(std::memory_order_relaxed) > 0;
         pending_accept_error = timed_out ? engine_error_kind::handshake_timeout : engine_error_kind::connection_closed;
         pending_accept_failure_text = timed_out ? "QUIC server handshake timed out before accept"
                                                 : "QUIC server connection closed before accept";
      }
      if (had_connection_ids && pending_accept_error) {
         wake(accept_waiters);
      }
   }

   void start_handshake_deadline(const std::shared_ptr<engine_connection::impl>& connection) {
      const auto timeout = options.handshake_timeout;
      connection->spawn_background(
          [timeout](const std::shared_ptr<engine_connection::impl>& value) -> asio::awaitable<void> {
             if (value->handshake_done || value->closing || value->canceled) {
                co_return;
             }
             value->handshake_timer.expires_after(timeout);
             auto ec = boost::system::error_code{};
             co_await value->handshake_timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
             if (ec) {
                co_return;
             }
             if (value->handshake_done || value->closing || value->canceled) {
                co_return;
             }
             value->metrics.handshakes_failed.fetch_add(1, std::memory_order_relaxed);
             value->metrics.timeouts.fetch_add(1, std::memory_order_relaxed);
             value->fail_all();
          });
   }

   boost::asio::awaitable<void> send_retry(const ngtcp2_pkt_hd& header,
                                           forge::net::transport::datagram_io::received route) {
      auto path = make_path(route.local, route.remote);
      const auto retry_scid = random_cid(cid_length);
      const auto token = initial_tokens.generate_retry(header.version,
                                                       initial_token_remote_address{
                                                           .address = path.path.remote.addr,
                                                           .length = path.path.remote.addrlen,
                                                       },
                                                       retry_scid, header.dcid, timestamp());
      if (!token) {
         throw_engine(engine_error_kind::internal_error, "failed to generate QUIC Retry token");
      }

      auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
      const auto packet_length = ngtcp2_crypto_write_retry(packet.data(), packet.size(), header.version, &header.scid,
                                                           &retry_scid, &header.dcid, token->data(), token->size());
      if (packet_length < 0) {
         throw_engine(engine_error_kind::internal_error, "failed to encode QUIC Retry packet");
      }
      const auto error = co_await server_socket->async_send(
          {.bytes = {packet.begin(), packet.begin() + packet_length}, .route = route});
      if (error && error != asio::error::operation_aborted) {
         throw_engine(engine_error_kind::internal_error, "failed to send QUIC Retry packet: " + error.message());
      }
   }

   boost::asio::awaitable<void> send_invalid_token_close(const ngtcp2_pkt_hd& header,
                                                         forge::net::transport::datagram_io::received route) {
      auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
      const auto packet_length = ngtcp2_crypto_write_connection_close(
          packet.data(), packet.size(), header.version, &header.scid, &header.dcid, NGTCP2_INVALID_TOKEN, nullptr, 0);
      if (packet_length < 0) {
         throw_engine(engine_error_kind::internal_error, "failed to encode QUIC INVALID_TOKEN connection close");
      }
      const auto error = co_await server_socket->async_send(
          {.bytes = {packet.begin(), packet.begin() + packet_length}, .route = route});
      if (error && error != asio::error::operation_aborted) {
         throw_engine(engine_error_kind::internal_error,
                      "failed to send QUIC INVALID_TOKEN connection close: " + error.message());
      }
   }

   boost::asio::awaitable<void> handle_packet(std::vector<std::uint8_t> packet,
                                              forge::net::transport::datagram_io::received route) {
      auto vcid = ngtcp2_version_cid{};
      auto rv = ngtcp2_pkt_decode_version_cid(&vcid, packet.data(), packet.size(), cid_length);
      if (rv != 0) {
         co_return;
      }
      auto key = cid_key(vcid.dcid, vcid.dcidlen);
      auto connection = std::shared_ptr<engine_connection::impl>{};
      connection = find_connection_by_cid(key);
      if (!connection) {
         auto hd = ngtcp2_pkt_hd{};
         const auto header_result = ngtcp2_pkt_decode_hd_long(&hd, packet.data(), packet.size());
         if (header_result < 0 || hd.type != NGTCP2_PKT_INITIAL) {
            co_return;
         }
         const auto token_bytes = hd.token == nullptr ? std::span<const std::uint8_t>{}
                                                      : std::span<const std::uint8_t>{hd.token, hd.tokenlen};
         const auto path = make_path(route.local, route.remote);
         const auto token = initial_tokens.validate(token_bytes, hd.version,
                                                    initial_token_remote_address{
                                                        .address = path.path.remote.addr,
                                                        .length = path.path.remote.addrlen,
                                                    },
                                                    hd.dcid, timestamp());
         switch (token.disposition) {
         case initial_token_disposition::retry:
            co_await send_retry(hd, route);
            co_return;
         case initial_token_disposition::reject_invalid:
            co_await send_invalid_token_close(hd, route);
            co_return;
         case initial_token_disposition::internal_failure:
            throw_engine(engine_error_kind::internal_error, "QUIC initial token verifier failed internally");
         case initial_token_disposition::accept:
            break;
         }
         try {
            connection = create_server_connection(hd, token, route);
         } catch (const engine_failure& error) {
            if (error.kind() == engine_error_kind::internal_error) {
               pending_accept_error = error.kind();
               pending_accept_failure_text = error.message();
               wake(accept_waiters);
            }
            throw;
         }
         register_connection_cid(connection, cid_key(hd.dcid.data, hd.dcid.datalen));
         auto local_cid = ngtcp2_cid{};
         ngtcp2_conn_get_scid(connection->conn, &local_cid);
         register_connection_cid(connection, cid_key(local_cid));
         start_handshake_deadline(connection);
      }
      try {
         co_await asio::co_spawn(connection->strand, connection->handle_packet(std::move(packet), std::move(route)),
                                 asio::use_awaitable);
      } catch (const engine_failure&) {
         asio::post(connection->strand, [connection] { connection->fail_all(); });
      }
   }

   [[nodiscard]] std::shared_ptr<engine_connection::impl>
   create_server_connection(const ngtcp2_pkt_hd& hd, const initial_token_validation& token,
                            const forge::net::transport::datagram_io::received& route) {
      if (!token.accepted()) {
         throw_engine(engine_error_kind::internal_error,
                      "cannot create QUIC server connection without token validation");
      }
      if (connection_count() >= options.limits.max_connections) {
         throw_engine(engine_error_kind::backpressure_rejected, "QUIC listener max connections exceeded");
      }
      if (options.inbound_connection_filter) {
         try {
            if (!options.inbound_connection_filter(from_udp_endpoint(route.local), from_udp_endpoint(route.remote))) {
               throw_engine(engine_error_kind::connection_rejected, "QUIC inbound connection rejected");
            }
         } catch (const engine_failure&) {
            throw;
         } catch (...) {
            throw_engine(engine_error_kind::connection_rejected, "QUIC inbound connection rejected");
         }
      }
      auto admission = std::shared_ptr<void>{};
      if (options.inbound_admission) {
         try {
            admission = options.inbound_admission();
         } catch (...) {
            throw_engine(engine_error_kind::internal_error, "QUIC inbound admission failed internally");
         }
         if (!admission) {
            throw_engine(engine_error_kind::backpressure_rejected, "QUIC inbound admission rejected");
         }
      }
      auto connection =
          std::make_shared<engine_connection::impl>(context, server_socket, route.local, route.remote, options.limits);
      connection->inbound_admission = std::move(admission);
      connection->self = connection;
      connection->server_side = true;
      connection->reset_secret = reset_secret;
      connection->metrics.connections_opened.store(1, std::memory_order_relaxed);
      connection->metrics.handshakes_started.store(1, std::memory_order_relaxed);
      auto listener_weak = self;
      connection->closed_hook = [listener_weak](std::shared_ptr<engine_connection::impl> closed_connection) {
         auto listener = listener_weak.lock();
         if (!listener) {
            return;
         }
         const auto had_connection_ids = listener->release_connection_slot(closed_connection.get());
         asio::post(listener->strand, [listener, closed_connection = std::move(closed_connection), had_connection_ids] {
            listener->cleanup_connection(closed_connection, had_connection_ids);
         });
      };
      connection->local_connection_id_issued_hook =
          [listener_weak, connection_weak = std::weak_ptr<engine_connection::impl>{connection}](const ngtcp2_cid& cid) {
             auto listener = listener_weak.lock();
             auto connection = connection_weak.lock();
             if (!listener || !connection) {
                return;
             }
             listener->register_connection_cid(connection, cid_key(cid));
          };
      connection->local_connection_id_retired_hook =
          [listener_weak, connection_weak = std::weak_ptr<engine_connection::impl>{connection}](const ngtcp2_cid& cid) {
             auto listener = listener_weak.lock();
             auto connection = connection_weak.lock();
             if (!listener || !connection) {
                return;
             }
             listener->unregister_connection_cid(connection.get(), cid_key(cid));
          };
      connection->issue_new_token = [listener_weak](engine_connection::impl& value) {
         if (value.new_token_submitted || value.conn == nullptr) {
            return;
         }
         const auto listener = listener_weak.lock();
         if (!listener) {
            return;
         }
         const auto path = make_path(value.local_endpoint(), value.remote_endpoint);
         const auto token = listener->initial_tokens.generate_regular(
             initial_token_remote_address{.address = path.path.remote.addr, .length = path.path.remote.addrlen},
             timestamp());
         if (!token || token->empty()) {
            return;
         }
         if (ngtcp2_conn_submit_new_token(value.conn, token->data(), token->size()) == 0) {
            value.new_token_submitted = true;
            value.metrics.new_tokens_submitted.fetch_add(1, std::memory_order_relaxed);
         }
      };
      auto connection_weak = std::weak_ptr<engine_connection::impl>{connection};
      connection->handshake_completed_hook = [listener_weak, connection_weak] {
         auto listener = listener_weak.lock();
         auto connection = connection_weak.lock();
         if (!listener || !connection) {
            return;
         }
         asio::post(listener->strand, [listener, connection] {
            if (listener->stopped) {
               return;
            }
            if (connection->listener_accept_notified) {
               return;
            }
            connection->listener_accept_notified = true;
            listener->accepted.push_back(std::shared_ptr<engine_connection>{new engine_connection{connection}});
            wake(listener->accept_waiters);
         });
      };
      auto callbacks = server_callbacks();
      auto settings = ngtcp2_settings{};
      auto params = ngtcp2_transport_params{};
      configure_settings(settings);
      configure_params(params, options.limits, options.idle_timeout);
      settings.token = hd.token;
      settings.tokenlen = hd.tokenlen;
      settings.token_type = token.token_type;
      params.original_dcid = token.original_dcid;
      params.original_dcid_present = 1;
      if (token.token_type == NGTCP2_TOKEN_TYPE_RETRY) {
         params.retry_scid = hd.dcid;
         params.retry_scid_present = 1;
      }
      params.stateless_reset_token_present = 1;

      const auto scid = random_cid(cid_length);
      if (ngtcp2_crypto_generate_stateless_reset_token(params.stateless_reset_token, reset_secret.data(),
                                                       reset_secret.size(), &scid) != 0) {
         throw_engine(engine_error_kind::tls_failed, "failed to generate stateless reset token");
      }
      auto path = make_path(route.local, route.remote);
      const auto rv = ngtcp2_conn_server_new(&connection->conn, &hd.scid, &scid, &path.path, hd.version, &callbacks,
                                             &settings, &params, nullptr, connection.get());
      if (rv != 0) {
         throw_engine(engine_error_kind::internal_error,
                      std::string{"ngtcp2_conn_server_new failed: "} + ngtcp2_strerror(rv));
      }
      validate_packet_buffer(connection->conn);
      configure_server_tls(*connection, options);
      ngtcp2_conn_set_tls_native_handle(connection->conn, connection->ossl_ctx);
      return connection;
   }
};

engine_listener::engine_listener(boost::asio::io_context& context, engine_endpoint bind_endpoint,
                                 engine_server_options options)
    : impl_(std::make_shared<impl>(context, std::move(bind_endpoint), std::move(options))) {
   impl_->self = impl_;
   if (impl_->bind_endpoint.host.empty()) {
      impl_->bind_endpoint.host = "127.0.0.1";
   }
   const auto address = literal_address(impl_->bind_endpoint);
   auto endpoint = udp::endpoint{address, impl_->bind_endpoint.port};
   impl_->server_socket->open_and_bind(endpoint);
   const auto local = impl_->server_socket->local_endpoint();
   impl_->bind_endpoint = from_udp_endpoint(local);
   impl_->start();
}

engine_listener::~engine_listener() {
   stop();
}

engine_endpoint engine_listener::local_endpoint() const {
   return impl_ ? impl_->bind_endpoint : engine_endpoint{};
}

boost::asio::awaitable<std::shared_ptr<engine_connection>> engine_listener::async_accept() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC listener");
   }
   auto state = impl_;
   co_return co_await asio::co_spawn(
       state->strand,
       [state]() -> asio::awaitable<std::shared_ptr<engine_connection>> {
          ++state->active_operations;
          struct completion_guard {
             std::shared_ptr<engine_listener::impl> value;

             ~completion_guard() {
                value->finish_operation();
             }
          } guard{state};
          while (state->accepted.empty() && !state->stopped &&
                 (!state->pending_accept_error || state->connection_count() != 0)) {
             auto timer = std::make_shared<asio::steady_timer>(state->strand);
             timer->expires_after(std::chrono::minutes{10});
             state->accept_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (state->accepted.empty() && state->pending_accept_error) {
             const auto kind = *state->pending_accept_error;
             auto message = std::move(state->pending_accept_failure_text);
             state->pending_accept_error.reset();
             state->pending_accept_failure_text.clear();
             throw_engine(kind, message.empty() ? "QUIC listener accept failed" : message);
          }
          if (state->accepted.empty()) {
             throw_engine(engine_error_kind::connection_closed, "QUIC listener stopped before accept");
          }
          state->pending_accept_error.reset();
          state->pending_accept_failure_text.clear();
          auto connection = std::move(state->accepted.front());
          state->accepted.pop_front();
          co_return connection;
       },
       asio::use_awaitable);
}

void engine_listener::stop() {
   if (!impl_) {
      return;
   }
   asio::post(impl_->strand, [impl = impl_] { impl->stop(); });
}

boost::asio::awaitable<void> engine_listener::async_stop() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   const auto shutdown_action = co_await asio::co_spawn(
       state->strand,
       [state]() -> asio::awaitable<engine_listener::impl::shutdown_action> { co_return state->begin_shutdown(); },
       asio::use_awaitable);
   if (shutdown_action == engine_listener::impl::shutdown_action::done) {
      if (auto error = state->shutdown_failure()) {
         std::rethrow_exception(error);
      }
      co_return;
   }
   if (shutdown_action == engine_listener::impl::shutdown_action::wait) {
      co_await asio::co_spawn(state->strand, state->wait_shutdown_complete(), asio::use_awaitable);
      if (auto error = state->shutdown_failure()) {
         std::rethrow_exception(error);
      }
      co_return;
   }
   auto shutdown_error = std::exception_ptr{};
   const auto remember_shutdown_error = [&] {
      if (!shutdown_error) {
         shutdown_error = std::current_exception();
      }
   };
   auto connections = std::vector<std::shared_ptr<engine_connection::impl>>{};
   try {
      connections = co_await asio::co_spawn(
          state->strand,
          [state]() -> asio::awaitable<std::vector<std::shared_ptr<engine_connection::impl>>> {
             auto connections = state->connections();
             state->stop();
             co_await state->wait_operations_idle();
             for (auto& candidate : state->connections()) {
                if (std::ranges::none_of(connections,
                                         [&](const auto& current) { return current.get() == candidate.get(); })) {
                   connections.push_back(std::move(candidate));
                }
             }
             co_return connections;
          },
          asio::use_awaitable);
   } catch (...) {
      remember_shutdown_error();
   }
   for (const auto& connection : connections) {
      try {
         co_await asio::co_spawn(
             connection->strand,
             [connection]() -> asio::awaitable<void> {
                connection->fail_all();
                co_await connection->wait_background_idle();
                connection->handshake_completed_hook = {};
                connection->closed_hook = {};
                connection->local_connection_id_issued_hook = {};
                connection->local_connection_id_retired_hook = {};
             },
             asio::use_awaitable);
      } catch (...) {
         remember_shutdown_error();
      }
   }
   try {
      co_await asio::co_spawn(
          state->strand,
          [state]() -> asio::awaitable<void> {
             state->clear_connection_registry();
             co_return;
          },
          asio::use_awaitable);
   } catch (...) {
      remember_shutdown_error();
   }
   co_await asio::co_spawn(
       state->strand,
       [state, shutdown_error]() -> asio::awaitable<void> {
          state->finish_shutdown(shutdown_error);
          co_return;
       },
       asio::use_awaitable);
   if (shutdown_error) {
      std::rethrow_exception(shutdown_error);
   }
}

} // namespace forge::net::quic::detail
