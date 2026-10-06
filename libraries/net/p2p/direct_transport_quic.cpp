module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.asio.notification;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.scoring;
import forge.net.p2p.identity;
import forge.net.p2p.resource_manager;
import forge.multiformats.exceptions;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.multiformats.multicodec;
import forge.multiformats.multihash;
import forge.multiformats.multibase;
import forge.multiformats.multiaddr;
import forge.net.quic.connection;
import forge.net.quic.connector;
import forge.net.quic.endpoint;
import forge.net.quic.exceptions;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.quic.transport;
import forge.net.transport.limits;
import forge.net.transport.session;
import forge.net.p2p.stream;
import forge.net.tcp.connection;
import forge.net.yamux.session;

#include "details/direct_transport.hxx"
#include "details/cancellation_latch.hxx"
#include "details/connection_gate.hxx"
#include "details/owner_cancellation.hxx"
#include "details/pending_quic_connection.hxx"
#include "details/quic_client_token_cache.hxx"
#include "details/quic_client_options.hxx"
#include "details/operation_deadline.hxx"
#include "details/stream_upgrade.hxx"
#include "details/quic_profile.hxx"
#include "details/coordinated_dial.hxx"

namespace forge::net::p2p::direct {

void register_quic_profile(registry& value, forge::asio::runtime& runtime, const node::options& options,
                           resource_manager resources, std::shared_ptr<forge::net::p2p::detail::connection_gate> gate) {
   auto owned = std::make_shared<quic_profile>(runtime, options, std::move(resources), std::move(gate));
   value.add(profile{
       .supports = [owned](const forge::net::p2p::endpoint& endpoint) { return owned->supports(endpoint); },
       .listening = [owned] { return owned->listening(); },
       .local_endpoints = [owned] { return owned->local_endpoints(); },
       .listen = [owned](forge::net::p2p::endpoint endpoint) { return owned->listen(std::move(endpoint)); },
       .stop = [owned] { owned->stop(); },
       .async_stop = [owned] { return owned->async_stop(); },
       .async_connect =
           [owned](forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                   std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                   authenticated_admission_handler authenticated,
                   tcp_transport_progress_handler tcp_transport_progress) {
              return owned->async_connect(std::move(endpoint), options, std::move(cancellation),
                                          std::move(native_lifetime), std::move(authenticated),
                                          std::move(tcp_transport_progress));
           },
       .async_connect_admitted =
           [owned](endpoint remote, const node::connect_options& options,
                   std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                   authenticated_admission_handler authenticated,
                   tcp_transport_progress_handler tcp_transport_progress,
                   native_socket_admission_handler socket_admission) {
              return owned->async_connect(std::move(remote), options, std::move(cancellation),
                                          std::move(native_lifetime), std::move(authenticated),
                                          std::move(tcp_transport_progress), {}, false, std::move(socket_admission));
           },
       .async_connect_coordinated =
           [owned](endpoint remote, peer_id peer, upgrade_role role, std::chrono::milliseconds budget,
                   std::shared_ptr<cancellation_latch> cancellation, authenticated_admission_handler authenticated,
                   std::optional<endpoint> local_source) {
              return owned->async_connect_coordinated(std::move(remote), std::move(peer), role, budget,
                                                      std::move(cancellation), std::move(authenticated),
                                                      std::move(local_source));
           },
       .async_accept = [owned](forge::net::p2p::endpoint endpoint) { return owned->async_accept(std::move(endpoint)); },
       .prepare_coordinated = [owned](const auto& operation) { owned->prepare_coordinated(operation); },
       .release_coordinated = [owned](const auto& operation) { owned->release_coordinated(operation); },
       .async_connect_coordinated_owned = [owned](std::shared_ptr<forge::net::p2p::detail::coordinated_dial> operation) {
          const auto budget = std::chrono::ceil<std::chrono::milliseconds>(
              operation->deadline - std::chrono::steady_clock::now());
          if (budget <= std::chrono::milliseconds::zero()) {
             FORGE_THROW_EXCEPTION(exceptions::timeout, "coordinated QUIC deadline expired before native dial");
          }
          return owned->async_connect_coordinated(operation->remote, operation->options.expected_peer,
              operation->role, budget, operation->transports, {}, operation->options.local_source, operation);
       },
   });
}

} // namespace forge::net::p2p::direct
