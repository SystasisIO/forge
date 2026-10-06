module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <exception>
#include <optional>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.scoring;
import forge.net.p2p.identity;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;

#include "details/direct_transport.hxx"
#include "details/coordinated_dial.hxx"
#include "details/connection_gate.hxx"

namespace forge::net::p2p::direct {
namespace {

[[nodiscard]] profile& profile_for(std::vector<profile>& profiles, const forge::net::p2p::endpoint& endpoint) {
   for (auto& candidate : profiles) {
      if (candidate.supports(endpoint)) {
         return candidate;
      }
   }
   FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "unsupported P2P direct transport");
}

} // namespace

registry::registry(forge::asio::runtime& runtime, const node::options& options,
                   const libp2p_identity_material& identity, resource_manager resources,
                   std::shared_ptr<forge::net::p2p::detail::connection_gate> gate)
    : state_(std::make_unique<state>()) {
   if (!gate) {
      gate = std::make_shared<forge::net::p2p::detail::connection_gate>(nullptr);
   }
   if (!options.private_network) {
      register_quic_profile(*this, runtime, options, resources, gate);
   }
   register_tcp_profile(*this, runtime, options, identity, std::move(resources), std::move(gate));
}

registry::~registry() = default;

void notify_tcp_transport_progress(const tcp_transport_progress_handler& handler) noexcept {
   if (!handler) {
      return;
   }
   try {
      handler();
   } catch (...) {
      // Progress is observational. A scheduler callback cannot take ownership
      // of, cancel, or strand a transport already committed to cleanup.
   }
}

bool owns_coordinated_source(const endpoint& listener, const endpoint& source) {
   if (listener.transport.protocol != source.transport.protocol || listener.transport.port != source.transport.port ||
       source.transport.port == 0) {
      return false;
   }
   auto error = boost::system::error_code{};
   const auto bound = boost::asio::ip::make_address(listener.transport.host, error);
   if (error) {
      return false;
   }
   const auto requested = boost::asio::ip::make_address(source.transport.host, error);
   return !error && !requested.is_unspecified() && !requested.is_multicast() && bound.is_v4() == requested.is_v4() &&
          (bound.is_unspecified() || bound == requested);
}

endpoint select_coordinated_source(std::span<const endpoint> listeners, const endpoint& remote,
                                   const std::optional<endpoint>& requested) {
   auto error = boost::system::error_code{};
   const auto remote_address = boost::asio::ip::make_address(remote.transport.host, error);
   if (error || remote_address.is_unspecified() || remote_address.is_multicast() || remote.transport.port == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated remote endpoint must be a concrete IP endpoint");
   }
   auto selected = std::optional<endpoint>{};
   for (const auto& listener : listeners) {
      auto source = requested.value_or(listener);
      if (source.transport.protocol != remote.transport.protocol || !owns_coordinated_source(listener, source)) {
         continue;
      }
      const auto address = boost::asio::ip::make_address(source.transport.host, error);
      if (error || address.is_v4() != remote_address.is_v4()) {
         continue;
      }
      if (selected) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated local source is ambiguous");
      }
      source.peer.reset();
      selected = std::move(source);
   }
   if (!selected) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "coordinated connect requires an active concrete listener source");
   }
   return std::move(*selected);
}

