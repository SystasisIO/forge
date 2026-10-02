module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.tcp.connection;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/relay_hop_exchange.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

namespace {

[[nodiscard]] endpoint relay_circuit_endpoint(endpoint relay_endpoint, const peer_id& relay_peer,
                                              const peer_id& circuit_peer) {
   relay_endpoint.peer = relay_peer;
   relay_endpoint.relayed = endpoint::circuit{.target = circuit_peer};
   return relay_endpoint;
}

[[nodiscard]] const endpoint& relay_transport_endpoint(const auto& session) {
   if (session.direct_endpoint) {
      return *session.direct_endpoint;
   }
   if (session.remote_endpoint) {
      return *session.remote_endpoint;
   }
   FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relay session has no canonical direct endpoint");
}

} // namespace

boost::asio::awaitable<upgraded_session>
node::impl::open_relay_yamux(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout,
                             std::function<void(const peer_id&)> authenticated_admission) {
   const auto started = std::chrono::steady_clock::now();
   record_path_attempt(path::kind::relay);
   auto relay_session = co_await ensure_direct_session(relay_peer, timeout);
   try {
      const auto& relay_endpoint = relay_transport_endpoint(*relay_session);
      const auto local_endpoint = relay_circuit_endpoint(relay_endpoint, relay_peer, local);
      const auto remote_endpoint = relay_circuit_endpoint(relay_endpoint, relay_peer, peer);
      connection_gate->address_dial(peer, remote_endpoint);
      auto exchange = co_await detail::async_exchange_relay_hop(
          runtime.context(), remaining_timeout(started, timeout, "P2P relay protocol open"), "P2P relay protocol open",
          [this, relay_session](
              detail::stream_admission_handler admitted) -> boost::asio::awaitable<forge::net::p2p::stream> {
             co_return co_await open_session_stream(relay_session, builtins::relay_hop, true, std::move(admitted));
          },
          relay::hop_message{
              .kind = relay::hop_message::message_kind::connect,
              .target = relay::peer{.id = peer},
          },
          reachability::options{}.max_message_size);
      auto response = std::move(exchange.response);
      if (response.kind != relay::hop_message::message_kind::status || response.status != relay::status::ok) {
         FORGE_THROW_CODE(response.kind == relay::hop_message::message_kind::status ? exceptions::code::relay_rejected
                                                                                    : exceptions::code::protocol_error,
                          response.kind == relay::hop_message::message_kind::status
                              ? "P2P relay open rejected with status " +
                                    std::to_string(static_cast<std::uint16_t>(response.status))
                              : "P2P relay open rejected with unexpected response");
      }
      record_path_open(path::kind::relay);
      auto stream = detail::stream_access::with_buffer(std::move(exchange.stream), std::move(exchange.buffered));
      co_return co_await upgrade_relay_outbound_session(
          std::move(stream), options, identity, peer,
          upgrade_callbacks{
              .secured =
                  [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                     gate->secured(connection_direction::outbound, authenticated_peer, local_endpoint, remote_endpoint);
                  },
              .established = std::move(authenticated_admission),
              .upgraded =
                  [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                     gate->upgraded(connection_direction::outbound, authenticated_peer, local_endpoint,
                                    remote_endpoint);
                  },
          });
   } catch (const forge::exceptions::base& error) {
      if (p2p_code(error) != exceptions::code::connection_rejected) {
         record_relay_failure();
      }
      rethrow_transport_as_p2p(error);
   }
}

