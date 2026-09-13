module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
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
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.dns.resolver;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/cancellation_latch.hxx"
#include "details/autonat_v2_dialback.hxx"
#include "details/node_impl.hxx"
#include "details/node_impl_autonat_operation.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p {
namespace asio = boost::asio;

namespace {

// Rust sends 4096-byte data fields plus protobuf framing; Go sends 4000-byte fields.
constexpr auto autonat_message_limit = std::size_t{4104};
constexpr auto autonat_address_limit = std::size_t{16};
constexpr auto autonat_data_limit = std::size_t{100'000};

reachability::options autonat_codec_options() {
   auto result = reachability::options{};
   result.max_message_size = autonat_message_limit;
   result.max_endpoints = autonat_address_limit;
   result.max_data_response_size = 4096;
   return result;
}

bool same_ip(const endpoint& left, const endpoint& right) {
   auto left_error = boost::system::error_code{};
   auto right_error = boost::system::error_code{};
   const auto a = asio::ip::make_address(left.transport.host, left_error);
   const auto b = asio::ip::make_address(right.transport.host, right_error);
   return !left_error && !right_error && a == b;
}

bool public_probe_endpoint(const endpoint& value, const peer_id& peer,
                            const std::vector<endpoint>& local_endpoints) {
   auto error = boost::system::error_code{};
   const auto address = asio::ip::make_address(value.transport.host, error);
   if (error) {
      return false;
   }
   if (address.is_v6()) {
      const auto bytes = address.to_v6().to_bytes();
      // A public-looking translation/tunnel prefix must not expose an embedded
      // private IPv4 destination to a remotely requested dial.
      if ((bytes[0] & 0xe0U) != 0x20U || (bytes[0] == 0x20U && bytes[1] == 0x02U) ||
          (bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0 && bytes[3] == 0)) {
         return false;
      }
   }
   return (value.is_direct_tcp() || value.is_direct_quic()) && value.transport.port != 0 &&
          (!value.peer || *value.peer == peer) &&
          host_addresses::classify_endpoint_scope(value) == host_addresses::endpoint_scope::public_address &&
          std::ranges::none_of(local_endpoints, [&](const endpoint& local) { return same_ip(local, value); });
}

} // namespace

// A nonce belongs to an outstanding probe, not to the server's dialer identity.
// Go uses a separate dialer host; Rust correlates an independent inbound connection.
void node::impl::remember_autonat_v2_nonce(const peer_id& peer, std::uint64_t nonce) {
   const auto now = std::chrono::steady_clock::now();
   auto lock = std::scoped_lock{mutex};
   std::erase_if(pending_autonat_v2_nonces, [now](const auto& item) { return item.second.expires_at <= now; });
   if (stopped || session_admission_closed) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "AutoNAT probe started during node shutdown");
   }
   if (nonce == 0 || std::ranges::any_of(pending_autonat_v2_nonces,
                                        [nonce](const auto& item) { return item.second.value == nonce; })) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT nonce must be nonzero and unique");
   }
   if (pending_autonat_v2_nonces.contains(peer) ||
       pending_autonat_v2_nonces.size() >= options.reachability_policy.max_pending_probes) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "AutoNAT pending probe limit reached");
   }
   pending_autonat_v2_nonces.emplace(peer, autonat_nonce{.value = nonce,
       .expires_at = now + options.reachability_policy.timeout,
       .changed = std::make_shared<forge::asio::notification>()});
}

void node::impl::forget_autonat_v2_nonce(const peer_id& peer, std::uint64_t nonce) {
   auto changed = std::shared_ptr<forge::asio::notification>{};
   {
      auto lock = std::scoped_lock{mutex};
      const auto found = pending_autonat_v2_nonces.find(peer);
      if (found != pending_autonat_v2_nonces.end() && found->second.value == nonce) {
         changed = found->second.changed;
         pending_autonat_v2_nonces.erase(found);
      }
   }
   if (changed) {
      changed->notify();
   }
}