std::optional<endpoint> select_dial_source(boost::asio::io_context& context, resource_manager& resources,
                                          std::span<const endpoint> listeners, const endpoint& remote) {
   auto error = boost::system::error_code{};
   const auto destination = boost::asio::ip::make_address(remote.transport.host, error);
   // Node dialing expands DNS before entering a profile. Do not guess an IP
   // family or parse unresolved hostnames as interface addresses here.
   if (error || destination.is_unspecified() || destination.is_multicast() || remote.transport.port == 0) {
      return std::nullopt;
   }
   const auto compatible = [&](const endpoint& listener) {
      auto parse_error = boost::system::error_code{};
      const auto address = boost::asio::ip::make_address(listener.transport.host, parse_error);
      return !parse_error && listener.transport.protocol == remote.transport.protocol &&
             listener.transport.port != 0 && address.is_v4() == destination.is_v4();
   };
   if (std::ranges::none_of(listeners, compatible)) {
      return std::nullopt;
   }
   auto preferred = boost::asio::ip::address{};
   {
      // UDP connect sends no packet. getsockname reports the kernel's routed
      // source, including IPv6 scope. The temporary FD is admitted and closed
      // before dial admission; it is never the connection's transport owner.
      auto lifecycle = resources.reserve_lifecycle();
      if (!lifecycle) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P source lookup lifecycle limit reached");
      }
      auto descriptor = lifecycle->reserve_file_descriptors(1);
      if (!descriptor) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P source lookup descriptor limit reached");
      }
      auto probe = boost::asio::ip::udp::socket{context};
      probe.open(destination.is_v4() ? boost::asio::ip::udp::v4() : boost::asio::ip::udp::v6(), error);
      if (!error) {
         probe.connect({destination, remote.transport.port}, error);
      }
      if (!error) {
         preferred = probe.local_endpoint(error).address();
      }
      if (error || preferred.is_unspecified()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P ordinary dial source route is unavailable");
      }
   }
   // Pinned Go QUIC/TCP prefer the routed concrete listener, then wildcard.
   // TCP additionally permits a loopback listener for a loopback destination.
   // Choose the first endpoint in the profile snapshot within a priority, never an
   // unrelated interface, another registry's listener, or a guessed WAN IP.
   for (const auto priority : {0, 1, 2}) {
      for (const auto& listener : listeners) {
         if (!compatible(listener)) {
            continue;
         }
         const auto bound = boost::asio::ip::make_address(listener.transport.host);
         const auto matches = priority == 0 ? bound == preferred
                              : priority == 1 ? remote.is_direct_tcp() && destination.is_loopback() && bound.is_loopback()
                                              : bound.is_unspecified();
         if (matches) {
            auto source = listener;
            source.peer.reset();
            if (bound.is_unspecified()) {
               source.transport.host = preferred.to_string();
               source.transport.host_type = preferred.is_v4() ? endpoint::host_kind::ip4 : endpoint::host_kind::ip6;
            }
            return source;
         }
      }
   }
   return std::nullopt;
}

resource_manager::dial_reservation reserve_coordinated_dial(resource_manager& resources, const peer_id& peer) {
   auto admission = resources.reserve_dial(peer);
   if (!admission) {
      if (admission.outcome() == resource_manager::transition_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P coordinated dial limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P coordinated dial admission failed");
   }
   return std::move(*admission);
}

resource_manager::session_reservation reserve_coordinated_session(resource_manager& resources,
                                                                  resource_manager::session_direction direction) {
   auto admission = resources.reserve_session(direction);
   if (!admission) {
      if (admission.outcome() == resource_manager::transition_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P coordinated session limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P coordinated session admission failed");
   }
   return std::move(*admission);
}

void establish_coordinated_session(resource_manager::session_reservation& admission, const peer_id& expected,
                                   const peer_id& actual, resource_manager::session_direction direction,
                                   const authenticated_admission_handler& authenticated) {
   if (actual != expected) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "P2P coordinated peer does not match expected peer");
   }
   const auto transition = admission.establish(resource_manager::session_scope{.peer = actual, .direction = direction});
   if (transition != resource_manager::transition_result::accepted) {
      if (transition == resource_manager::transition_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P coordinated established session limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P coordinated session transition failed");
   }
   if (authenticated) {
      authenticated(actual);
   }
}

boost::asio::awaitable<void> async_discard_unpublished(connection& value) {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});

   value.session.request_cancel();
   auto transport = std::move(value.session);
   value.session = {};
   try {
      co_await transport.async_close();
   } catch (...) {
      // transport::session reports terminal failures only after lower cleanup.
   }
   transport = {};

   if (value.admission) {
      value.admission->release();
      value.admission.reset();
   }
   value.native_lifetime.reset();
}

bool registry::listening() const noexcept {
   return state_ && std::ranges::any_of(state_->profiles, [](const profile& value) { return value.listening(); });
}

std::optional<forge::net::p2p::endpoint> registry::local_endpoint() const {
   auto endpoints = local_endpoints();
   if (endpoints.empty()) {
      return std::nullopt;
   }
   return endpoints.front();
}

std::vector<forge::net::p2p::endpoint> registry::local_endpoints() const {
   auto out = std::vector<forge::net::p2p::endpoint>{};
   if (!state_) {
      return out;
   }
   for (const auto& value : state_->profiles) {
      auto endpoints = value.local_endpoints();
      out.insert(out.end(), std::make_move_iterator(endpoints.begin()), std::make_move_iterator(endpoints.end()));
   }
   return out;
}

void registry::add(profile value) {
   if (!value.supports || !value.listening || !value.local_endpoints || !value.listen || !value.stop ||
       !value.async_stop || !value.async_connect || !value.async_accept) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P direct transport profile is empty");
   }
   state_->profiles.push_back(std::move(value));
}

