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
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/scope/scope_exit.hpp>

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.tcp.connection;
import forge.net.tcp.connector;
import forge.net.tcp.exceptions;
import forge.net.tcp.listener;
import forge.net.tcp.options;
import forge.net.stcp.options;
import forge.net.transport.connector;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/direct_transport.hxx"
#include "details/coordinated_dial.hxx"
#include "details/cancellation_latch.hxx"
#include "details/connection_gate.hxx"
#include "details/libp2p_tls.hxx"
#include "details/operation_deadline.hxx"
#include "details/stream_upgrade.hxx"
#include "details/tcp_profile.hxx"

namespace forge::net::p2p::direct {
namespace {

[[nodiscard]] forge::net::p2p::endpoint p2p_endpoint_for(forge::net::transport::endpoint value) {
   return forge::net::p2p::endpoint{.transport = std::move(value)};
}

[[nodiscard]] std::string listener_key(forge::net::p2p::endpoint value) {
   value.peer.reset();
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.transport.host, error);
   if (!error) {
      value.transport.host = address.to_string();
   }
   return value.to_string();
}

[[nodiscard]] exceptions::code map_tcp_error(forge::net::tcp::exceptions::code kind) noexcept {
   using tcp_kind = forge::net::tcp::exceptions::code;
   switch (kind) {
   case tcp_kind::invalid_endpoint:
   case tcp_kind::invalid_options:
      return exceptions::code::invalid_options;
   case tcp_kind::canceled:
      return exceptions::code::canceled;
   case tcp_kind::closed:
      return exceptions::code::closed;
   case tcp_kind::connect_failed:
      return exceptions::code::peer_not_found;
   case tcp_kind::listen_failed:
   case tcp_kind::accept_failed:
   case tcp_kind::io_error:
      return exceptions::code::internal;
   }
   return exceptions::code::internal;
}

[[noreturn]] void rethrow_tcp_as_p2p(const forge::exceptions::base& error) {
   const auto code = forge::net::tcp::exceptions::code_of(error);
   if (code) {
      FORGE_THROW_CODE(map_tcp_error(*code), error.what());
   }
   throw;
}

[[nodiscard]] std::optional<peer_id> expected_peer_for(const forge::net::p2p::endpoint& endpoint,
                                                       const node::connect_options& options) {
   if (options.expected_peer) {
      return options.expected_peer;
   }
   return endpoint.peer;
}

} // namespace

std::shared_ptr<void> tcp_profile::track_native_terminal(std::shared_ptr<void> resources,
    std::function<boost::asio::awaitable<void>()>& wait) {
   const auto terminal = std::make_shared<forge::asio::notification>();
   const auto completed = std::make_shared<std::atomic_bool>(false);
   wait = [terminal, completed]() -> boost::asio::awaitable<void> {
      while (!completed->load(std::memory_order_acquire)) {
         const auto epoch = terminal->epoch();
         if (!completed->load(std::memory_order_acquire)) { co_await terminal->async_wait(epoch); }
      }
   };
   // TCP's terminal worker holds this token through close_on_owner, including
   // into_transport_stream. release_socket forwards it to STCP, whose worker
   // holds it through async_terminal_close. No profile/security wrapper owns
   // this control block: its release acknowledges the last native close, not
   // the destruction of a moved-from connection or secure wrapper.
   return std::shared_ptr<void>{terminal.get(),
       [resources = std::move(resources), terminal, completed](void*) mutable noexcept {
          resources.reset();
          completed->store(true, std::memory_order_release);
          terminal->notify();
       }};
}

tcp_profile::cancel_current_scope::~cancel_current_scope() {
   if (value) {
      static_cast<void>(value->finish());
   }
}

tcp_profile::tcp_profile(forge::asio::runtime& runtime_value, const node::options& options_value,
                         const libp2p_identity_material& identity_value, resource_manager resources_value,
                         std::shared_ptr<forge::net::p2p::detail::connection_gate> gate)
    : runtime_(runtime_value), options_(options_value), identity_(identity_value),
      resources_(std::move(resources_value)), gate_(std::move(gate)) {}

[[nodiscard]] bool tcp_profile::supports(const forge::net::p2p::endpoint& endpoint) const noexcept {
   return endpoint.is_direct_tcp();
}

[[nodiscard]] bool tcp_profile::listening() const noexcept {
   auto lock = std::scoped_lock{listeners_mutex_};
   return std::ranges::any_of(listeners_, [](const auto& item) { return item.second.active; });
}