bool node::impl::consume_autonat_v2_nonce(std::uint64_t nonce, const endpoint& local_endpoint) {
   auto changed = std::shared_ptr<forge::asio::notification>{};
   {
      auto lock = std::scoped_lock{mutex};
      if (nonce == 0 || stopped || session_admission_closed) {
         return false;
      }
      const auto now = std::chrono::steady_clock::now();
      for (auto& [_, pending] : pending_autonat_v2_nonces) {
         if (pending.value == nonce && !pending.observed && now < pending.expires_at) {
            pending.observed = local_endpoint;
            changed = pending.changed;
            break;
         }
      }
   }
   if (changed) {
      changed->notify();
   }
   return static_cast<bool>(changed);
}

std::optional<endpoint> node::impl::autonat_v2_observation(const peer_id& observer, std::uint64_t nonce) const {
   auto lock = std::scoped_lock{mutex};
   const auto found = pending_autonat_v2_nonces.find(observer);
   if (found == pending_autonat_v2_nonces.end() || found->second.value != nonce ||
       std::chrono::steady_clock::now() >= found->second.expires_at) {
      return std::nullopt;
   }
   return found->second.observed;
}

boost::asio::awaitable<std::optional<endpoint>> node::impl::async_autonat_v2_observation(
    peer_id observer, std::uint64_t nonce, std::shared_ptr<cancellation_latch> cancellation) {
   const auto executor = co_await asio::this_coro::executor;
   // Isolate the cancellation filter from the caller's coroutine state.
   co_return co_await asio::co_spawn(
       executor, wait_autonat_v2_observation(shared_from_this(), std::move(observer), nonce, std::move(cancellation)),
       asio::bind_executor(executor, asio::use_awaitable));
}

boost::asio::awaitable<std::optional<endpoint>> node::impl::wait_autonat_v2_observation(
    std::shared_ptr<impl> self, peer_id observer, std::uint64_t nonce,
    std::shared_ptr<cancellation_latch> cancellation) {
   auto stop = std::stop_source{};
   auto subscription = cancellation_latch::subscribe(cancellation, [stop]() mutable noexcept { stop.request_stop(); });
   const auto inherited = (co_await asio::this_coro::cancellation_state).cancelled();
   co_await asio::this_coro::reset_cancellation_state(
       [stop](asio::cancellation_type type) mutable noexcept {
          if (type != asio::cancellation_type::none) {
             stop.request_stop();
          }
          return type;
       }, asio::enable_total_cancellation{});
   if (inherited != asio::cancellation_type::none) {
      stop.request_stop();
   }
   auto tracked = self->lifecycle.track();
   auto changed = std::shared_ptr<forge::asio::notification>{};
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   {
      auto lock = std::scoped_lock{self->mutex};
      const auto found = self->pending_autonat_v2_nonces.find(observer);
      if (found == self->pending_autonat_v2_nonces.end() || found->second.value != nonce || !tracked.active()) {
         co_return std::nullopt;
      }
      changed = found->second.changed;
      deadline = std::min(deadline, found->second.expires_at);
   }
   auto ticket = self->teardown.track([changed] noexcept { changed->notify(); });
   if (!ticket.active()) {
      co_return std::nullopt;
   }
   while (true) {
      if (stop.stop_requested()) {
         throw boost::system::system_error{asio::error::operation_aborted};
      }
      const auto epoch = changed->epoch();
      {
         auto lock = std::scoped_lock{self->mutex};
         const auto found = self->pending_autonat_v2_nonces.find(observer);
         if (self->stopped || self->session_admission_closed || found == self->pending_autonat_v2_nonces.end() ||
             found->second.value != nonce || found->second.changed != changed ||
             std::chrono::steady_clock::now() >= deadline) {
            co_return std::nullopt;
         }
         if (found->second.observed) {
            co_return found->second.observed;
         }
      }
      static_cast<void>(co_await changed->async_wait_until(epoch, deadline, stop.get_token()));
   }
}


boost::asio::awaitable<void> node::impl::handle_autonat_v1(std::shared_ptr<session_state> session,
                                                          forge::net::p2p::stream stream) {
   return handle_autonat(std::move(session), std::move(stream), autonat_protocol::v1);
}

boost::asio::awaitable<void> node::impl::handle_autonat_v2_dial_request(std::shared_ptr<session_state> session,
                                                                      forge::net::p2p::stream stream) {
   return handle_autonat(std::move(session), std::move(stream), autonat_protocol::v2_request);
}

boost::asio::awaitable<void> node::impl::handle_autonat_v2_dial_back(std::shared_ptr<session_state> session,
                                                                   forge::net::p2p::stream stream) {
   return handle_autonat(std::move(session), std::move(stream), autonat_protocol::v2_dial_back);
}

