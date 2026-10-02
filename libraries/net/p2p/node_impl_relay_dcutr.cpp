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
#include <boost/asio/use_awaitable.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

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
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/lifecycle_wakeup.hxx"
#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

boost::asio::awaitable<void> node::impl::handle_dcutr(std::shared_ptr<node::impl::session_state> session,
                                                      forge::net::p2p::stream stream) {
   auto buffer = std::vector<std::uint8_t>{};
   auto first = co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size);
   auto request = hole_punch::codec::decode(first);
   if (request.kind != hole_punch::message::message_kind::connect) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected CONNECT");
   }
   auto observed = local_endpoints_for_control();
   co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
       .kind = hole_punch::message::message_kind::connect,
       .observed_endpoints = std::move(observed),
   }));
   auto sync_bytes = co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size);
   auto sync = hole_punch::codec::decode(sync_bytes);
   if (sync.kind != hole_punch::message::message_kind::sync) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected SYNC");
   }
   for (const auto& candidate : request.observed_endpoints) {
      try {
         (void)co_await connect_direct(candidate, node::connect_options{
                                                      .expected_peer = session->info.remote_peer,
                                                      .allow_relay = false,
                                                      .timeout = std::chrono::milliseconds{5'000},
                                                  });
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return;
      } catch (const forge::exceptions::base& error) {
         if (detail::remote_peer_attributable_failure(p2p_code(error), false)) {
            record_direct_failure(session->info.remote_peer);
         }
      } catch (...) {
         record_direct_failure(session->info.remote_peer);
      }
   }
   if (co_await wait_for_direct_session(session->info.remote_peer, std::chrono::milliseconds{5'000})) {
      record_hole_punch_result(hole_punch::status::succeeded);
      co_return;
   }
   record_hole_punch_result(hole_punch::status::failed);
}

boost::asio::awaitable<bool> node::impl::wait_for_direct_session(const peer_id& peer,
                                                                 std::chrono::milliseconds timeout) {
   const auto started = std::chrono::steady_clock::now();
   while (std::chrono::steady_clock::now() - started < timeout) {
      const auto observed = lifecycle_wakeup->epoch();
      if (lifecycle.stop_requested()) {
         co_return false;
      }
      if (session_for_path(peer, path::kind::direct)) {
         co_return !lifecycle.stop_requested();
      }
      // Session admission and lifecycle stop both notify this sticky epoch.
      co_await lifecycle_wakeup->async_wait_until(observed, started + timeout);
   }
   co_return false;
}

boost::asio::awaitable<hole_punch::status>
node::impl::run_dcutr_initiator(const peer_id& peer, const std::shared_ptr<session_state>& session,
                                std::chrono::milliseconds timeout) {
   auto observed = local_endpoints_for_control();
   if (observed.empty()) {
      record_hole_punch_result(hole_punch::status::failed);
      co_return hole_punch::status::failed;
   }
   try {
      auto stream = co_await open_session_stream(session, builtins::dcutr);
      const auto sent = std::chrono::steady_clock::now();
      co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
          .kind = hole_punch::message::message_kind::connect,
          .observed_endpoints = observed,
      }));
      auto dcutr_buffer = std::vector<std::uint8_t>{};
      auto response = hole_punch::codec::decode(
          co_await async_read_length_delimited(stream, dcutr_buffer, hole_punch::options{}.max_message_size));
      const auto rtt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - sent);
      if (response.kind != hole_punch::message::message_kind::connect || response.observed_endpoints.empty()) {
         record_hole_punch_result(hole_punch::status::failed);
         co_return hole_punch::status::failed;
      }
      co_await stream.async_write(
          hole_punch::codec::encode(hole_punch::message{.kind = hole_punch::message::message_kind::sync}));
      if (rtt > std::chrono::milliseconds{0}) {
         auto timer = asio::steady_timer{runtime.context()};
         timer.expires_after(rtt / 2);
         co_await timer.async_wait(asio::use_awaitable);
      }
      for (const auto& candidate : response.observed_endpoints) {
         if (lifecycle.stop_requested()) {
            co_return hole_punch::status::failed;
         }
         try {
            (void)co_await connect_direct(candidate, node::connect_options{
                                                         .expected_peer = peer,
                                                         .allow_relay = false,
                                                         .timeout = timeout,
                                                     });
            record_hole_punch_result(hole_punch::status::succeeded);
            co_return hole_punch::status::succeeded;
         } catch (const forge::exceptions::base& error) {
            if (lifecycle.stop_requested() || p2p_code(error) == exceptions::code::canceled) {
               co_return hole_punch::status::failed;
            }
            if (detail::remote_peer_attributable_failure(p2p_code(error), false)) {
               record_direct_failure(peer);
            }
         } catch (...) {
            if (lifecycle.stop_requested()) {
               co_return hole_punch::status::failed;
            }
            record_direct_failure(peer);
         }
      }
      if (co_await wait_for_direct_session(peer, std::min(timeout, std::chrono::milliseconds{5'000}))) {
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return hole_punch::status::succeeded;
      }
   } catch (...) {
   }
   if (!lifecycle.stop_requested()) {
      record_hole_punch_result(hole_punch::status::failed);
   }
   co_return hole_punch::status::failed;
}

boost::asio::awaitable<hole_punch::status>
node::impl::attempt_hole_punch(peer_id peer, std::optional<peer_id> relay_peer, std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P hole punch timeout");
   if (session_for_path(peer, path::kind::direct)) {
      co_return hole_punch::status::succeeded;
   }
   if (!relay_peer) {
      const auto record = store.find(peer);
      if (record) {
         for (const auto& endpoint : record->endpoints) {
            if (endpoint.relay_peer) {
               relay_peer = endpoint.relay_peer;
               break;
            }
         }
      }
   }
   if (!relay_peer) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P hole punching requires a relay peer");
   }
   auto observed = local_endpoints_for_control();
   if (observed.empty()) {
      record_hole_punch_result(hole_punch::status::failed);
      co_return hole_punch::status::failed;
   }
   try {
      static_cast<void>(co_await ensure_relay_session(peer, *relay_peer, timeout));
      if (co_await wait_for_direct_session(peer, timeout)) {
         record_hole_punch_result(hole_punch::status::succeeded);
         co_return hole_punch::status::succeeded;
      }
   } catch (...) {
      // DCUtR failures are expected on many NATs; the caller sees a typed status.
   }
   record_hole_punch_result(hole_punch::status::failed);
   co_return hole_punch::status::failed;
}

} // namespace forge::net::p2p