[[nodiscard]] std::vector<forge::net::p2p::endpoint> tcp_profile::local_endpoints() const {
   auto out = std::vector<forge::net::p2p::endpoint>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   out.reserve(listeners_.size());
   for (const auto& [_, listener] : listeners_) {
      if (listener.active) {
         out.push_back(listener.local);
      }
   }
   return out;
}

forge::net::p2p::endpoint tcp_profile::listen(forge::net::p2p::endpoint endpoint) {
   if (!endpoint.is_direct_tcp()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P endpoint is not a direct TCP endpoint");
   }
   const auto requested_key = listener_key(endpoint);
   {
      auto lock = std::scoped_lock{listeners_mutex_};
      if (listeners_stopped_) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P TCP direct listener is stopped");
      }
      if (endpoint.transport.port != 0) {
         auto found = listeners_.find(requested_key);
         if (found != listeners_.end() && found->second.active) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P TCP direct listener endpoint is already active");
         }
      }
   }
   try {
      auto lifecycle = resources_.reserve_lifecycle();
      if (!lifecycle) {
         if (lifecycle.outcome() == resource_manager::transition_result::policy_rejected) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P TCP listener lifecycle limit reached");
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P TCP listener lifecycle resource admission failed");
      }
      auto descriptor = lifecycle->reserve_file_descriptors(1);
      if (!descriptor) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P TCP listener file descriptor limit reached");
      }
      auto native_lifetime = std::make_shared<resource_manager::file_descriptor_reservation>(std::move(*descriptor));
      auto listener = std::make_shared<forge::net::tcp::listener>(runtime_.context().get_executor(), endpoint.transport,
                                                                  forge::net::transport::listen_options{},
                                                                  forge::net::tcp::options{.reuse_port = true});
      auto local = p2p_endpoint_for(listener->local_endpoint());
      const auto key = listener_key(local);
      auto stopped = false;
      auto duplicate = false;
      {
         auto lock = std::scoped_lock{listeners_mutex_};
         stopped = listeners_stopped_;
         const auto found = listeners_.find(key);
         duplicate = found != listeners_.end() && found->second.active;
         if (!stopped && !duplicate) {
            listeners_.emplace(
                key,
                listener_entry{.value = listener, .native_lifetime = native_lifetime, .local = local, .active = true});
         }
      }
      if (stopped || duplicate) {
         try {
            listener->close();
         } catch (...) {
         }
      }
      if (stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P TCP direct listener is stopped");
      }
      if (duplicate) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P TCP direct listener endpoint is already active");
      }
      return local;
   } catch (const forge::exceptions::base& error) {
      rethrow_tcp_as_p2p(error);
   }
}

void tcp_profile::stop() {
   auto listeners = stop_listeners();
   for (const auto& listener : listeners) {
      try {
         listener->close();
      } catch (...) {
      }
   }
   auto active = std::vector<std::shared_ptr<cancellation_latch>>{};
   {
      auto lock = std::scoped_lock{active_mutex_};
      stopped_ = true;
      for (auto iterator = active_.begin(); iterator != active_.end();) {
         if (auto operation = iterator->lock()) {
            active.push_back(std::move(operation));
            ++iterator;
         } else {
            iterator = active_.erase(iterator);
         }
      }
   }
   for (const auto& operation : active) {
      operation->request_stop();
   }
}

boost::asio::awaitable<void> tcp_profile::async_stop() {
   stop();
   auto listeners = listener_snapshot();
   for (const auto& listener : listeners) {
      co_await listener->async_close();
   }
   auto lock = std::scoped_lock{listeners_mutex_};
   coordinated_.clear();
   listeners_.clear();
}