boost::asio::awaitable<std::shared_ptr<node::impl::session_state>>
node::impl::ensure_relay_session(const peer_id& peer, const peer_id& relay_peer, std::chrono::milliseconds timeout) {
   if (auto existing = session_for_path(peer, path::kind::relay, relay_peer)) {
      co_return existing;
   }
   connection_gate->peer_dial(peer);
   auto reservation = resources.reserve_session(resource_manager::session_direction::outbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.connection_rejections;
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P pending outbound relay session limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound relay session resource admission failed");
   }
   auto upgraded = co_await open_relay_yamux(
       peer, relay_peer, timeout, [this, &reservation](const peer_id& authenticated_peer) {
          const auto transition = reservation->establish(resource_manager::session_scope{
              .peer = authenticated_peer,
              .direction = resource_manager::session_direction::outbound,
          });
          if (transition != resource_manager::transition_result::accepted) {
             if (transition != resource_manager::transition_result::policy_rejected) {
                FORGE_THROW_EXCEPTION(exceptions::internal, "P2P outbound relay session resource transition failed");
             }
             {
                auto lock = std::scoped_lock{mutex};
                ++metrics_value.backpressure_rejections;
                ++metrics_value.connection_rejections;
             }
             FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                   "P2P established outbound relay session limit reached");
          }
       });
   auto session = std::make_shared<session_state>();
   session->info = node::session_info{
       .remote_peer = std::move(upgraded.peer),
       .path = path::kind::relay,
       .relay_peer = relay_peer,
   };
   session->authentication = upgraded.authentication;
   session->connection = std::move(*upgraded.session).as_transport();
   session->resource = std::move(*reservation);
   co_await remember_session(session, connection_manager::direction::outbound);
   launch_session_accept_loop(session);
   launch_identify(session);
   co_return session;
}

boost::asio::awaitable<forge::net::p2p::stream> node::impl::open_protocol_via_relay(const peer_id& peer,
                                                                                    const protocol_id& protocol,
                                                                                    const peer_id& relay_peer,
                                                                                    std::chrono::milliseconds timeout) {
   auto session = co_await ensure_relay_session(peer, relay_peer, timeout);
   co_return co_await open_session_stream(session, protocol);
}