boost::asio::awaitable<void> node::impl::handle_autonat(std::shared_ptr<session_state> session,
                                                       forge::net::p2p::stream stream, autonat_protocol protocol) {
   auto self = shared_from_this();
   auto operation = std::make_shared<autonat_operation>();
   operation->owner = self;
   operation->caller = std::move(session);
   operation->request = std::move(stream);
   operation->protocol = protocol;
   operation->service = protocol != autonat_protocol::v2_dial_back;
   operation->deadline = std::chrono::steady_clock::now() + options.reachability_policy.timeout;
   auto tracked = lifecycle.track();
   auto ticket = detail::session_teardown::ticket{};
   auto deadline = std::optional<operation_deadline>{};
   try {
      if (!tracked.active() || (private_network_enabled() && options.private_network->internet_egress !=
          private_network::internet_egress_policy::allow_internet)) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "AutoNAT unavailable on this node");
      }
      const auto stop = operation->stop;
      ticket = teardown.track([stop] noexcept { stop->request_stop(); });
      if (!ticket.active()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "AutoNAT stopped before admission");
      }
      if ((protocol == autonat_protocol::v1 && !options.reachability_policy.service_v1_enabled) ||
          (protocol == autonat_protocol::v2_request && !options.reachability_policy.service_v2_enabled)) {
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "AutoNAT service is disabled");
      }
      {
         auto lock = std::scoped_lock{mutex};
         const auto& caller = *operation->caller;
         if (stopped || session_admission_closed || caller.closed ||
             caller.authentication == peer_authentication::unverified || caller.info.path != path::kind::direct) {
            FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "AutoNAT requires an authenticated direct caller");
         }
         operation->peer = caller.info.remote_peer;
         operation->remote_endpoint = caller.remote_endpoint;
         if (caller.direction == connection_manager::direction::inbound) {
            operation->inbound_endpoint = caller.local_endpoint;
         }
         operation->local_endpoints = local_endpoints_for_control_locked();
         const auto now = std::chrono::steady_clock::now();
         std::erase_if(autonat_service_requests, [&](const auto& item) {
            return now - item.second >= options.reachability_policy.service_rate_interval;
         });
         const auto peer_requests = std::ranges::count_if(autonat_service_requests,
             [&](const auto& item) { return item.first == operation->peer; });
         const auto quota_exceeded =
             autonat_handlers_active >= options.reachability_policy.max_pending_probes ||
             autonat_service_requests.size() >= options.reachability_policy.service_global_limit ||
             static_cast<std::size_t>(peer_requests) >= options.reachability_policy.service_per_peer_limit ||
             (operation->service && autonat_service_active.contains(operation->peer));
         // A bounded, already-admitted request receives the native refusal
         // instead of a reset. It does not join the service-worker quota.
         operation->memory = operation->caller->resource.reserve_memory(
             quota_exceeded ? autonat_message_limit : 4 * autonat_message_limit);
         if (!operation->memory) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "AutoNAT message memory limit reached");
         }
         if (quota_exceeded) {
            if (!operation->service) {
               FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected,
                                     "AutoNAT dial-back concurrency limit reached");
            }
            operation->quota_rejected = true;
         } else {
            autonat_service_requests.emplace_back(operation->peer, now);
            if (operation->service) {
               autonat_service_active.insert(operation->peer);
            }
            ++autonat_handlers_active;
            operation->admitted = true;
         }
      }
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          operation->deadline - std::chrono::steady_clock::now());
      deadline.emplace(runtime.context(), remaining);
      deadline->arm([stop] noexcept { stop->request_stop(); });
      co_await detail::async_run_with_stop_bridge(
          stop, [self, operation](std::shared_ptr<detail::worker_terminal_owner> terminal) {
             return self->run_autonat(operation, std::move(terminal));
          }, {.lifecycle_stop = tracked.stop_source()});
   } catch (...) {
      if (!operation->failure) {
         operation->failure = std::current_exception();
      }
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   if (deadline) {
      static_cast<void>(deadline->finish());
   }
   // Also covers admission or worker-spawn failure before run_autonat owns cleanup.
   if (!operation->completed) {
      operation->request.request_cancel();
      try { co_await operation->request.async_close(); } catch (...) {}
   }
   if (!operation->completed && !operation->failure) {
      try {
         operation->check();
         FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT stopped before service work");
      } catch (...) {
         operation->failure = std::current_exception();
      }
   }
   const auto failure = operation->failure;
   operation.reset();
   ticket.release();
   tracked.release();
   if (failure) {
      std::rethrow_exception(failure);
   }
}