boost::asio::awaitable<connection>
tcp_profile::async_connect(forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                           std::shared_ptr<cancellation_latch> cancellation, std::shared_ptr<void> native_lifetime,
                           authenticated_admission_handler authenticated,
                           tcp_transport_progress_handler tcp_transport_progress, upgrade_role role,
                           std::shared_ptr<forge::net::tcp::listener> source,
                           std::optional<forge::net::p2p::endpoint> local_source, bool strict_identity,
                           native_socket_admission_handler socket_admission) {
   if (!endpoint.is_direct_tcp()) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P endpoint is not a direct TCP endpoint");
   }
   auto expected_peer = expected_peer_for(endpoint, options);
   auto upgrade_options = options_;
   if (strict_identity) {
      upgrade_options.allow_insecure_test_mode = false;
   }
   const auto started = std::chrono::steady_clock::now();
   const auto source_supplied = static_cast<bool>(source);
   if (!source_supplied) {
      local_source = select_dial_source(runtime_.context(), resources_, local_endpoints(), endpoint);
      if (local_source) {
         auto lock = std::scoped_lock{listeners_mutex_};
         for (const auto& [_, entry] : listeners_) {
            if (entry.active && owns_coordinated_source(entry.local, *local_source)) {
               source = entry.value;
               native_lifetime = std::make_shared<native_connection_lifetime>(native_connection_lifetime{
                   .source = source, .source_admission = entry.native_lifetime, .parent = std::move(native_lifetime)});
               break;
            }
         }
         if (!source || !source->valid()) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "ordinary TCP source listener stopped during selection");
         }
      }
   }
   if (cancellation && cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P TCP direct connect canceled before socket admission");
   }
   if (socket_admission) {
      socket_admission(1);
   }
   const auto remaining = options.timeout - std::chrono::duration_cast<std::chrono::milliseconds>(
                                               std::chrono::steady_clock::now() - started);
   if (remaining <= std::chrono::milliseconds::zero()) {
      throw_operation_timeout("P2P TCP direct source selection");
   }
   auto remote_transport = endpoint.transport;
   // Explicit sources require exact reuse; an ordinary auto-selected source is preferred.
   auto connector =
       source ? (source_supplied ? source->make_coordinated_connector(local_source->transport)
                                 : source->make_connector(local_source->transport,
                                                          forge::net::tcp::connector::reuse_policy::preferred))
              : forge::net::tcp::connector{runtime_.context().get_executor()};
   auto cancel_current = std::make_shared<cancellation_latch>();
   auto parent_subscription =
       cancellation_latch::subscribe(cancellation, [cancel_current] noexcept { cancel_current->request_stop(); });
   track(cancel_current);
   cancel_current->arm([&connector] noexcept { connector.request_cancel(); });
   auto deadline = operation_deadline{runtime_.context(), remaining};
   auto cancel_scope = cancel_current_scope{cancel_current};
   deadline.arm([cancel_current] noexcept { cancel_current->request_stop(); });
   auto tcp = std::shared_ptr<forge::net::tcp::connection>{};
   auto upgraded = upgraded_session{};
   auto failure = std::exception_ptr{};
   auto wait_terminal = std::function<boost::asio::awaitable<void>()>{};
   try {
      tcp = std::make_shared<forge::net::tcp::connection>(
          co_await connector.async_connect_connection(std::move(remote_transport), {},
              track_native_terminal(native_lifetime, wait_terminal)));
      cancel_current->arm([tcp] noexcept { tcp->request_cancel(); });
      const auto local_endpoint = p2p_endpoint_for(tcp->local_endpoint());
      const auto remote_endpoint = p2p_endpoint_for(tcp->remote_endpoint());
      // The socket is connected, but no PNET/security/muxer work has begun.
      // The profile's active-operation mutex is not held across this callback.
      notify_tcp_transport_progress(tcp_transport_progress);
      cancel_current->clear();
      upgraded = co_await upgrade_tcp(
          std::move(*tcp), upgrade_options, identity_, std::move(expected_peer), role,
          tcp_upgrade_deadline{
              .context = &runtime_.context(), .timeout = remaining, .cancel_current = cancel_current},
          upgrade_callbacks{
              .secured =
                  [gate = gate_, local_endpoint, remote_endpoint](const peer_id& peer) {
                     gate->secured(connection_direction::outbound, peer, local_endpoint, remote_endpoint);
                  },
              .established = std::move(authenticated),
              .upgraded =
                  [gate = gate_, local_endpoint, remote_endpoint](const peer_id& peer) {
                     gate->upgraded(connection_direction::outbound, peer, local_endpoint, remote_endpoint);
                  },
          });
      if (source) {
         // Join the borrowed connector without closing its source listener.
         co_await connector.async_stop();
      }
      const auto deadline_completed = deadline.finish();
      const auto operation_completed = cancel_current->finish();
      if (!deadline_completed || !operation_completed) {
         upgraded.session->cancel();
      }
      if (!deadline_completed) {
         throw_operation_timeout("P2P TCP direct connect");
      }
      if (!operation_completed) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P TCP direct connect canceled");
      }
      co_return connection{
          .peer = std::move(upgraded.peer),
          .session = std::move(*upgraded.session).as_transport(),
          .local_endpoint = std::move(local_endpoint),
          .remote_endpoint = std::move(remote_endpoint),
          .native_lifetime = std::move(native_lifetime),
          .authentication = upgraded.authentication,
          .muxer = std::move(upgraded.muxer),
          .used_early_muxer_negotiation = upgraded.used_early_muxer_negotiation,
          .role = upgraded.role,
          .security_role = upgraded.security_role,
          .yamux_role = upgraded.yamux_role,
      };
   } catch (...) {
      failure = std::current_exception();
   }
   const auto timed_out = deadline.timed_out();
   const auto canceled = cancel_current->stop_requested();
   cancel_current->request_stop();
   static_cast<void>(cancel_current->finish());
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   if (upgraded.session) {
      upgraded.session->cancel();
      try {
         co_await upgraded.session->async_close();
      } catch (...) {
      }
   } else if (tcp) {
      tcp->request_cancel();
      try {
         co_await tcp->async_close();
      } catch (...) {
      }
   }
   try {
      co_await connector.async_stop();
   } catch (...) {
   }
   // Release any untransferred connection reference only after its close. The
   // native worker still owns the receipt until a TLS/Noise handoff closes too.
   tcp.reset();
   upgraded.session.reset();
   if (wait_terminal) { co_await wait_terminal(); }
   if (timed_out) {
      throw_operation_timeout("P2P TCP direct connect");
   }
   if (canceled) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P TCP direct connect canceled");
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      rethrow_tcp_as_p2p(error);
   }
}

