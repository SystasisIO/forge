#pragma once

#include "quic_engine.hxx"
#include "acknowledged_ranges.hxx"
#include "initial_token.hxx"
#include "server_udp_socket.hxx"

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

#include "engine_client_options.hxx"
#include "engine_server_options.hxx"

#include "path_storage.hxx"
#include "tls_handles.hxx"

namespace forge::net::quic::detail {

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

[[nodiscard]] ngtcp2_tstamp timestamp() noexcept;

[[nodiscard]] std::string openssl_error();

[[noreturn]] void throw_engine(engine_error_kind kind, std::string message);

[[nodiscard]] std::string ngtcp2_read_error_message(ngtcp2_conn* conn, int rv);

[[nodiscard]] std::chrono::milliseconds remaining_timeout_budget(std::chrono::steady_clock::time_point started,
                                                                 std::chrono::milliseconds timeout) noexcept;

[[nodiscard]] bool connect_failpoint_enabled(const engine_client_options& options, std::string_view name);

int accept_any_certificate_cb(int, X509_STORE_CTX*);

[[nodiscard]] bool fill_random(std::span<std::uint8_t> bytes);

[[nodiscard]] stateless_reset_secret random_stateless_reset_secret();

[[nodiscard]] initial_token_validator::secret random_initial_token_secret();

void rand_cb(std::uint8_t* dest, std::size_t destlen, const ngtcp2_rand_ctx*);

[[nodiscard]] std::string cid_key(const ngtcp2_cid& cid);

[[nodiscard]] std::string cid_key(const std::uint8_t* data, std::size_t len);

[[nodiscard]] sockaddr_storage to_sockaddr_storage(const udp::endpoint& endpoint);

[[nodiscard]] ngtcp2_addr to_ngtcp2_addr(sockaddr_storage& storage);

[[nodiscard]] udp::endpoint from_ngtcp2_addr(const ngtcp2_addr& address);

[[nodiscard]] forge::net::transport::datagram_io::received copy_route(const ngtcp2_path& path);

[[nodiscard]] path_storage make_path(const udp::endpoint& local, const udp::endpoint& remote);

[[nodiscard]] std::vector<std::uint8_t> length_prefixed_alpn(std::string_view alpn);

int select_alpn_cb(SSL*, const unsigned char** out, unsigned char* outlen, const unsigned char* in, unsigned int inlen,
                   void* arg);

using ssl_ctx_ptr = std::unique_ptr<SSL_CTX, tls_handles::context_deleter>;
using ssl_ptr = std::unique_ptr<SSL, tls_handles::session_deleter>;
using x509_ptr = std::unique_ptr<X509, tls_handles::certificate_deleter>;
using pkey_ptr = std::unique_ptr<EVP_PKEY, tls_handles::key_deleter>;

[[nodiscard]] x509_ptr load_certificate(std::string_view pem);

[[nodiscard]] pkey_ptr load_private_key(std::string_view pem);

void add_trusted_certificate(SSL_CTX* ctx, std::string_view pem);

void configure_default_trust(SSL_CTX* ctx, const engine_security_options& security);

[[nodiscard]] bool configure_verify_peer_name(SSL* ssl, std::string_view host);

[[nodiscard]] std::vector<std::uint8_t> der_from_certificate(X509* certificate);

void wake(const std::shared_ptr<asio::steady_timer>& timer) noexcept;

void wake(std::vector<std::weak_ptr<asio::steady_timer>>& waiters) noexcept;

void remove_waiter(std::vector<std::weak_ptr<asio::steady_timer>>& waiters,
                   const std::shared_ptr<asio::steady_timer>& target) noexcept;

[[nodiscard]] engine_endpoint from_udp_endpoint(const udp::endpoint& value);

[[nodiscard]] asio::ip::address literal_address(const engine_endpoint& value);

void publish_stream_terminal(const std::shared_ptr<engine_stream::impl>& stream) noexcept;

void finish_stream_terminal_cleanup(const std::shared_ptr<engine_stream::impl>& stream) noexcept;

void release_stream_terminal_owner(engine_stream::impl* stream) noexcept;

using stream_terminal_owner = std::unique_ptr<engine_stream::impl, decltype(&release_stream_terminal_owner)>;

[[nodiscard]] stream_terminal_owner
claim_stream_terminal_owner(const std::shared_ptr<engine_stream::impl>& stream) noexcept;

boost::asio::awaitable<void> wait_for_stream_terminal_cleanup(const std::shared_ptr<engine_stream::impl>& stream);

int get_new_connection_id_cb(ngtcp2_conn*, ngtcp2_cid* cid, ngtcp2_stateless_reset_token* token, std::size_t cidlen,
                             void* user_data);

int remove_connection_id_cb(ngtcp2_conn*, const ngtcp2_cid* cid, void* user_data);

ngtcp2_conn* get_conn_cb(ngtcp2_crypto_conn_ref* conn_ref);

int handshake_completed_cb(ngtcp2_conn*, void* user_data);

int recv_retry_cb(ngtcp2_conn* connection, const ngtcp2_pkt_hd* header, void* user_data);

int recv_new_token_cb(ngtcp2_conn*, const std::uint8_t* token, std::size_t token_length, void* user_data);

int stream_open_cb(ngtcp2_conn* conn, std::int64_t stream_id, void* user_data);

int recv_stream_data_cb(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id, std::uint64_t offset,
                        const std::uint8_t* data, std::size_t datalen, void* user_data, void* stream_user_data);

int acked_stream_data_offset_cb(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t offset, std::uint64_t datalen,
                                void* user_data, void*);

int stream_close_cb(ngtcp2_conn* conn, std::uint32_t, std::int64_t stream_id, std::uint64_t, void* user_data, void*);

int stream_reset_cb(ngtcp2_conn*, std::int64_t stream_id, std::uint64_t, std::uint64_t, void* user_data, void*);

int extend_max_local_streams_bidi_cb(ngtcp2_conn*, std::uint64_t, void* user_data);

[[nodiscard]] ngtcp2_callbacks client_callbacks();

[[nodiscard]] ngtcp2_callbacks server_callbacks();

void configure_settings(ngtcp2_settings& settings);

void validate_packet_buffer(ngtcp2_conn* connection);

void configure_params(ngtcp2_transport_params& params, const engine_transport_limits& limits,
                      std::chrono::milliseconds idle_timeout);

[[nodiscard]] ngtcp2_cid random_cid(std::size_t len = cid_length);

void configure_client_tls(engine_connection::impl& connection, const engine_endpoint& remote,
                          const engine_client_options& options);

void configure_server_tls(engine_connection::impl& connection, const engine_server_options& options);

} // namespace forge::net::quic::detail
