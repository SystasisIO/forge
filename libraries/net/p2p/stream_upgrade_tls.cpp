module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.negotiation;
import forge.net.p2p.stream;
import forge.net.stcp.connection;
import forge.net.stcp.exceptions;
import forge.net.tcp.connection;
import forge.net.transport.connector;
import forge.net.transport.stream;
import forge.net.yamux.session;
import forge.net.tls.options;

#include "details/stream_upgrade.hxx"
#include "details/libp2p_tls.hxx"
#include "details/exact_negotiation_io.hxx"

namespace forge::net::p2p::detail {

namespace {

[[nodiscard]] exceptions::code map_stcp_error(forge::net::stcp::exceptions::code kind) noexcept {
   using stcp_kind = forge::net::stcp::exceptions::code;
   switch (kind) {
   case stcp_kind::invalid_endpoint:
   case stcp_kind::invalid_options:
      return exceptions::code::invalid_options;
   case stcp_kind::connect_failed:
   case stcp_kind::listen_failed:
   case stcp_kind::accept_failed:
   case stcp_kind::io_error:
      return exceptions::code::internal;
   case stcp_kind::verification_failed:
   case stcp_kind::handshake_failed:
      return exceptions::code::peer_verification_failed;
   case stcp_kind::canceled:
      return exceptions::code::canceled;
   case stcp_kind::timeout:
      return exceptions::code::timeout;
   case stcp_kind::closed:
      return exceptions::code::closed;
   }
   return exceptions::code::internal;
}

[[noreturn]] void rethrow_stcp_as_p2p(const forge::exceptions::base& error) {
   const auto code = forge::net::stcp::exceptions::code_of(error);
   if (code) {
      FORGE_THROW_CODE(map_stcp_error(*code), error.what());
   }
   throw;
}

} // namespace

boost::asio::awaitable<upgraded_session>
finish_tls_outbound(forge::net::tcp::connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   try {
      auto stop = std::make_shared<std::stop_source>();
      set_cancel(deadline, [stop] { static_cast<void>(stop->request_stop()); });
      auto tls = std::make_shared<forge::net::stcp::connection>(
          has_timeout(deadline)
              ? co_await forge::net::stcp::async_upgrade_client(std::move(connection),
                                                                make_libp2p_tls_client_options(identity),
                                                                deadline.timeout, stop->get_token())
              : co_await forge::net::stcp::async_upgrade_client(
                    std::move(connection), make_libp2p_tls_client_options(identity), stop->get_token()));
      set_cancel(deadline, [tls] { tls->cancel(); });
      const auto handshake = verify_libp2p_tls_handshake(tls->peer_certificate_chain(), tls->selected_alpn(),
                                                         options.allow_insecure_test_mode ? std::nullopt : expected_peer);
      const auto& peer = handshake.peer;
      if (callbacks.secured) {
         callbacks.secured(peer);
      }
      if (callbacks.established) {
         callbacks.established(peer);
      }
      auto muxer = handshake.muxer ? *handshake.muxer : co_await negotiate_yamux(*tls, true);
      clear_cancel(deadline);
      auto stream = std::move(*tls).into_transport_stream();
      const auto yamux_role = forge::net::yamux::side::initiator;
      auto yamux = std::make_shared<forge::net::yamux::session>(std::move(stream.stream), yamux_role);
      if (callbacks.upgraded) {
         callbacks.upgraded(peer);
      }
      set_cancel(deadline, [yamux] { yamux->cancel(); });
      co_return upgraded_session{
          .peer = peer,
          .session = std::move(yamux),
          .authentication = peer_authentication::libp2p_tls,
          .muxer = std::move(muxer),
          .used_early_muxer_negotiation = handshake.muxer.has_value(),
          .role = upgrade_role::initiator,
          .security_role = forge::net::tls::endpoint_role::client,
          .yamux_role = yamux_role,
      };
   } catch (const forge::exceptions::base& error) {
      rethrow_stcp_as_p2p(error);
   }
}

boost::asio::awaitable<upgraded_session>
finish_tls_inbound(forge::net::tcp::connection connection, const node::options& options,
                   const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                   tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   try {
      auto stop = std::make_shared<std::stop_source>();
      set_cancel(deadline, [stop] { static_cast<void>(stop->request_stop()); });
      auto tls = std::make_shared<forge::net::stcp::connection>(
          has_timeout(deadline)
              ? co_await forge::net::stcp::async_upgrade_server(std::move(connection),
                                                                make_libp2p_tls_server_options(identity),
                                                                deadline.timeout, stop->get_token())
              : co_await forge::net::stcp::async_upgrade_server(
                    std::move(connection), make_libp2p_tls_server_options(identity), stop->get_token()));
      set_cancel(deadline, [tls] { tls->cancel(); });
      const auto handshake = verify_libp2p_tls_handshake(tls->peer_certificate_chain(), tls->selected_alpn(),
                                                         options.allow_insecure_test_mode ? std::nullopt : expected_peer);
      const auto& peer = handshake.peer;
      if (callbacks.secured) {
         callbacks.secured(peer);
      }
      if (callbacks.established) {
         callbacks.established(peer);
      }
      auto muxer = handshake.muxer ? *handshake.muxer : co_await negotiate_yamux(*tls, false);
      clear_cancel(deadline);
      auto stream = std::move(*tls).into_transport_stream();
      const auto yamux_role = forge::net::yamux::side::responder;
      auto yamux = std::make_shared<forge::net::yamux::session>(std::move(stream.stream), yamux_role);
      if (callbacks.upgraded) {
         callbacks.upgraded(peer);
      }
      set_cancel(deadline, [yamux] { yamux->cancel(); });
      co_return upgraded_session{
          .peer = peer,
          .session = std::move(yamux),
          .authentication = peer_authentication::libp2p_tls,
          .muxer = std::move(muxer),
          .used_early_muxer_negotiation = handshake.muxer.has_value(),
          .role = upgrade_role::responder,
          .security_role = forge::net::tls::endpoint_role::server,
          .yamux_role = yamux_role,
      };
   } catch (const forge::exceptions::base& error) {
      rethrow_stcp_as_p2p(error);
   }
}

boost::asio::awaitable<upgraded_session>
finish_tls_outbound(forge::net::transport::stream_connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   try {
      auto stop = std::make_shared<std::stop_source>();
      set_cancel(deadline, [stop] { static_cast<void>(stop->request_stop()); });
      const auto timeout = has_timeout(deadline) ? std::optional<std::chrono::milliseconds>{deadline.timeout}
                                                 : std::optional<std::chrono::milliseconds>{};
      auto tls = std::make_shared<forge::net::stcp::connection>(
          co_await forge::net::stcp::async_upgrade_client(std::move(connection), make_libp2p_tls_client_options(identity),
                                                           timeout, stop->get_token()));
      set_cancel(deadline, [tls] { tls->cancel(); });
      const auto handshake = verify_libp2p_tls_handshake(tls->peer_certificate_chain(), tls->selected_alpn(),
                                                         options.allow_insecure_test_mode ? std::nullopt : expected_peer);
      const auto& peer = handshake.peer;
      if (callbacks.secured) {
         callbacks.secured(peer);
      }
      if (callbacks.established) {
         callbacks.established(peer);
      }
      auto muxer = handshake.muxer ? *handshake.muxer : co_await negotiate_yamux(*tls, true);
      clear_cancel(deadline);
      auto stream = std::move(*tls).into_transport_stream();
      const auto yamux_role = forge::net::yamux::side::initiator;
      auto yamux = std::make_shared<forge::net::yamux::session>(std::move(stream.stream), yamux_role);
      if (callbacks.upgraded) {
         callbacks.upgraded(peer);
      }
      set_cancel(deadline, [yamux] { yamux->cancel(); });
      co_return upgraded_session{
          .peer = peer,
          .session = std::move(yamux),
          .authentication = peer_authentication::libp2p_tls,
          .muxer = std::move(muxer),
          .used_early_muxer_negotiation = handshake.muxer.has_value(),
          .role = upgrade_role::initiator,
          .security_role = forge::net::tls::endpoint_role::client,
          .yamux_role = yamux_role,
      };
   } catch (const forge::exceptions::base& error) {
      rethrow_stcp_as_p2p(error);
   }
}

boost::asio::awaitable<upgraded_session>
finish_tls_inbound(forge::net::transport::stream_connection connection, const node::options& options,
                   const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                   tcp_upgrade_deadline deadline, upgrade_callbacks callbacks) {
   auto cleanup = cancel_cleanup{&deadline};
   try {
      auto stop = std::make_shared<std::stop_source>();
      set_cancel(deadline, [stop] { static_cast<void>(stop->request_stop()); });
      const auto timeout = has_timeout(deadline) ? std::optional<std::chrono::milliseconds>{deadline.timeout}
                                                 : std::optional<std::chrono::milliseconds>{};
      auto tls = std::make_shared<forge::net::stcp::connection>(
          co_await forge::net::stcp::async_upgrade_server(std::move(connection), make_libp2p_tls_server_options(identity),
                                                           timeout, stop->get_token()));
      set_cancel(deadline, [tls] { tls->cancel(); });
      const auto handshake = verify_libp2p_tls_handshake(tls->peer_certificate_chain(), tls->selected_alpn(),
                                                         options.allow_insecure_test_mode ? std::nullopt : expected_peer);
      const auto& peer = handshake.peer;
      if (callbacks.secured) {
         callbacks.secured(peer);
      }
      if (callbacks.established) {
         callbacks.established(peer);
      }
      auto muxer = handshake.muxer ? *handshake.muxer : co_await negotiate_yamux(*tls, false);
      clear_cancel(deadline);
      auto stream = std::move(*tls).into_transport_stream();
      const auto yamux_role = forge::net::yamux::side::responder;
      auto yamux = std::make_shared<forge::net::yamux::session>(std::move(stream.stream), yamux_role);
      if (callbacks.upgraded) {
         callbacks.upgraded(peer);
      }
      set_cancel(deadline, [yamux] { yamux->cancel(); });
      co_return upgraded_session{
          .peer = peer,
          .session = std::move(yamux),
          .authentication = peer_authentication::libp2p_tls,
          .muxer = std::move(muxer),
          .used_early_muxer_negotiation = handshake.muxer.has_value(),
          .role = upgrade_role::responder,
          .security_role = forge::net::tls::endpoint_role::server,
          .yamux_role = yamux_role,
      };
   } catch (const forge::exceptions::base& error) {
      rethrow_stcp_as_p2p(error);
   }
}

} // namespace forge::net::p2p::detail