boost::asio::awaitable<connection> tcp_profile::async_connect_coordinated(
    endpoint remote, peer_id expected_peer, upgrade_role role, std::chrono::milliseconds budget,
    std::shared_ptr<cancellation_latch> cancellation, authenticated_admission_handler authenticated,
    std::optional<endpoint> requested, std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner) {
   try {
      if (role != upgrade_role::initiator && role != upgrade_role::responder) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid coordinated TCP upgrade role");
      }
      gate_->peer_dial(expected_peer);
      gate_->address_dial(expected_peer, remote);
      const auto local = select_coordinated_source(local_endpoints(), remote, requested);
      auto source = listener_entry{};
      auto source_key = std::string{};
      {
         auto lock = std::scoped_lock{listeners_mutex_};
         for (const auto& [key, entry] : listeners_) {
            if (entry.active && owns_coordinated_source(entry.local, local)) {
               source = entry;
               source_key = key;
               break;
            }
         }
      }
      if (!source.value || !source.value->valid()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated TCP source listener is not active");
      }
      if (owner && (owner->source.get() != source.value.get() || owner->source_key != source_key)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated TCP listener generation changed");
      }
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "coordinated TCP canceled before admission");
      }
      auto dial = resource_manager::dial_reservation{};
      if (!owner) { dial = reserve_coordinated_dial(resources_, expected_peer); }
      auto admission = std::make_shared<resource_manager::session_reservation>(
          reserve_coordinated_session(resources_, resource_manager::session_direction::outbound));
      auto descriptor = admission->reserve_file_descriptors(1);
      if (!descriptor) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P coordinated TCP descriptor limit reached");
      }
      // Retained by the native stream worker on failure, not just this coroutine.
      auto lifetime = std::make_shared<native_connection_lifetime>(native_connection_lifetime{
          .source = source.value,
          .source_admission = source.native_lifetime,
          .admission = admission,
          .descriptor = std::move(*descriptor),
      });
      // A failed outgoing socket does not end the coordinated batch. Preserve
      // its agreed role for a later accepted socket until batch stop/expiry.
      auto batch_cancel = cancellation ? std::move(cancellation) : std::make_shared<cancellation_latch>();
      track(batch_cancel);
      if (!owner) {
         auto ticket = std::make_shared<coordinated_tcp_upgrade>(coordinated_tcp_upgrade{
             .expected_peer = expected_peer,
             .local_source = local,
             .role = role,
             .expires = std::chrono::steady_clock::now() + budget,
             .cancellation = batch_cancel,
         });
         auto lock = std::scoped_lock{listeners_mutex_};
         const auto now = std::chrono::steady_clock::now();
         std::erase_if(coordinated_, [now](const auto& entry) {
            return !entry.second->owner &&
                (entry.second->expires <= now || entry.second->cancellation->stop_requested());
         });
         const auto key = std::make_pair(source_key, listener_key(remote));
         if (coordinated_.contains(key)) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated TCP attempt is already active");
         }
         if (coordinated_.size() >= max_coordinated_candidates) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "coordinated TCP candidate limit reached");
         }
         coordinated_[key] = ticket;
      }
      auto result = co_await async_connect(
          std::move(remote), node::connect_options{.expected_peer = expected_peer, .timeout = budget}, batch_cancel,
          lifetime,
          [admission, expected_peer, authenticated = std::move(authenticated)](const peer_id& actual) {
             establish_coordinated_session(*admission, expected_peer, actual,
                                           resource_manager::session_direction::outbound, authenticated);
          },
          {}, role, source.value, local, true);
      result.admission = std::move(*admission);
      result.native_lifetime = std::move(lifetime);
      result.coordinated_owner = std::move(owner);
      if (result.authentication == peer_authentication::unverified) {
         co_await async_discard_unpublished(result);
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "coordinated TCP requires authenticated identity");
      }
      co_return result;
   } catch (const forge::exceptions::base& error) {
      rethrow_tcp_as_p2p(error);
   }
}

