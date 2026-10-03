module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
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
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
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

#include "details/dht_exchange.hxx"
#include "details/dht_time.hxx"
#include "details/host_addresses.hxx"
#include "details/node_impl.hxx"
#include "details/operation_deadline.hxx"
#include "details/peer_failure.hxx"

namespace forge::net::p2p {

[[nodiscard]] host_addresses::learning_context
discovery_context_for_session_peer(std::optional<peer_id> session_peer, std::optional<endpoint> session_remote_endpoint,
                                   std::optional<endpoint> session_direct_endpoint, const peer_id& peer);

[[nodiscard]] std::chrono::system_clock::time_point
dht_value_expiry(const dht::record& value, std::chrono::system_clock::time_point now, const dht::profile& profile) {
   const auto requested = value.ttl.count() > 0 ? value.ttl : profile.limits.value_record_ttl;
   const auto bounded = std::chrono::duration_cast<std::chrono::system_clock::duration>(
       std::min(requested, profile.limits.value_record_ttl));
   const auto maximum = (std::chrono::system_clock::time_point::max)();
   return now > maximum - bounded ? maximum : now + bounded;
}

namespace {

constexpr auto inbound_dht_response_limit = std::size_t{16 * 1024};

[[nodiscard]] bool protocol_open_failure_requires_peer_penalty(const forge::exceptions::base& error) {
   const auto kind = p2p_code(error);
   return kind == exceptions::code::unsupported_protocol || kind == exceptions::code::protocol_error ||
          kind == exceptions::code::codec_error;
}

void record_dht_exchange_failure(std::mutex& mutex, const bool& stopped, peer_store& store, const peer_id& peer,
                                 const forge::exceptions::base& error) {
   auto node_stopped = false;
   {
      auto lock = std::scoped_lock{mutex};
      node_stopped = stopped;
   }
   if (detail::remote_peer_attributable_failure(p2p_code(error), node_stopped)) {
      store.mark_failure(peer);
   }
}

[[nodiscard]] dht::peer sanitize_discovered_peer_for_session(dht::peer value, const auto& session) {
   value.endpoints = host_addresses::sanitize_discovered_addresses(
       std::move(value.endpoints), value.id,
       discovery_context_for_session_peer(session ? std::optional<peer_id>{session->info.remote_peer} : std::nullopt,
                                          session ? session->remote_endpoint : std::nullopt,
                                          session ? session->direct_endpoint : std::nullopt, value.id));
   return value;
}

[[nodiscard]] dht::peer provider_peer(const dht::record_store::provider_record& value) {
   return dht::peer{.id = value.provider,
                    .endpoints = value.endpoints,
                    .connection = value.endpoints.empty() ? dht::connection_type::not_connected
                                                          : dht::connection_type::can_connect};
}

[[nodiscard]] bool response_fits(const dht::message& response, const dht::profile& profile) {
   try {
      static_cast<void>(dht::codec::encode(response, profile));
      return true;
   } catch (const exceptions::invalid_options&) {
      return false;
   }
}

void append_unique_bounded(dht::message& response, std::vector<dht::peer>& peers, dht::peer value, std::size_t limit,
                           const dht::profile& profile) {
   const auto current = std::ranges::find_if(peers, [&](const auto& candidate) { return candidate.id == value.id; });
   if (current == peers.end()) {
      if (peers.size() >= limit) {
         return;
      }
      auto endpoints = std::move(value.endpoints);
      value.endpoints.clear();
      peers.push_back(std::move(value));
      if (!response_fits(response, profile)) {
         peers.pop_back();
         return;
      }
      auto& inserted = peers.back();
      for (auto& endpoint : endpoints) {
         inserted.endpoints.push_back(std::move(endpoint));
         if (!response_fits(response, profile)) {
            inserted.endpoints.pop_back();
         }
      }
      return;
   }
   for (auto& endpoint : value.endpoints) {
      const auto known = std::ranges::any_of(
          current->endpoints, [&](const auto& candidate) { return candidate.to_string() == endpoint.to_string(); });
      if (!known) {
         current->endpoints.push_back(std::move(endpoint));
         if (!response_fits(response, profile)) {
            current->endpoints.pop_back();
         }
      }
   }
}

void assign_record_bounded(dht::message& response, dht::record value, const dht::profile& profile) {
   response.record_value = std::move(value);
   if (!response_fits(response, profile)) {
      response.record_value.reset();
   }
}

} // namespace

void node::impl::increment_dht_query() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.dht_queries;
}

void node::impl::increment_dht_response() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.dht_responses;
}