boost::asio::awaitable<void> node::impl::run_autonat(std::shared_ptr<autonat_operation> operation,
                                                     std::shared_ptr<detail::worker_terminal_owner> terminal) {
   static_cast<void>(terminal->publish(detail::worker_terminal_owner::callback{
       [operation] noexcept { operation->cancel(); }}));
   try {
      operation->check();
      if (operation->quota_rejected) {
         if (operation->protocol == autonat_protocol::v1) {
            auto buffer = std::vector<std::uint8_t>{};
            static_cast<void>(reachability::codec::decode_v1(
                co_await async_read_length_delimited(operation->request, buffer, autonat_message_limit),
                autonat_codec_options()));
            co_await operation->request.async_write(reachability::codec::encode_v1(
                {.kind = reachability::message::message_kind::dial_response,
                 .response = reachability::dial_response{.status = reachability::dial_status::dial_refused}}));
         } else {
            co_await operation->request.async_write(reachability::codec::encode_v2(
                {.type = reachability::v2::message::kind::dial_response,
                 .dial_response = reachability::v2::dial_response{
                     .status = reachability::v2::response_status::request_rejected}},
                autonat_codec_options()));
         }
      } else if (operation->protocol == autonat_protocol::v1) {
         co_await serve_autonat_v1(operation);
      } else if (operation->protocol == autonat_protocol::v2_request) {
         co_await serve_autonat_v2(operation);
      } else {
         auto buffer = std::vector<std::uint8_t>{};
         const auto request = reachability::codec::decode_v2_dial_back(
             co_await async_read_length_delimited(operation->request, buffer, 1024), autonat_codec_options());
         operation->check();
         if (!operation->inbound_endpoint ||
             !consume_autonat_v2_nonce(request.nonce, *operation->inbound_endpoint)) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 unexpected or replayed dial-back");
         }
         co_await operation->request.async_write(reachability::codec::encode_v2_dial_back_response({}));
      }
      operation->check();
      co_await operation->request.async_close();
      operation->check();
   } catch (...) {
      operation->failure = std::current_exception();
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   if (operation->failure) {
      operation->cancel();
      try { co_await operation->request.async_close(); } catch (...) {}
   }
   operation->completed = true;
}