forge::net::p2p::endpoint registry::listen(forge::net::p2p::endpoint endpoint) {
   const auto requested = endpoint.to_string();
   const auto existing = local_endpoints();
   if (std::ranges::any_of(existing, [&](const auto& value) { return value.to_string() == requested; })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P direct listener endpoint is already active");
   }
   auto& selected = profile_for(state_->profiles, endpoint);
   return selected.listen(std::move(endpoint));
}

void registry::stop() noexcept {
   if (!state_) {
      return;
   }
   for (auto& value : state_->profiles) {
      try {
         value.stop();
      } catch (...) {
         // Teardown cancellation is best effort and must reach every transport.
      }
   }
}

forge::net::p2p::detail::session_teardown::operation registry::teardown_operation() const {
   auto close_profiles = state_ ? state_->profiles : std::vector<profile>{};
   auto cancel_profiles = close_profiles;
   return forge::net::p2p::detail::session_teardown::operation{
       .close = [profiles = std::move(close_profiles)]() mutable -> boost::asio::awaitable<void> {
          for (auto& value : profiles) {
             try {
                co_await value.async_stop();
             } catch (...) {
                // A failed backend must not bypass the remaining teardown operations.
             }
          }
       },
       .cancel =
           [profiles = std::move(cancel_profiles)]() mutable noexcept {
              for (auto& value : profiles) {
                 try {
                    value.stop();
                 } catch (...) {
                 }
              }
           },
   };
}

boost::asio::awaitable<connection> registry::async_connect(forge::net::p2p::endpoint endpoint,
                                                           const node::connect_options& options,
                                                           std::shared_ptr<cancellation_latch> cancellation,
                                                           std::shared_ptr<void> native_lifetime,
                                                           authenticated_admission_handler authenticated,
                                                           tcp_transport_progress_handler tcp_transport_progress,
                                                           native_socket_admission_handler socket_admission) {
   auto& selected = profile_for(state_->profiles, endpoint);
   if (selected.async_connect_admitted) {
      co_return co_await selected.async_connect_admitted(
          std::move(endpoint), options, std::move(cancellation), std::move(native_lifetime), std::move(authenticated),
          std::move(tcp_transport_progress), std::move(socket_admission));
   }
   if (socket_admission) {
      socket_admission(1);
   }
   co_return co_await selected.async_connect(std::move(endpoint), options, std::move(cancellation),
                                             std::move(native_lifetime), std::move(authenticated),
                                             std::move(tcp_transport_progress));
}

boost::asio::awaitable<connection> registry::async_accept(forge::net::p2p::endpoint endpoint) {
   auto& selected = profile_for(state_->profiles, endpoint);
   co_return co_await selected.async_accept(std::move(endpoint));
}

void registry::prepare_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) {
   auto& selected = profile_for(state_->profiles, owner->remote);
   if (!selected.prepare_coordinated || !selected.release_coordinated || !selected.async_connect_coordinated_owned) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "direct profile lacks coordinated generation ownership");
   }
   selected.prepare_coordinated(owner);
}

void registry::release_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) noexcept {
   for (auto& selected : state_->profiles) {
      if (selected.supports(owner->remote) && selected.release_coordinated) { selected.release_coordinated(owner); return; }
   }
}

boost::asio::awaitable<connection> registry::async_connect_coordinated(
    std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner) {
   auto& selected = profile_for(state_->profiles, owner->remote);
   if (!selected.async_connect_coordinated_owned) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "direct profile lacks coordinated generation ownership");
   }
   co_return co_await selected.async_connect_coordinated_owned(std::move(owner));
}

boost::asio::awaitable<connection> registry::async_connect_coordinated(endpoint remote, peer_id expected_peer,
                                                                       upgrade_role role,
                                                                       std::chrono::milliseconds budget,
                                                                       std::shared_ptr<cancellation_latch> cancellation,
                                                                       authenticated_admission_handler authenticated,
                                                                       std::optional<endpoint> local_source) {
   if (!valid_peer_id(expected_peer) || budget <= std::chrono::milliseconds::zero()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated connect requires a peer and positive budget");
   }
   if (remote.peer && *remote.peer != expected_peer) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
                            "coordinated endpoint peer does not match expected peer");
   }
   auto& selected = profile_for(state_->profiles, remote);
   if (!selected.async_connect_coordinated) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P direct profile does not support coordinated reuse");
   }
   co_return co_await selected.async_connect_coordinated(std::move(remote), std::move(expected_peer), role, budget,
                                                         std::move(cancellation), std::move(authenticated),
                                                         std::move(local_source));
}

} // namespace forge::net::p2p::direct
