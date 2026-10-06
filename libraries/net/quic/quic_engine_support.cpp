#include "details/quic_engine_support.hxx"
#include "details/engine_connection_impl.hxx"

namespace forge::net::quic::detail {

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

[[nodiscard]] stream_terminal_owner
claim_stream_terminal_owner(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
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

int stream_close_cb(ngtcp2_conn* conn, std::uint32_t flags, std::int64_t stream_id, std::uint64_t, void* user_data, void*) {
   auto* connection = static_cast<engine_connection::impl*>(user_data);
   if (auto it = connection->streams.find(stream_id); it != connection->streams.end()) {
      auto stream = std::move(it->second);
      connection->streams.erase(it);
      connection->release_queued_stream_writes(stream);
      stream->native_write_rejected |= (flags & NGTCP2_STREAM_CLOSE_FLAG_APP_ERROR_CODE_SET) != 0;
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

[[nodiscard]] ngtcp2_cid random_cid(std::size_t len) {
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

} // namespace forge::net::quic::detail