boost::asio::awaitable<reachability::v2::dial_status>
node::impl::probe_autonat(std::shared_ptr<autonat_operation> operation, endpoint target,
                           std::optional<std::uint64_t> nonce) {
   operation->check();
   auto dial = resources.reserve_dial(operation->peer);
   if (!dial) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "AutoNAT logical dial admission failed");
   }
   connection_gate->peer_dial(operation->peer);
   connection_gate->address_dial(operation->peer, target);
   auto result = reachability::v2::dial_status::dial_error;
   auto failure = std::exception_ptr{};
   try {
      auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          operation->deadline - std::chrono::steady_clock::now());
      operation->attempt = co_await connect_direct_attempt(
          std::move(target), node::connect_options{.expected_peer = operation->peer, .allow_relay = false,
              .timeout = std::min(remaining, std::chrono::milliseconds{1500}), .allow_hole_punch = false},
          operation->cancellation);
      operation->check();
      if (operation->attempt.connection.authentication == peer_authentication::unverified ||
          operation->attempt.connection.peer != operation->peer) {
         FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "AutoNAT dial-back identity mismatch");
      }
      result = reachability::v2::dial_status::ok;
      if (nonce) {
         result = reachability::v2::dial_status::dial_back_error;
         auto reservation = resources.reserve_stream(operation->peer, resource_manager::session_direction::outbound);
         if (!reservation) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "AutoNAT dial-back stream admission failed");
         }
         auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
         operation->dial_back_resource = resource;
         resource->attach(co_await operation->attempt.connection.session.async_open_stream());
         operation->check();
         const auto protocol = builtins::autonat_v2_dial_back;
         if (resource->bind_protocol(protocol) != resource_manager::stream_reservation::bind_result::accepted ||
             resource->bind_service_for_protocol(protocol, false) !=
                 resource_manager::stream_reservation::bind_result::accepted) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "AutoNAT dial-back protocol resource limit reached");
         }
         operation->dial_back = co_await protocol_negotiation::async_select(std::move(guarded), protocol);
         operation->check();
         result = co_await detail::async_autonat_v2_dialback(
             std::move(operation->dial_back), *nonce, runtime.context(), operation->deadline,
             operation->cancellation);
         operation->check();
      }
   } catch (...) {
      failure = std::current_exception();
      if (result == reachability::v2::dial_status::ok) {
         result = reachability::v2::dial_status::dial_error;
      }
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   operation->dial_back.request_cancel();
   if (operation->dial_back_resource) {
      operation->dial_back_resource->request_cancel();
      try {
         co_await operation->dial_back_resource->async_close();
      } catch (...) {
         if (!failure) {
            failure = std::current_exception();
            result = reachability::v2::dial_status::dial_back_error;
         }
      }
   }
   operation->dial_back = {};
   operation->dial_back_resource.reset();
   // Never publish this attempt or reuse an ordinary node session. Reset before
   // the close helper moves it so cancellation cannot lose the native owner.
   operation->attempt.connection.session.request_cancel();
   co_await async_close_direct_attempt(operation->attempt);
   operation->check();
   if (failure) {
      try {
         std::rethrow_exception(failure);
      } catch (const forge::exceptions::base& error) {
         if (nonce && result == reachability::v2::dial_status::dial_back_error &&
             detail::autonat_v2_remote_close(error)) {
            co_return result;
         }
         switch (p2p_code(error)) {
         case exceptions::code::timeout:
         case exceptions::code::peer_not_found:
         case exceptions::code::peer_verification_failed:
            break;
         case exceptions::code::protocol_error:
         case exceptions::code::codec_error:
         case exceptions::code::unsupported_protocol:
            if (result != reachability::v2::dial_status::dial_back_error) {
               throw;
            }
            break;
         default:
            // Local policy/resource/runtime refusal is not evidence that the
            // remote address is unreachable. Preserve its type after drain.
            throw;
         }
      } catch (...) {
         throw;
      }
   }
   co_return result;
}

boost::asio::awaitable<void> node::impl::serve_autonat_v1(std::shared_ptr<autonat_operation> operation) {
   auto buffer = std::vector<std::uint8_t>{};
   const auto request = reachability::codec::decode_v1(
       co_await async_read_length_delimited(operation->request, buffer, autonat_message_limit), autonat_codec_options());
   auto response = reachability::dial_response{.status = reachability::dial_status::bad_request};
   if (request.kind == reachability::message::message_kind::dial && request.peer &&
       request.peer->peer == operation->peer) {
      response.status = reachability::dial_status::dial_refused;
      if (operation->remote_endpoint &&
          public_probe_endpoint(*operation->remote_endpoint, operation->peer, operation->local_endpoints)) {
         for (auto candidate : request.peer->endpoints) {
            operation->check();
            if ((private_network_enabled() && !candidate.is_direct_tcp()) ||
                (candidate.transport.host_type != endpoint::host_kind::ip4 &&
                 candidate.transport.host_type != endpoint::host_kind::ip6) ||
                (candidate.peer && *candidate.peer != operation->peer)) {
               continue;
            }
            // Like Go/Rust v1, retain the requested port but only dial the observed IP.
            candidate.transport.host_type = operation->remote_endpoint->transport.host_type;
            candidate.transport.host = operation->remote_endpoint->transport.host;
            candidate.peer = operation->peer;
            if (!public_probe_endpoint(candidate, operation->peer, operation->local_endpoints)) {
               continue;
            }
            try {
               const auto result = co_await probe_autonat(operation, candidate, std::nullopt);
               response.status = result == reachability::v2::dial_status::ok
                   ? reachability::dial_status::ok : reachability::dial_status::dial_error;
            } catch (const forge::exceptions::base& error) {
               operation->check();
               switch (p2p_code(error)) {
               case exceptions::code::canceled:
               case exceptions::code::closed:
               case exceptions::code::timeout:
                  throw;
               case exceptions::code::connection_rejected:
               case exceptions::code::backpressure_rejected:
                  response.status = reachability::dial_status::dial_refused;
                  break;
               default:
                  response.status = reachability::dial_status::internal_error;
                  break;
               }
               break;
            }
            if (response.status == reachability::dial_status::ok) {
               response.endpoint = std::move(candidate);
               break;
            }
         }
      }
   }
   if (response.status == reachability::dial_status::dial_error) {
      // Hide fast connection failures without consuming the response/close budget.
      operation->delay = std::make_unique<asio::steady_timer>(co_await asio::this_coro::executor);
      operation->delay->expires_at(operation->deadline - options.reachability_policy.timeout / 4);
      auto error = boost::system::error_code{};
      co_await operation->delay->async_wait(asio::redirect_error(asio::use_awaitable, error));
      operation->check();
   }
   co_await operation->request.async_write(reachability::codec::encode_v1(
       {.kind = reachability::message::message_kind::dial_response, .response = std::move(response)}));
}

