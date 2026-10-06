module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

module forge.net.p2p.node;

import forge.crypto.asymmetric;
import forge.crypto.asymmetric.x25519;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.pnet.protector;
import forge.net.p2p.stream;
import forge.net.stcp.connection;
import forge.net.stcp.exceptions;
import forge.net.transport.stream;
import forge.net.transport.connector;
import forge.net.tcp.connection;
import forge.net.yamux.session;
import forge.net.tls.options;

#include "details/libp2p_tls.hxx"
#include "details/private_transport_stream.hxx"
#include "details/stream_upgrade.hxx"
#include "details/noise_handshake.hxx"
#include "details/secure_io.hxx"
#include "details/secure_stream_concept.hxx"
#include "details/exact_negotiation_io.hxx"
#include "details/cancellation_latch.hxx"

namespace forge::net::p2p {

namespace detail {

void set_cancel(tcp_upgrade_deadline& deadline, std::function<void()> cancel) {
   if (deadline.cancel_current) {
      deadline.cancel_current->arm(std::move(cancel));
   }
}

void clear_cancel(tcp_upgrade_deadline& deadline) noexcept {
   if (!deadline.cancel_current) {
      return;
   }
   deadline.cancel_current->clear();
}

cancel_cleanup::~cancel_cleanup() {
   if (deadline) {
      clear_cancel(*deadline);
   }
}

[[nodiscard]] bool has_timeout(const tcp_upgrade_deadline& deadline) noexcept {
   return deadline.timeout.count() > 0;
}

} // namespace detail

namespace {

using detail::accept_protocol;
using detail::cancel_cleanup;
using detail::clear_cancel;
using detail::finish_tls_inbound;
using detail::finish_tls_outbound;
using detail::noise_initiator;
using detail::noise_responder;
using detail::secure_transport_stream;
using detail::select_protocol;
using detail::set_cancel;

[[noreturn]] void rethrow_private_transport_as_p2p(const forge::exceptions::base& error) {
   if (const auto code = forge::net::pnet::exceptions::code_of(error)) {
      switch (*code) {
      case forge::net::pnet::exceptions::code::invalid_options:
         FORGE_THROW_CODE(exceptions::code::invalid_options, error.what());
      case forge::net::pnet::exceptions::code::closed:
         FORGE_THROW_CODE(exceptions::code::closed, error.what());
      case forge::net::pnet::exceptions::code::canceled:
         FORGE_THROW_CODE(exceptions::code::canceled, error.what());
      }
   }
   if (const auto code = forge::net::tcp::exceptions::code_of(error)) {
      using tcp_code = forge::net::tcp::exceptions::code;
      switch (*code) {
      case tcp_code::invalid_endpoint:
      case tcp_code::invalid_options:
         FORGE_THROW_CODE(exceptions::code::invalid_options, error.what());
      case tcp_code::closed:
         FORGE_THROW_CODE(exceptions::code::closed, error.what());
      case tcp_code::canceled:
         FORGE_THROW_CODE(exceptions::code::canceled, error.what());
      case tcp_code::connect_failed:
      case tcp_code::listen_failed:
      case tcp_code::accept_failed:
      case tcp_code::io_error:
         FORGE_THROW_CODE(exceptions::code::internal, error.what());
      }
   }
   throw;
}

boost::asio::awaitable<upgraded_session>
finish_noise_outbound(forge::net::p2p::stream stream, const node::options& options,
                      const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                      tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {}) {
   auto cleanup = cancel_cleanup{&deadline};
   auto secure = co_await noise_initiator(std::move(stream), identity,
                                          options.allow_insecure_test_mode ? std::nullopt : std::move(expected_peer),
                                          deadline.cancel_current);
   if (callbacks.secured) {
      callbacks.secured(secure.peer);
   }
   if (callbacks.established) {
      try {
         callbacks.established(secure.peer);
      } catch (...) {
         secure.secure->request_cancel();
         throw;
      }
   }
   auto muxer_stream = secure_transport_stream(std::move(secure.secure));
   auto muxer = secure.muxer.value_or(protocol_id{.value = "/yamux/1.0.0"});
   if (!secure.muxer) {
      auto negotiated =
          co_await protocol_negotiation::async_select(std::move(muxer_stream), muxer);
      muxer_stream = std::move(negotiated).into_transport_stream();
   }
   const auto yamux_role = forge::net::yamux::side::initiator;
   auto yamux = std::make_shared<forge::net::yamux::session>(std::move(muxer_stream), yamux_role);
   if (callbacks.upgraded) {
      callbacks.upgraded(secure.peer);
   }
   set_cancel(deadline, [yamux] { yamux->cancel(); });
   co_return upgraded_session{
       .peer = std::move(secure.peer),
       .session = std::move(yamux),
       .authentication = peer_authentication::noise,
       .muxer = std::move(muxer),
       .used_early_muxer_negotiation = secure.muxer.has_value(),
       .role = upgrade_role::initiator,
       .security_role = forge::net::tls::endpoint_role::client,
       .yamux_role = yamux_role,
   };
}

boost::asio::awaitable<upgraded_session>
finish_noise_inbound(forge::net::p2p::stream stream, const node::options& options,
                     const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                     tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {}) {
   auto cleanup = cancel_cleanup{&deadline};
   auto secure = co_await noise_responder(std::move(stream), identity,
                                          options.allow_insecure_test_mode ? std::nullopt : std::move(expected_peer),
                                          deadline.cancel_current);
   if (callbacks.secured) {
      callbacks.secured(secure.peer);
   }
   if (callbacks.established) {
      try {
         callbacks.established(secure.peer);
      } catch (...) {
         secure.secure->request_cancel();
         throw;
      }
   }
   auto muxer_stream = secure_transport_stream(std::move(secure.secure));
   auto muxer = secure.muxer.value_or(protocol_id{.value = "/yamux/1.0.0"});
   if (!secure.muxer) {
      auto negotiated =
          co_await protocol_negotiation::async_accept(std::move(muxer_stream), {muxer});
      muxer = std::move(negotiated.protocol);
      muxer_stream = std::move(negotiated.stream).into_transport_stream();
   }
   const auto yamux_role = forge::net::yamux::side::responder;
   auto yamux = std::make_shared<forge::net::yamux::session>(std::move(muxer_stream), yamux_role);
   if (callbacks.upgraded) {
      callbacks.upgraded(secure.peer);
   }
   set_cancel(deadline, [yamux] { yamux->cancel(); });
   co_return upgraded_session{
       .peer = std::move(secure.peer),
       .session = std::move(yamux),
       .authentication = peer_authentication::noise,
       .muxer = std::move(muxer),
       .used_early_muxer_negotiation = secure.muxer.has_value(),
       .role = upgrade_role::responder,
       .security_role = forge::net::tls::endpoint_role::server,
       .yamux_role = yamux_role,
   };
}

[[nodiscard]] const forge::net::pnet::protector& private_protector(const node::options& options) {
   if (!options.private_network || !options.private_network->protector) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P private-network profile requires a pnet protector");
   }
   return *options.private_network->protector;
}

boost::asio::awaitable<upgraded_session>
upgrade_outbound_private_tcp(forge::net::tcp::connection connection, const node::options& options,
                             const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                             tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   auto protect_stop = std::make_shared<std::stop_source>();
   set_cancel(deadline, [protect_stop] { protect_stop->request_stop(); });
   auto protected_connection = forge::net::transport::stream_connection{};
   try {
      protected_connection = co_await private_protector(options).async_protect(
          std::move(connection).into_transport_stream(), protect_stop->get_token());
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
   clear_cancel(deadline);
   auto local = std::move(protected_connection.local_endpoint);
   auto remote = std::move(protected_connection.remote_endpoint);
   auto protected_stream =
       std::make_shared<forge::net::p2p::stream>(forge::net::p2p::stream{std::move(protected_connection.stream)});
   set_cancel(deadline, [protected_stream] { protected_stream->request_cancel(); });
   auto selected = protocol_id{};
   try {
      selected = co_await detail::select_private_stream_security_protocol(*protected_stream);
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
   clear_cancel(deadline);
   try {
      if (selected.value == "/tls/1.0.0") {
         auto source = forge::net::transport::stream_connection{
             .local_endpoint = std::move(local),
             .remote_endpoint = std::move(remote),
             .stream = detail::adapt_private_transport_stream(
                 std::move(*protected_stream).into_transport_stream()),
         };
         co_return co_await finish_tls_outbound(std::move(source), options, identity, std::move(expected_peer), deadline,
                                                std::move(callbacks));
      }
      co_return co_await finish_noise_outbound(std::move(*protected_stream), options, identity,
                                               std::move(expected_peer), deadline, std::move(callbacks));
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
}

boost::asio::awaitable<upgraded_session>
upgrade_inbound_private_tcp(forge::net::tcp::connection connection, const node::options& options,
                            const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                            tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   auto protect_stop = std::make_shared<std::stop_source>();
   set_cancel(deadline, [protect_stop] { protect_stop->request_stop(); });
   auto protected_connection = forge::net::transport::stream_connection{};
   try {
      protected_connection = co_await private_protector(options).async_protect(
          std::move(connection).into_transport_stream(), protect_stop->get_token());
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
   clear_cancel(deadline);
   auto local = std::move(protected_connection.local_endpoint);
   auto remote = std::move(protected_connection.remote_endpoint);
   auto protected_stream =
       std::make_shared<forge::net::p2p::stream>(forge::net::p2p::stream{std::move(protected_connection.stream)});
   set_cancel(deadline, [protected_stream] { protected_stream->request_cancel(); });
   auto selected = protocol_id{};
   try {
      selected = co_await detail::accept_private_stream_security_protocol(*protected_stream);
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
   clear_cancel(deadline);
   try {
      if (selected.value == "/tls/1.0.0") {
         auto source = forge::net::transport::stream_connection{
             .local_endpoint = std::move(local),
             .remote_endpoint = std::move(remote),
             .stream = detail::adapt_private_transport_stream(
                 std::move(*protected_stream).into_transport_stream()),
         };
         co_return co_await finish_tls_inbound(std::move(source), options, identity, std::move(expected_peer), deadline,
                                               std::move(callbacks));
      }
      co_return co_await finish_noise_inbound(std::move(*protected_stream), options, identity,
                                              std::move(expected_peer), deadline, std::move(callbacks));
   } catch (const forge::exceptions::base& error) {
      rethrow_private_transport_as_p2p(error);
   }
}

} // namespace

boost::asio::awaitable<protocol_id>
detail::select_private_stream_security_protocol(forge::net::p2p::stream& stream) {
   const auto protocols = std::array{
       protocol_id{.value = "/tls/1.0.0"},
       protocol_id{.value = "/noise"},
   };
   auto selected = co_await select_protocol(stream, protocols);
   stream = detail::stream_access::with_buffer(std::move(stream), std::move(selected.buffered));
   co_return std::move(selected.protocol);
}

boost::asio::awaitable<protocol_id>
detail::accept_private_stream_security_protocol(forge::net::p2p::stream& stream) {
   const auto protocols = std::array{
       protocol_id{.value = "/tls/1.0.0"},
       protocol_id{.value = "/noise"},
   };
   auto selected = co_await accept_protocol(stream, protocols);
   stream = detail::stream_access::with_buffer(std::move(stream), std::move(selected.buffered));
   co_return std::move(selected.protocol);
}

boost::asio::awaitable<upgraded_session> upgrade_outbound_stream(forge::net::p2p::stream stream,
                                                                 const node::options& options,
                                                                 const libp2p_identity_material& identity,
                                                                 std::optional<peer_id> expected_peer,
                                                                 upgrade_callbacks callbacks) {
   const auto noise_protocol = protocol_id{.value = "/noise"};
   auto noise_stream = co_await protocol_negotiation::async_select(std::move(stream), noise_protocol);
   co_return co_await finish_noise_outbound(std::move(noise_stream), options, identity, std::move(expected_peer), {},
                                            std::move(callbacks));
}

boost::asio::awaitable<upgraded_session> upgrade_inbound_stream(forge::net::p2p::stream stream,
                                                                const node::options& options,
                                                                const libp2p_identity_material& identity,
                                                                std::optional<peer_id> expected_peer,
                                                                upgrade_callbacks callbacks) {
   const auto noise_protocol = protocol_id{.value = "/noise"};
   auto noise_stream = co_await protocol_negotiation::async_accept(std::move(stream), {noise_protocol});
   co_return co_await finish_noise_inbound(std::move(noise_stream.stream), options, identity, std::move(expected_peer),
                                           {}, std::move(callbacks));
}

boost::asio::awaitable<upgraded_session>
upgrade_tcp(forge::net::tcp::connection connection, const node::options& options,
             const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
             upgrade_role role, tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   switch (role) {
   case upgrade_role::initiator:
      co_return co_await upgrade_outbound_tcp(std::move(connection), options, identity, std::move(expected_peer),
                                               std::move(deadline), std::move(callbacks));
   case upgrade_role::responder:
      co_return co_await upgrade_inbound_tcp(std::move(connection), options, identity, std::move(expected_peer),
                                              std::move(deadline), std::move(callbacks));
   }
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid TCP security upgrade role");
}

boost::asio::awaitable<upgraded_session> upgrade_outbound_tcp(forge::net::tcp::connection connection,
                                                              const node::options& options,
                                                              const libp2p_identity_material& identity,
                                                              std::optional<peer_id> expected_peer) {
   co_return co_await upgrade_outbound_tcp(std::move(connection), options, identity, std::move(expected_peer), {});
}

boost::asio::awaitable<upgraded_session>
upgrade_outbound_tcp(forge::net::tcp::connection connection, const node::options& options,
                     const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                     tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   if (options.private_network) {
      co_return co_await upgrade_outbound_private_tcp(std::move(connection), options, identity,
                                                       std::move(expected_peer), deadline, std::move(callbacks));
   }
   auto cleanup = cancel_cleanup{&deadline};
   set_cancel(deadline, [&connection] { connection.cancel(); });
   const auto protocols = std::array{
       protocol_id{.value = "/tls/1.0.0"},
       protocol_id{.value = "/noise"},
   };
   const auto selected = co_await select_protocol(connection, protocols);
   clear_cancel(deadline);
   if (selected.protocol.value == "/tls/1.0.0") {
      co_return co_await finish_tls_outbound(std::move(connection), options, identity, std::move(expected_peer),
                                             deadline, std::move(callbacks));
   }
   auto stream = std::move(connection).into_transport_stream();
   co_return co_await finish_noise_outbound(forge::net::p2p::stream{std::move(stream.stream)}, options, identity,
                                            std::move(expected_peer), deadline, std::move(callbacks));
}

boost::asio::awaitable<upgraded_session> upgrade_inbound_tcp(forge::net::tcp::connection connection,
                                                             const node::options& options,
                                                             const libp2p_identity_material& identity,
                                                             std::optional<peer_id> expected_peer) {
   co_return co_await upgrade_inbound_tcp(std::move(connection), options, identity, std::move(expected_peer), {});
}

boost::asio::awaitable<upgraded_session>
upgrade_inbound_tcp(forge::net::tcp::connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   if (options.private_network) {
      co_return co_await upgrade_inbound_private_tcp(std::move(connection), options, identity,
                                                      std::move(expected_peer), deadline, std::move(callbacks));
   }
   auto cleanup = cancel_cleanup{&deadline};
   set_cancel(deadline, [&connection] { connection.cancel(); });
   const auto protocols = std::array{
       protocol_id{.value = "/tls/1.0.0"},
       protocol_id{.value = "/noise"},
   };
   const auto selected = co_await accept_protocol(connection, protocols);
   clear_cancel(deadline);
   if (selected.protocol.value == "/tls/1.0.0") {
      co_return co_await finish_tls_inbound(std::move(connection), options, identity, std::move(expected_peer),
                                            deadline, std::move(callbacks));
   }
   auto stream = std::move(connection).into_transport_stream();
   co_return co_await finish_noise_inbound(forge::net::p2p::stream{std::move(stream.stream)}, options, identity,
                                           std::move(expected_peer), deadline, std::move(callbacks));
}

} // namespace forge::net::p2p