boost::asio::awaitable<void> node::impl::handle_relayed_yamux_stream(std::shared_ptr<node::impl::session_state> session,
                                                                     forge::net::transport::stream stream,
                                                                     resource_manager::stream_reservation reservation) {
   auto admitted =
       co_await accept_resource_stream(session->info.remote_peer, std::move(stream), std::move(reservation));
   detail::stream_access::set_authentication(admitted.stream, session->authentication);
   if (admitted.protocol == builtins::ping) {
      co_await handle_ping(std::move(admitted.stream));
   } else if (admitted.protocol == builtins::identify) {
      co_await handle_identify(session, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::identify_push) {
      co_await handle_identify_push(session, std::move(admitted.stream), std::move(admitted.resource));
   } else if (admitted.protocol == builtins::dcutr) {
      co_await handle_dcutr(session, std::move(admitted.stream));
   } else if (dht_profiles.contains(admitted.protocol)) {
      co_await handle_dht(session, admitted.protocol, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::rendezvous) {
      co_await handle_rendezvous(session, std::move(admitted.stream));
   } else if (admitted.protocol == builtins::meshsub_v11 || admitted.protocol == builtins::meshsub_v10) {
      co_await handle_pubsub(session, std::move(admitted.stream));
   } else {
      auto handler = handler_for(admitted.protocol);
      if (!handler) {
         increment_protocol_rejected();
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "unsupported negotiated relayed P2P protocol");
      }
      increment_protocol_accepted();
      co_await (*handler)(node::incoming_protocol_stream{
          .session = session_info_for(session),
          .protocol = admitted.protocol,
          .stream = std::move(admitted.stream),
      });
   }
   co_await detail::async_close_unescaped(admitted.resource);
}

boost::asio::awaitable<void> node::impl::handle_relay_stop(std::shared_ptr<node::impl::session_state> session,
                                                           forge::net::p2p::stream stream) {
   auto relay_buffer = std::vector<std::uint8_t>{};
   auto request = relay::codec::decode_stop(
       co_await async_read_length_delimited(stream, relay_buffer, reachability::options{}.max_message_size));
   if (request.kind != relay::stop_message::message_kind::connect || !request.source) {
      co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
          .kind = relay::stop_message::message_kind::status,
          .status = relay::status::malformed_message,
      }));
      co_return;
   }
   const auto ownership = [this, &session] {
      const auto lock = std::scoped_lock{mutex};
      cleanup_expired_relay_reservations_locked();
      const auto live = sessions.find(session->id);
      const auto owner = outbound_relay_reservations.find(session->info.remote_peer);
      return options.relay_policy.client_enabled && !private_network_enabled() && !stopped &&
          !session_admission_closed && !session->closed && session->info.path == path::kind::direct &&
          session->authentication != peer_authentication::unverified &&
          live != sessions.end() && live->second == session && owner != outbound_relay_reservations.end() &&
          !owner->second.canceled && owner->second.session_id == session->id &&
          owner->second.expires_at > std::chrono::steady_clock::now();
   };
   if (!ownership()) {
      co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
          .kind = relay::stop_message::message_kind::status,
          .status = relay::status::permission_denied,
      }));
      co_return;
   }
   const auto& relay_endpoint = relay_transport_endpoint(*session);
   const auto local_endpoint = relay_circuit_endpoint(relay_endpoint, session->info.remote_peer, local);
   const auto remote_endpoint = relay_circuit_endpoint(relay_endpoint, session->info.remote_peer, request.source->id);
   connection_gate->accept(local_endpoint, remote_endpoint);
   auto reservation = resources.reserve_session(resource_manager::session_direction::inbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.backpressure_rejections;
            ++metrics_value.connection_rejections;
         }
         co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
             .kind = relay::stop_message::message_kind::status,
             .status = relay::status::resource_limit_exceeded,
         }));
         co_return;
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P inbound relay session resource admission failed");
   }
   if (!ownership()) {
      co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
          .kind = relay::stop_message::message_kind::status,
          .status = relay::status::permission_denied,
      }));
      co_return;
   }
   co_await stream.async_write(relay::codec::encode_stop(relay::stop_message{
       .kind = relay::stop_message::message_kind::status,
       .limit_value = request.limit_value,
       .status = relay::status::ok,
   }));
   stream = detail::stream_access::with_buffer(std::move(stream), std::move(relay_buffer));
   auto upgraded = co_await upgrade_relay_inbound_session(
       std::move(stream), options, identity, request.source->id,
       upgrade_callbacks{
           .secured =
               [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                  gate->secured(connection_direction::inbound, authenticated_peer, local_endpoint, remote_endpoint);
           },
           .established =
               [&reservation, &ownership](const peer_id& authenticated_peer) {
                  if (!ownership()) {
                     FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay reservation owner was lost during inbound upgrade");
                  }
                  const auto transition = reservation->establish(resource_manager::session_scope{
                      .peer = authenticated_peer,
                      .direction = resource_manager::session_direction::inbound,
                  });
                  if (transition != resource_manager::transition_result::accepted) {
                     if (transition != resource_manager::transition_result::policy_rejected) {
                        FORGE_THROW_EXCEPTION(exceptions::internal,
                                              "P2P inbound relay session resource transition failed");
                     }
                     FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                           "P2P established inbound relay session limit reached");
                  }
               },
           .upgraded =
               [gate = connection_gate, local_endpoint, remote_endpoint](const peer_id& authenticated_peer) {
                  gate->upgraded(connection_direction::inbound, authenticated_peer, local_endpoint, remote_endpoint);
               },
       });
   auto relayed_session = std::make_shared<session_state>();
   relayed_session->info = node::session_info{
       .remote_peer = std::move(upgraded.peer),
       .path = path::kind::relay,
       .relay_peer = session->info.remote_peer,
   };
   relayed_session->authentication = upgraded.authentication;
   relayed_session->connection = std::move(*upgraded.session).as_transport();
   relayed_session->resource = std::move(*reservation);
   co_await remember_session(relayed_session, connection_manager::direction::inbound);
   launch_session_accept_loop(relayed_session);
   launch_identify(relayed_session);
   if (options.capabilities.has(capabilities::hole_punching)) {
      auto self = shared_from_this();
      static_cast<void>(launch_tracked([self, relayed_session]() -> asio::awaitable<void> {
         static_cast<void>(co_await self->run_dcutr_initiator(relayed_session->info.remote_peer, relayed_session,
                                                              std::chrono::milliseconds{10'000}));
      }));
   }
}


} // namespace forge::net::p2p