boost::asio::awaitable<void> node::impl::serve_autonat_v2(std::shared_ptr<autonat_operation> operation) {
   const auto codec = autonat_codec_options();
   auto buffer = std::vector<std::uint8_t>{};
   const auto request = reachability::codec::decode_v2(
       co_await async_read_length_delimited(operation->request, buffer, autonat_message_limit), codec);
   auto response = reachability::v2::dial_response{.status = reachability::v2::response_status::request_rejected};
   if (request.type == reachability::v2::message::kind::dial_request && request.dial_request &&
       request.dial_request->nonce != 0) {
      response.status = reachability::v2::response_status::dial_refused;
      for (std::size_t index = 0; index < request.dial_request->endpoints.size(); ++index) {
         const auto& candidate = request.dial_request->endpoints[index];
         if ((private_network_enabled() && !candidate.is_direct_tcp()) ||
             !public_probe_endpoint(candidate, operation->peer, operation->local_endpoints)) {
            continue;
         }
         operation->check();
         if (!operation->remote_endpoint || !same_ip(*operation->remote_endpoint, candidate)) {
            const auto required = 30'000 + random_nonce() % (autonat_data_limit - 30'000 + 1);
            co_await operation->request.async_write(reachability::codec::encode_v2(
                {.type = reachability::v2::message::kind::dial_data_request,
                 .dial_data_request = reachability::v2::dial_data_request{
                     .index = static_cast<std::uint32_t>(index), .bytes = required}}, codec));
            for (auto remaining = required; remaining != 0;) {
               operation->check();
               const auto data = reachability::codec::decode_v2(
                   co_await async_read_length_delimited(operation->request, buffer, autonat_message_limit), codec);
               if (data.type != reachability::v2::message::kind::dial_data_response || !data.dial_data_response) {
                  FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT expected dial data");
               }
               const auto size = data.dial_data_response->data.size();
               if (size == 0 || (size < 100 && size < remaining)) {
                  FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT invalid dial data size");
               }
               remaining = size >= remaining ? 0 : remaining - size;
            }
            operation->delay = std::make_unique<asio::steady_timer>(co_await asio::this_coro::executor);
            operation->delay->expires_after(std::chrono::milliseconds{static_cast<std::int64_t>(random_nonce() % 1001)});
            auto error = boost::system::error_code{};
            co_await operation->delay->async_wait(asio::redirect_error(asio::use_awaitable, error));
         }
         operation->check();
         response.index = static_cast<std::uint32_t>(index);
         try {
            response.dial_status = co_await probe_autonat(operation, candidate, request.dial_request->nonce);
            response.status = reachability::v2::response_status::ok;
         } catch (const forge::exceptions::base& error) {
            operation->check();
            switch (p2p_code(error)) {
            case exceptions::code::canceled:
            case exceptions::code::closed:
            case exceptions::code::timeout:
               throw;
            case exceptions::code::connection_rejected:
               response.status = reachability::v2::response_status::dial_refused;
               break;
            case exceptions::code::backpressure_rejected:
               response.status = reachability::v2::response_status::request_rejected;
               break;
            default:
               response.status = reachability::v2::response_status::internal_error;
               break;
            }
         }
         break;
      }
   }
   co_await operation->request.async_write(reachability::codec::encode_v2(
       {.type = reachability::v2::message::kind::dial_response, .dial_response = std::move(response)}, codec));
}

} // namespace forge::net::p2p