boost::asio::awaitable<node::impl::dht_exchange_result>
node::impl::exchange_dht(const protocol_id& protocol, const peer_id& peer, dht::message request,
                         std::chrono::milliseconds timeout, std::shared_ptr<cancellation_latch> cancellation) {
   const auto started = std::chrono::steady_clock::now();
   auto& state = dht_profile(protocol);
   auto opened = co_await open_protocol_direct_with_context(
       peer, protocol, timeout, node::open_options{}.max_direct_endpoints,
       node::open_options{}.direct_attempt_timeout, cancellation);
   try {
      auto response = co_await detail::async_exchange_dht(std::move(opened.stream), std::move(request), state.profile,
                                                           runtime.context(),
                                                           remaining_timeout(started, timeout, "P2P DHT exchange"),
                                                           std::move(cancellation));
      co_return dht_exchange_result{
          .message = std::move(response),
          .remote_endpoint = std::move(opened.remote_endpoint),
          .direct_endpoint = std::move(opened.direct_endpoint),
      };
   } catch (const forge::exceptions::base& error) {
      record_dht_exchange_failure(mutex, stopped, store, peer, error);
      throw;
   }
}

boost::asio::awaitable<void> node::impl::send_dht(const protocol_id& protocol, const peer_id& peer,
                                                  dht::message request, std::chrono::milliseconds timeout,
                                                  std::shared_ptr<cancellation_latch> cancellation) {
   const auto started = std::chrono::steady_clock::now();
   auto& state = dht_profile(protocol);
   auto stream = std::optional<opened_direct_stream>{};
   try {
      stream.emplace(co_await open_protocol_direct_with_context(
          peer, protocol, timeout, node::open_options{}.max_direct_endpoints,
          node::open_options{}.direct_attempt_timeout, cancellation));
   } catch (const forge::exceptions::base& error) {
      if (protocol_open_failure_requires_peer_penalty(error)) {
         store.mark_failure(peer);
      }
      throw;
   }
   try {
      co_await detail::async_send_dht(std::move(stream->stream), std::move(request), state.profile, runtime.context(),
                                      remaining_timeout(started, timeout, "P2P DHT send"),
                                      std::move(cancellation));
   } catch (const forge::exceptions::base& error) {
      record_dht_exchange_failure(mutex, stopped, store, peer, error);
      throw;
   }
}

