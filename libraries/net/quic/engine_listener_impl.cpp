#include "details/engine_listener_impl.hxx"
#include "details/engine_connection_impl.hxx"

namespace forge::net::quic::detail {
engine_listener::impl::impl(boost::asio::io_context& context_value, engine_endpoint endpoint_value,
                            engine_server_options options_value)
    : context(context_value), strand(asio::make_strand(context_value)),
      server_socket(std::make_shared<server_udp_socket>(strand)), bind_endpoint(std::move(endpoint_value)),
      options(std::move(options_value)) {}

[[nodiscard]] std::vector<std::shared_ptr<engine_connection::impl>> engine_listener::impl::connections() {
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

void engine_listener::impl::finish_operation() noexcept {
   if (active_operations == 0) {
      return;
   }
   --active_operations;
   if (active_operations == 0) {
      // These waits and all admitted-operation guards share this strand.
      // Direct cancellation avoids any_io_executor's allocating dispatch.
      auto waiters = std::move(operation_waiters);
      operation_waiters.clear();
      for (const auto& weak : waiters) {
         if (auto timer = weak.lock()) {
            assert(strand.running_in_this_thread());
            timer->cancel();
         }
      }
   }
}

boost::asio::awaitable<void> engine_listener::impl::wait_operations_idle() {
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

[[nodiscard]] engine_listener::impl::shutdown_action engine_listener::impl::begin_shutdown() {
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

[[nodiscard]] std::exception_ptr engine_listener::impl::shutdown_failure() const {
   auto lock = std::scoped_lock{shutdown_mutex};
   return shutdown_error;
}

boost::asio::awaitable<void> engine_listener::impl::wait_shutdown_complete() {
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

void engine_listener::impl::finish_shutdown(std::exception_ptr error) noexcept {
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

void engine_listener::impl::clear_connection_registry() {
   {
      auto lock = std::scoped_lock{cid_mutex};
      connections_by_cid.clear();
      cids_by_connection.clear();
   }
   accepted.clear();
   pending_accept_error.reset();
   pending_accept_failure_text.clear();
}

void engine_listener::impl::stop() {
   if (stopped) {
      return;
   }
   stopped = true;
   stop_requested.store(true, std::memory_order_release);
   server_socket->stop();
   wake(punch_waiters);
   wake(accept_waiters);
   for (auto& connection : connections()) {
      asio::post(connection->strand, [connection] { connection->fail_all(); });
   }
}

void engine_listener::impl::start() {
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
             const auto finish = [self](engine_listener::impl*) noexcept { self->finish_operation(); };
             auto guard = std::unique_ptr<engine_listener::impl, decltype(finish)>{self.get(), finish};
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
      receive_started = false;
      finish_operation();
      throw;
   }
}

[[nodiscard]] std::shared_ptr<engine_connection::impl>
engine_listener::impl::find_connection_by_cid(const std::string& key) {
   auto lock = std::scoped_lock{cid_mutex};
   if (auto it = connections_by_cid.find(key); it != connections_by_cid.end()) {
      return it->second;
   }
   return {};
}

[[nodiscard]] std::size_t engine_listener::impl::connection_count() {
   auto lock = std::scoped_lock{cid_mutex};
   return cids_by_connection.size();
}

void engine_listener::impl::register_connection_cid(const std::shared_ptr<engine_connection::impl>& connection,
                                                    std::string key) {
   auto lock = std::scoped_lock{cid_mutex};
   if (const auto existing = connections_by_cid.find(key); existing != connections_by_cid.end()) {
      if (existing->second == connection) {
         return;
      }
      throw_engine(engine_error_kind::internal_error, "QUIC connection ID is already owned by another connection");
   }
   if (connection->test_failpoint) {
      static_cast<void>(connection->test_failpoint("cid_registry_before_reverse_index_insert"));
   }
   const auto [reverse, inserted] = cids_by_connection.try_emplace(connection.get());
   auto appended = false;
   try {
      if (connection->test_failpoint) {
         static_cast<void>(connection->test_failpoint("cid_registry_before_reverse_index_append"));
      }
      reverse->second.push_back(key);
      appended = true;
      if (connection->test_failpoint) {
         static_cast<void>(connection->test_failpoint("cid_registry_before_forward_index_insert"));
      }
      // Publish the strong owner only after its reverse entry is complete.
      // Either map allocation may fail; rollback never allocates.
      connections_by_cid.emplace(std::move(key), connection);
   } catch (...) {
      if (appended) {
         reverse->second.pop_back();
      }
      if (inserted) {
         cids_by_connection.erase(reverse);
      }
      throw;
   }
}

void engine_listener::impl::unregister_connection_cid(engine_connection::impl* connection, std::string key) {
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

[[nodiscard]] bool engine_listener::impl::release_connection_slot(engine_connection::impl* connection) {
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

void engine_listener::impl::cleanup_connection(const std::shared_ptr<engine_connection::impl>& connection,
                                               bool had_connection_ids) {
   const auto has_accept_waiter = std::ranges::any_of(
       accept_waiters, [](const std::weak_ptr<asio::steady_timer>& waiter) { return !waiter.expired(); });
   const auto failed_before_accept = has_accept_waiter && connection->report_accept_failure &&
                                     !connection->handshake_done && !connection->listener_accept_notified && !stopped;
   if (failed_before_accept && had_connection_ids) {
      const auto timed_out = connection->metrics.timeouts.load(std::memory_order_relaxed) > 0;
      pending_accept_error = timed_out ? engine_error_kind::handshake_timeout : engine_error_kind::connection_closed;
      pending_accept_failure_text =
          timed_out ? "QUIC server handshake timed out before accept" : "QUIC server connection closed before accept";
   }
   if (had_connection_ids && pending_accept_error) {
      wake(accept_waiters);
   }
}

void engine_listener::impl::start_handshake_deadline(const std::shared_ptr<engine_connection::impl>& connection) {
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

boost::asio::awaitable<void> engine_listener::impl::send_retry(const ngtcp2_pkt_hd& header,
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
   const auto error =
       co_await server_socket->async_send({.bytes = {packet.begin(), packet.begin() + packet_length}, .route = route});
   if (error && error != asio::error::operation_aborted) {
      throw_engine(engine_error_kind::internal_error, "failed to send QUIC Retry packet: " + error.message());
   }
}

boost::asio::awaitable<void>
engine_listener::impl::send_invalid_token_close(const ngtcp2_pkt_hd& header,
                                                forge::net::transport::datagram_io::received route) {
   auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
   const auto packet_length = ngtcp2_crypto_write_connection_close(
       packet.data(), packet.size(), header.version, &header.scid, &header.dcid, NGTCP2_INVALID_TOKEN, nullptr, 0);
   if (packet_length < 0) {
      throw_engine(engine_error_kind::internal_error, "failed to encode QUIC INVALID_TOKEN connection close");
   }
   const auto error =
       co_await server_socket->async_send({.bytes = {packet.begin(), packet.begin() + packet_length}, .route = route});
   if (error && error != asio::error::operation_aborted) {
      throw_engine(engine_error_kind::internal_error,
                   "failed to send QUIC INVALID_TOKEN connection close: " + error.message());
   }
}

boost::asio::awaitable<void> engine_listener::impl::handle_packet(std::vector<std::uint8_t> packet,
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
      const auto token_bytes =
          hd.token == nullptr ? std::span<const std::uint8_t>{} : std::span<const std::uint8_t>{hd.token, hd.tokenlen};
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
engine_listener::impl::create_server_connection(const ngtcp2_pkt_hd& hd, const initial_token_validation& token,
                                                const forge::net::transport::datagram_io::received& route) {
   if (!token.accepted()) {
      throw_engine(engine_error_kind::internal_error, "cannot create QUIC server connection without token validation");
   }
   if (connection_count() + pending_dials >= options.limits.max_connections) {
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
} // namespace forge::net::quic::detail
