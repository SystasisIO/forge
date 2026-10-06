#include "details/quic_engine_support.hxx"
#include "details/engine_stream_impl.hxx"
#include "details/engine_connection_impl.hxx"
#include "details/engine_connector_impl.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {
engine_connector::engine_connector(boost::asio::io_context& context) : impl_(std::make_shared<impl>(context)) {}

engine_connector::engine_connector(boost::asio::io_context& context, engine_listener& source, engine_endpoint local)
    : impl_(std::make_shared<impl>(context)) {
   const auto endpoint = udp::endpoint{literal_address(local), local.port};
   if (&source.impl_->context != &context || source.impl_->stop_requested.load(std::memory_order_acquire)) {
      throw_engine(engine_error_kind::connection_closed, "QUIC source listener is closed or uses another runtime");
   }
   if (!source.impl_->server_socket->owns_local_endpoint(endpoint)) {
      throw_engine(engine_error_kind::invalid_endpoint, "QUIC source endpoint does not belong to listener");
   }
   impl_->source = source.impl_;
   impl_->source_endpoint = endpoint;
}

boost::asio::awaitable<std::shared_ptr<engine_connection>>
engine_connector::async_connect(engine_endpoint remote, engine_client_options options) {
   if (!impl_ || !impl_->valid()) {
      throw_engine(engine_error_kind::canceled, "QUIC connector is canceled");
   }
   auto owner = impl_;
   if (owner->source) {
      // Spawn before admission. Every suspension and unwind of the admitted
      // operation then resumes on the listener strand, including its guard.
      co_return co_await asio::co_spawn(
          owner->source->strand, async_connect_owned(owner, std::move(remote), std::move(options)),
          asio::use_awaitable);
   }
   co_return co_await async_connect_owned(std::move(owner), std::move(remote), std::move(options));
}

boost::asio::awaitable<std::shared_ptr<engine_connection>>
engine_connector::async_connect_owned(std::shared_ptr<impl> owner, engine_endpoint remote, engine_client_options options) {
   if (!owner->valid()) {
      throw_engine(engine_error_kind::canceled, "QUIC connector is canceled");
   }
   const auto executor = co_await asio::this_coro::executor;
   auto source = owner->source;
   if (source) {
      assert(source->strand.running_in_this_thread());
      const auto remote_address = literal_address(remote);
      if (remote.port == 0 || remote_address.is_unspecified() || remote_address.is_multicast() ||
          remote_address.is_v4() != owner->source_endpoint->address().is_v4()) {
         throw_engine(engine_error_kind::invalid_endpoint, "coordinated QUIC requires a compatible literal remote");
      }
      if (source->shutdown_started || source->stop_requested.load(std::memory_order_acquire) || source->stopped) {
         throw_engine(engine_error_kind::connection_closed, "QUIC source listener is closed");
      }
      if (source->connection_count() + source->pending_dials >= source->options.limits.max_connections) {
         throw_engine(engine_error_kind::backpressure_rejected, "QUIC listener dial limit exceeded");
      }
      source->start();
      ++source->pending_dials;
      ++source->active_operations;
   }
   auto source_pending = static_cast<bool>(source);
   const auto release_source_operation = [source, &source_pending, &options](engine_listener::impl*) noexcept {
      if (source) {
         assert(source->strand.running_in_this_thread());
         options.connection_lifetime.reset();
         if (source_pending) {
            --source->pending_dials;
         }
         source->finish_operation();
      }
   };
   auto source_operation = std::unique_ptr<engine_listener::impl, decltype(release_source_operation)>{
       source.get(), release_source_operation};
   const auto inherited_cancellation = co_await asio::this_coro::cancellation_state;
   const auto connect_started = std::chrono::steady_clock::now();
   auto resolver = std::make_shared<udp::resolver>(executor);
   auto connect_timer = std::make_shared<asio::steady_timer>(executor);
   auto active_connect = owner->track_connect(resolver);
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
   auto socket = std::shared_ptr<udp::socket>{};
   auto local_endpoint = udp::endpoint{};
   if (source) {
      if (source->stop_requested.load(std::memory_order_acquire)) {
         finish_connect_or_throw();
         throw_engine(engine_error_kind::connection_closed, "QUIC source listener closed before dial");
      }
      local_endpoint = *owner->source_endpoint;
   } else {
      socket = std::make_shared<udp::socket>(owner->context);
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
      local_endpoint = socket->local_endpoint(ec);
      if (ec || local_endpoint.address().is_unspecified() ||
          local_endpoint.protocol() != remote_endpoint.endpoint().protocol()) {
         finish_connect_or_throw();
         throw_engine(engine_error_kind::internal_error, "failed to read QUIC UDP socket endpoint: " + ec.message());
      }
   }
   if (active_connect->canceled()) {
      connect_timer->cancel();
      throw_engine(engine_error_kind::canceled, "QUIC client connect canceled");
   }
   if (active_connect->timed_out()) {
      connect_timer->cancel();
      throw_engine(engine_error_kind::connect_timeout, "QUIC client connect timed out");
   }

   auto connection_impl =
       source ? std::make_shared<engine_connection::impl>(owner->context, source->server_socket, local_endpoint,
                                                          remote_endpoint.endpoint(), options.limits)
              : std::make_shared<engine_connection::impl>(owner->context, socket, local_endpoint,
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
   auto result = std::shared_ptr<engine_connection>{};
   try {
      throw_if_terminal();
      // Allocate both wrapper and control block before CID/native publication.
      // Keep it inert until success; the exception path exclusively owns the
      // awaited native cleanup below.
      result = std::shared_ptr<engine_connection>{new engine_connection{nullptr}};
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
             if (source) {
                co_await asio::co_spawn(
                    source->strand,
                    [source, connection_impl, scid, &source_pending]() -> asio::awaitable<void> {
                       if (source->shutdown_started || source->stop_requested.load(std::memory_order_acquire)) {
                          throw_engine(engine_error_kind::connection_closed, "QUIC source listener closed during dial");
                       }
                       const auto weak_source = std::weak_ptr<engine_listener::impl>{source};
                       const auto weak_connection = std::weak_ptr<engine_connection::impl>{connection_impl};
                       connection_impl->closed_hook =
                           [weak_source](std::shared_ptr<engine_connection::impl> connection) {
                              if (const auto listener = weak_source.lock()) {
                                 static_cast<void>(listener->release_connection_slot(connection.get()));
                              }
                           };
                       connection_impl->local_connection_id_issued_hook = [weak_source,
                                                                           weak_connection](const ngtcp2_cid& cid) {
                          const auto listener = weak_source.lock();
                          const auto connection = weak_connection.lock();
                          if (listener && connection) {
                             listener->register_connection_cid(connection, cid_key(cid));
                          }
                       };
                       connection_impl->local_connection_id_retired_hook = [weak_source,
                                                                            weak_connection](const ngtcp2_cid& cid) {
                          const auto listener = weak_source.lock();
                          const auto connection = weak_connection.lock();
                          if (listener && connection) {
                             listener->unregister_connection_cid(connection.get(), cid_key(cid));
                          }
                       };
                       source->register_connection_cid(connection_impl, cid_key(scid));
                       --source->pending_dials;
                       source_pending = false;
                       co_return;
                    },
                    asio::use_awaitable);
             } else {
                connection_impl->start_client_receive_loop();
             }
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
   result->impl_ = std::move(connection_impl);
   co_return result;
}

void engine_connector::cancel() {
   if (impl_) {
      impl_->cancel();
   }
}

} // namespace forge::net::quic::detail
