module;

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/steady_timer.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.reachability;
import forge.net.p2p.stream;

#include "details/autonat_exchange.hxx"
#include "details/cancellation_latch.hxx"
#include "details/length_delimited.hxx"
#include "details/operation_deadline.hxx"

namespace forge::net::p2p::detail {
namespace {

bool consistent_dialback(const endpoint& local, const endpoint& requested) {
   // NAT changes the IP/port. The local socket must still use the requested native transport.
   return !local.relayed && !requested.relayed &&
          local.transport.host_type == requested.transport.host_type &&
          local.transport.protocol == requested.transport.protocol &&
          local.encapsulation == requested.encapsulation;
}

boost::asio::awaitable<reachability::result> exchange_v1(stream& channel, peer_id local,
                                                        std::vector<endpoint> candidates) {
   auto options = reachability::options{};
   options.max_message_size = 4096;
   options.max_endpoints = 16;
   co_await channel.async_write(reachability::codec::encode_v1(reachability::message{
       .kind = reachability::message::message_kind::dial,
       .peer = reachability::peer_info{local, candidates},
   }, options));
   auto buffer = std::vector<std::uint8_t>{};
   const auto message = reachability::codec::decode_v1(
       co_await async_read_length_delimited(channel, buffer, options.max_message_size), options);
   if (message.kind != reachability::message::message_kind::dial_response || !message.response) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v1 expected dial response");
   }
   const auto& response = *message.response;
   if (response.status == reachability::dial_status::ok) {
      if (!response.endpoint || (!response.endpoint->is_direct_tcp() && !response.endpoint->is_direct_quic()) ||
          response.endpoint->transport.port == 0 ||
          (response.endpoint->peer && *response.endpoint->peer != local)) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v1 response has an invalid dial address");
      }
      // Donors may substitute the observed address. This is a v1 vote only;
      // the returned address is never authority for Identify advertisement.
      co_return reachability::result{reachability::state::publicly_reachable, response.endpoint};
   }
   co_return reachability::result{response.status == reachability::dial_status::dial_error
       ? reachability::state::private_network : reachability::state::unknown};
}

boost::asio::awaitable<reachability::result> exchange_v2(stream& channel, std::vector<endpoint> candidates,
                                                        std::uint64_t nonce, autonat_dialback_wait wait,
                                                        std::shared_ptr<cancellation_latch> cancellation) {
   auto options = reachability::options{};
   options.max_message_size = 8192;
   options.max_endpoints = 16;
   co_await channel.async_write(reachability::codec::encode_v2(reachability::v2::message{
       .type = reachability::v2::message::kind::dial_request,
       .dial_request = reachability::v2::dial_request{candidates, nonce},
   }, options));
   auto buffer = std::vector<std::uint8_t>{};
   auto message = reachability::codec::decode_v2(
       co_await async_read_length_delimited(channel, buffer, options.max_message_size), options);
   if (message.type == reachability::v2::message::kind::dial_data_request && message.dial_data_request) {
      const auto request = *message.dial_data_request;
      if (request.index >= candidates.size() || request.bytes > 100'000) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 dial data request exceeds offered bounds");
      }
      for (auto remaining = request.bytes; remaining != 0;) {
         const auto bytes = std::min<std::uint64_t>(4000, remaining);
         co_await channel.async_write(reachability::codec::encode_v2(reachability::v2::message{
             .type = reachability::v2::message::kind::dial_data_response,
             .dial_data_response = reachability::v2::dial_data_response{std::vector<std::uint8_t>(bytes)},
         }, options));
         remaining -= bytes;
      }
      message = reachability::codec::decode_v2(
          co_await async_read_length_delimited(channel, buffer, options.max_message_size), options);
      if (!message.dial_response || (message.dial_response->status == reachability::v2::response_status::ok &&
          message.dial_response->index != request.index)) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 changed its selected address");
      }
   }
   if (message.type != reachability::v2::message::kind::dial_response || !message.dial_response) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 expected one terminal dial response");
   }
   const auto response = *message.dial_response;
   if (response.status != reachability::v2::response_status::ok) {
      co_return reachability::result{};
   }
   if (response.index >= candidates.size() || response.dial_status == reachability::v2::dial_status::unused) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 invalid terminal address or dial status");
   }
   auto selected = candidates[response.index];
   if (response.dial_status == reachability::v2::dial_status::dial_error) {
      co_return reachability::result{reachability::state::private_network, std::move(selected)};
   }
   if (response.dial_status != reachability::v2::dial_status::ok &&
       response.dial_status != reachability::v2::dial_status::dial_back_error) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 invalid terminal dial status");
   }
   const auto actual = co_await wait(std::move(cancellation));
   if (!actual || !consistent_dialback(*actual, selected)) {
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "AutoNAT v2 dialback did not prove the selected transport");
   }
   co_return reachability::result{reachability::state::publicly_reachable, std::move(selected)};
}

} // namespace

boost::asio::awaitable<reachability::result> async_exchange_autonat(
    stream channel, peer_id local, std::vector<endpoint> candidates, bool v2, std::uint64_t nonce,
    autonat_dialback_wait dialback, boost::asio::io_context& context, std::chrono::milliseconds timeout,
    std::shared_ptr<cancellation_latch> cancellation) {
   if (candidates.empty() || candidates.size() > 16 || (v2 && (!dialback || nonce == 0))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "AutoNAT exchange requires bounded candidates and correlation");
   }
   auto deadline = operation_deadline{context, timeout};
   auto child = std::make_shared<cancellation_latch>();
   auto parent = cancellation_latch::subscribe(cancellation, [stop = deadline.stopping()] noexcept {
      static_cast<void>(stop.request_stop());
   });
   deadline.arm([&channel, child] noexcept { channel.request_cancel(); child->request_stop(); });
   auto result = reachability::result{};
   auto failure = std::exception_ptr{};
   try {
      result = v2 ? co_await exchange_v2(channel, std::move(candidates), nonce, std::move(dialback), child)
                       : co_await exchange_v1(channel, std::move(local), std::move(candidates));
      co_await channel.async_close();
   } catch (...) {
      failure = std::current_exception();
   }
   static_cast<void>(deadline.finish());
   if (failure || deadline.stopped() || deadline.timed_out()) {
      channel.request_cancel();
      // Keep the stream and its reservation until terminal transport cleanup,
      // even when the coroutine's caller has already canceled its wait.
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      try { co_await channel.async_close(); }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   if (deadline.timed_out()) { throw_operation_timeout("AutoNAT exchange"); }
   if (deadline.stopped()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT exchange canceled");
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return result;
}

} // namespace forge::net::p2p::detail