void tcp_profile::prepare_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) {
   const auto local = select_coordinated_source(local_endpoints(), owner->remote, owner->options.local_source);
   auto source = listener_entry{};
   auto source_key = std::string{};
   {
      const auto lock = std::scoped_lock{listeners_mutex_};
      for (const auto& [key, entry] : listeners_) {
         if (entry.active && owns_coordinated_source(entry.local, local)) { source = entry; source_key = key; break; }
      }
   }
   if (!source.value || !source.value->valid() || !listener_is_current(source_key, source.value)) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated TCP source is not an active listener");
   }
   auto permit = reserve_coordinated_dial(resources_, owner->options.expected_peer);
   auto ticket = std::make_shared<coordinated_tcp_upgrade>(coordinated_tcp_upgrade{
       .expected_peer = owner->options.expected_peer, .local_source = local, .role = owner->role,
       .expires = owner->deadline, .cancellation = owner->transports, .owner = owner});
   const auto key = std::make_pair(source_key, listener_key(owner->remote));
   auto stored_key = source_key;
   {
      const auto lock = std::scoped_lock{listeners_mutex_};
      const auto current = listeners_.find(source_key);
      if (current == listeners_.end() || !current->second.active || current->second.value != source.value) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "coordinated TCP listener generation changed before lease");
      }
      const auto now = std::chrono::steady_clock::now();
      std::erase_if(coordinated_, [now](const auto& entry) {
         return !entry.second->owner &&
             (entry.second->expires <= now || entry.second->cancellation->stop_requested());
      });
      if (coordinated_.contains(key)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated TCP tuple is already leased");
      }
      if (coordinated_.size() >= max_coordinated_candidates) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "coordinated TCP leases are at capacity");
      }
      owner->source = source.value;
      owner->source_admission = source.native_lifetime;
      owner->source_key = std::move(stored_key);
      owner->permit = std::move(permit);
      coordinated_.emplace(key, std::move(ticket));
   }
}

void tcp_profile::release_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) noexcept {
   const auto lock = std::scoped_lock{listeners_mutex_};
   std::erase_if(coordinated_, [&owner](const auto& entry) { return entry.second->owner == owner; });
}