boost::asio::awaitable<void> node::impl::handle_dht(std::shared_ptr<node::impl::session_state> session,
                                                    protocol_id protocol, forge::net::p2p::stream stream) {
   auto& state = dht_profile(protocol);
   const auto& profile = state.profile;
   auto response_profile = profile;
   response_profile.limits.max_outbound_message_size =
       std::min(response_profile.limits.max_outbound_message_size, inbound_dht_response_limit);
   if (profile.operating_mode != dht::mode::server) {
      FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "DHT profile server mode is disabled");
   }
   auto buffer = std::vector<std::uint8_t>{};
   while (true) {
      auto deadline = operation_deadline{runtime.context(), profile.limits.query_timeout};
      deadline.arm([&stream] noexcept { stream.request_cancel(); });
      try {
         auto encoded = std::vector<std::uint8_t>{};
         try {
            encoded = co_await async_read_length_delimited(stream, buffer, profile.limits.max_inbound_message_size);
         } catch (const forge::exceptions::base& error) {
            if (!is_clean_stream_eof(error)) {
               throw;
            }
            if (!buffer.empty()) {
               FORGE_THROW_EXCEPTION(exceptions::codec_error, "DHT stream ended with a truncated message");
            }
            if (!deadline.finish()) {
               throw_operation_timeout("P2P inbound DHT exchange");
            }
            co_return;
         }

         auto request = dht::codec::decode(encoded, profile);
         increment_dht_query();
         detail::validate_dht_request(request, session->info.remote_peer, profile);
         if (request.type != dht::message_type::ping && request.key_value.bytes.empty()) {
            FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DHT request key must not be empty");
         }

         auto response = dht::message{.type = request.type};
         if (request.type == dht::message_type::put_value) {
            response.key_value = request.key_value;
            if (!response_fits(response, response_profile)) {
               response.key_value = {};
            }
         }
         const auto append_closest = [&] {
            for (auto& peer : state.routing.closest(request.key_value.bytes, profile.limits.replication)) {
               if (peer.id != session->info.remote_peer) {
                  append_unique_bounded(response, response.closer_peers, std::move(peer), profile.limits.replication,
                                        response_profile);
               }
            }
         };

         switch (request.type) {
         case dht::message_type::find_node: {
            try {
               const auto requested = peer_id::from_bytes(request.key_value.bytes);
               if (requested == local) {
                  auto addresses = std::vector<forge::multiformats::multiaddr>{};
                  for (const auto& endpoint : local_endpoints_for_control()) {
                     addresses.push_back(endpoint.to_multiaddr());
                  }
                  append_unique_bounded(response, response.closer_peers,
                                        dht::peer{.id = local,
                                                  .endpoints = std::move(addresses),
                                                  .connection = dht::connection_type::connected},
                                        profile.limits.replication, response_profile);
               } else if (requested != session->info.remote_peer) {
                  if (const auto record = store.find(requested)) {
                     auto exact = dht::peer{.id = requested, .connection = dht::connection_type::can_connect};
                     for (const auto& item : record->endpoints) {
                        exact.endpoints.push_back(item.address);
                     }
                     append_unique_bounded(response, response.closer_peers, std::move(exact),
                                           profile.limits.replication, response_profile);
                  }
               }
            } catch (const forge::exceptions::base&) {
               // Arbitrary keys use ordinary XOR-distance routing.
            }
            append_closest();
            break;
         }
         case dht::message_type::get_providers:
            for (const auto& provider :
                 state.records.find_providers(request.key_value, profile.limits.max_provider_peers)) {
               if (provider.provider != session->info.remote_peer) {
                  append_unique_bounded(response, response.provider_peers, provider_peer(provider),
                                        profile.limits.max_provider_peers, response_profile);
               }
            }
            append_closest();
            break;
         case dht::message_type::add_provider: {
            const auto now = std::chrono::system_clock::now();
            auto accepted = std::optional<dht::peer>{};
            for (auto provider : request.provider_peers) {
               if (provider.id != session->info.remote_peer) {
                  continue;
               }
               provider = sanitize_discovered_peer_for_session(std::move(provider), session);
               if (!accepted) {
                  accepted = dht::peer{.id = provider.id};
               }
               for (auto& candidate : provider.endpoints) {
                  const auto duplicate = std::ranges::any_of(accepted->endpoints, [&](const auto& current) {
                     return current.to_string() == candidate.to_string();
                  });
                  if (!duplicate && accepted->endpoints.size() < profile.limits.max_peer_endpoints) {
                     accepted->endpoints.push_back(std::move(candidate));
                  }
               }
            }
            if (accepted) {
               const auto addresses_expire = accepted->endpoints.empty()
                                                 ? std::chrono::system_clock::time_point{}
                                                 : detail::dht_expiry_after(now, profile.limits.provider_address_ttl);
               co_await state.records.async_upsert_provider(dht::record_store::provider_record{
                   .key = request.key_value,
                   .provider = accepted->id,
                   .endpoints = std::move(accepted->endpoints),
                   .provider_expires_at = detail::dht_expiry_after(now, profile.limits.provider_record_ttl),
                   .addresses_expires_at = addresses_expire,
               });
            }
            if (!deadline.finish()) {
               throw_operation_timeout("P2P inbound DHT exchange");
            }
            continue;
         }
         case dht::message_type::put_value: {
            assign_record_bounded(response, *request.record_value, response_profile);
            if (!response.record_value) {
               FORGE_THROW_EXCEPTION(exceptions::protocol_error,
                                     "DHT PUT_VALUE echo exceeds the outbound message limit");
            }
            const auto now = std::chrono::system_clock::now();
            const auto stored = co_await state.records.async_put(
                dht::record_store::value_record{
                    .record = *request.record_value,
                    .expires_at = dht_value_expiry(*request.record_value, now, profile),
                },
                now);
            if (stored.outcome != dht::record_store::put_outcome::incoming_stored) {
               FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DHT PUT_VALUE incoming record was not selected");
            }
            break;
         }
         case dht::message_type::get_value:
            if (const auto value = state.records.find_value(request.key_value)) {
               auto record = value->record;
               if (record.ttl > std::chrono::seconds::zero()) {
                  const auto remaining = value->expires_at - std::chrono::system_clock::now();
                  record.ttl =
                      std::max(std::chrono::seconds{1}, std::chrono::duration_cast<std::chrono::seconds>(remaining));
               }
               assign_record_bounded(response, std::move(record), response_profile);
            }
            append_closest();
            break;
         case dht::message_type::ping:
            break;
         }

         increment_dht_response();
         co_await stream.async_write(dht::codec::encode(response, response_profile));
         if (!deadline.finish()) {
            throw_operation_timeout("P2P inbound DHT exchange");
         }
      } catch (...) {
         const auto completed = deadline.finish();
         stream.cancel();
         if (deadline.timed_out() || !completed) {
            throw_operation_timeout("P2P inbound DHT exchange");
         }
         throw;
      }
   }
}

} // namespace forge::net::p2p