boost::asio::awaitable<connection> tcp_profile::async_accept(forge::net::p2p::endpoint endpoint) {
   const auto key = listener_key(std::move(endpoint));
   auto listener = std::shared_ptr<forge::net::tcp::listener>{};
   {
      auto lock = std::scoped_lock{listeners_mutex_};
      const auto found = listeners_.find(key);
      if (found != listeners_.end() && found->second.active) {
         listener = found->second.value;
      }
   }
   if (!listener || !listener->valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P TCP direct listener is not active");
   }
   try {
      // The terminal worker retains this holder across TCP/STCP handoff, but
      // it starts empty so an idle accept does not consume session or FD
      // budget before the kernel has produced a connection.
      auto native_lifetime = std::make_shared<native_connection_lifetime>();
      auto wait_terminal = std::function<boost::asio::awaitable<void>()>{};
      auto tcp =
          std::make_shared<forge::net::tcp::connection>(co_await listener->async_accept_connection(
              track_native_terminal(native_lifetime, wait_terminal)));
      auto received_owner = std::shared_ptr<forge::net::p2p::detail::coordinated_dial>{};
      auto transferred = false;
      auto native_terminal = boost::scope::scope_exit{[&] {
         if (received_owner && !transferred) { received_owner->end_inbound(); }
      }};
      auto failure = std::exception_ptr{};
      auto upgraded = upgraded_session{};
      try {
         if (!listener_is_current(key, listener)) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P TCP direct listener stopped during accept");
         }
         const auto local_endpoint = p2p_endpoint_for(tcp->local_endpoint());
         const auto remote_endpoint = p2p_endpoint_for(tcp->remote_endpoint());
         auto coordinated = std::shared_ptr<coordinated_tcp_upgrade>{};
         {
            auto lock = std::scoped_lock{listeners_mutex_};
            const auto found = coordinated_.find(std::make_pair(key, listener_key(remote_endpoint)));
            if (found != coordinated_.end()) {
               coordinated = found->second;
            }
         }
         if (coordinated &&
             (coordinated->expires <= std::chrono::steady_clock::now() || coordinated->cancellation->stop_requested() ||
              listener_key(coordinated->local_source) != listener_key(local_endpoint))) {
            if (coordinated->owner) {
               FORGE_THROW_EXCEPTION(exceptions::canceled, "coordinated TCP inbound lease stopped");
            }
            coordinated.reset();
         }
         if (coordinated && coordinated->owner) {
            if (!coordinated->owner->begin_inbound()) {
               FORGE_THROW_EXCEPTION(exceptions::canceled, "coordinated TCP inbound admission closed");
            }
            received_owner = coordinated->owner;
         }
         const auto upgrade_timeout =
             coordinated
                 ? std::max(std::chrono::milliseconds{1}, std::chrono::duration_cast<std::chrono::milliseconds>(
                                                              coordinated->expires - std::chrono::steady_clock::now()))
                 : node::connect_options{}.timeout;
         gate_->accept(local_endpoint, remote_endpoint);
         auto reserved = resources_.reserve_session(resource_manager::session_direction::inbound);
         if (!reserved) {
            if (reserved.outcome() == resource_manager::transition_result::policy_rejected) {
               FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P inbound session limit reached");
            }
            FORGE_THROW_EXCEPTION(exceptions::internal, "P2P inbound TCP session resource admission failed");
         }
         auto admission = std::make_shared<resource_manager::session_reservation>(std::move(*reserved));
         native_lifetime->admission = admission;
         auto descriptor = admission->reserve_file_descriptors(1);
         if (!descriptor) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P inbound TCP file descriptor limit reached");
         }
         native_lifetime->descriptor.emplace(std::move(*descriptor));
         auto cancel_current = std::make_shared<cancellation_latch>();
         auto subscription =
             cancellation_latch::subscribe(coordinated ? coordinated->cancellation : nullptr,
                                           [cancel_current] noexcept { cancel_current->request_stop(); });
         track(cancel_current);
         cancel_current->arm([tcp] noexcept { tcp->request_cancel(); });
         auto deadline = operation_deadline{runtime_.context(), upgrade_timeout};
         auto cancel_scope = cancel_current_scope{cancel_current};
         deadline.arm([cancel_current] noexcept { cancel_current->request_stop(); });
         auto upgrade_options = options_;
         if (coordinated) {
            upgrade_options.allow_insecure_test_mode = false;
         }
         try {
            cancel_current->clear();
            upgraded = co_await upgrade_tcp(
                std::move(*tcp), upgrade_options, identity_,
                coordinated ? std::optional<peer_id>{coordinated->expected_peer} : std::nullopt,
                coordinated ? coordinated->role : upgrade_role::responder,
                tcp_upgrade_deadline{
                    .context = &runtime_.context(), .timeout = upgrade_timeout, .cancel_current = cancel_current},
                upgrade_callbacks{
                    .secured =
                        [gate = gate_, local_endpoint, remote_endpoint](const peer_id& peer) {
                           gate->secured(connection_direction::inbound, peer, local_endpoint, remote_endpoint);
                        },
                    .established =
                        [&admission](const peer_id& peer) {
                           const auto transition = admission->establish(resource_manager::session_scope{
                               .peer = peer,
                               .direction = resource_manager::session_direction::inbound,
                           });
                           if (transition != resource_manager::transition_result::accepted) {
                              if (transition != resource_manager::transition_result::policy_rejected) {
                                 FORGE_THROW_EXCEPTION(exceptions::internal,
                                                       "P2P inbound TCP session resource transition failed");
                              }
                              FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                                    "P2P established inbound session limit reached");
                           }
                        },
                    .upgraded =
                        [gate = gate_, local_endpoint, remote_endpoint](const peer_id& peer) {
                           gate->upgraded(connection_direction::inbound, peer, local_endpoint, remote_endpoint);
                        },
                });
            const auto deadline_completed = deadline.finish();
            const auto operation_completed = cancel_current->finish();
            if (!deadline_completed || !operation_completed) {
               upgraded.session->cancel();
               co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
               try {
                  co_await upgraded.session->async_close();
               } catch (...) {
               }
            }
            if (!deadline_completed) {
               throw_operation_timeout("P2P TCP direct accept");
            }
            if (!operation_completed) {
               FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P TCP direct accept canceled");
            }
         } catch (const forge::exceptions::base&) {
            if (deadline.timed_out()) {
               throw_operation_timeout("P2P TCP direct accept");
            }
            if (!cancel_current->finish()) {
               FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P TCP direct accept canceled");
            }
            throw;
         }
         auto result = connection{
             .peer = std::move(upgraded.peer),
             .session = std::move(*upgraded.session).as_transport(),
             .local_endpoint = std::move(local_endpoint),
             .remote_endpoint = std::move(remote_endpoint),
             .admission = std::move(*admission),
             .native_lifetime = std::move(native_lifetime),
             .authentication = upgraded.authentication,
             .muxer = std::move(upgraded.muxer),
             .used_early_muxer_negotiation = upgraded.used_early_muxer_negotiation,
             .role = upgraded.role,
             .security_role = upgraded.security_role,
             .yamux_role = upgraded.yamux_role,
             .coordinated_owner = received_owner,
             .coordinated_inbound_worker = static_cast<bool>(received_owner),
         };
         transferred = true;
         co_return std::move(result);
      } catch (...) {
         failure = std::current_exception();
      }
      tcp->request_cancel();
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      if (upgraded.session) {
         upgraded.session->cancel();
         try { co_await upgraded.session->async_close(); } catch (...) {}
      } else {
         try { co_await tcp->async_close(); } catch (...) {}
      }
      tcp.reset();
      upgraded.session.reset();
      co_await wait_terminal();
      std::rethrow_exception(failure);
   } catch (const forge::exceptions::base& error) {
      rethrow_tcp_as_p2p(error);
   }
}

[[nodiscard]] bool tcp_profile::listener_is_current(const std::string& key,
                                                    const std::shared_ptr<forge::net::tcp::listener>& listener) const {
   auto lock = std::scoped_lock{listeners_mutex_};
   const auto found = listeners_.find(key);
   return !listeners_stopped_ && found != listeners_.end() && found->second.active && found->second.value == listener;
}

[[nodiscard]] std::vector<std::shared_ptr<forge::net::tcp::listener>> tcp_profile::stop_listeners() {
   auto listeners = std::vector<std::shared_ptr<forge::net::tcp::listener>>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   listeners_stopped_ = true;
   listeners.reserve(listeners_.size());
   for (auto& [_, listener] : listeners_) {
      listener.active = false;
      listeners.push_back(listener.value);
   }
   return listeners;
}

[[nodiscard]] std::vector<std::shared_ptr<forge::net::tcp::listener>> tcp_profile::listener_snapshot() const {
   auto listeners = std::vector<std::shared_ptr<forge::net::tcp::listener>>{};
   auto lock = std::scoped_lock{listeners_mutex_};
   listeners.reserve(listeners_.size());
   for (const auto& [_, listener] : listeners_) {
      listeners.push_back(listener.value);
   }
   return listeners;
}

void tcp_profile::track(const std::shared_ptr<cancellation_latch>& operation) {
   auto cancel_now = false;
   {
      auto lock = std::scoped_lock{active_mutex_};
      cancel_now = stopped_;
      if (!cancel_now) {
         active_.erase(
             std::remove_if(active_.begin(), active_.end(), [](const auto& operation) { return operation.expired(); }),
             active_.end());
         active_.push_back(operation);
      }
   }
   if (cancel_now) {
      operation->request_stop();
   }
}

} // namespace forge::net::p2p::direct
