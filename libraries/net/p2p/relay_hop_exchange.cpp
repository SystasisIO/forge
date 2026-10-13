module;

#include <forge/exceptions/macros.hpp>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;

#include "details/operation_deadline.hxx"
#include "details/cancellation_latch.hxx"
#include "details/owner_cancellation.hxx"
#include "details/length_delimited.hxx"
#include "details/relay_hop_exchange.hxx"

namespace forge::net::p2p::detail {

boost::asio::awaitable<relay_hop_exchange>
async_exchange_relay_hop(boost::asio::io_context& context, std::chrono::milliseconds timeout, std::string operation,
                         relay_stream_opener open_stream, relay::hop_message request, std::size_t max_message_size,
                         std::shared_ptr<cancellation_latch> cancellation) {
   auto stop = std::make_shared<worker_stop_bridge>();
   auto subscription = cancellation_latch::subscribe(cancellation, [stop]() noexcept { stop->request_stop(); });
   auto result = std::optional<relay_hop_exchange>{};
   auto deadline = operation_deadline{context, timeout};
   deadline.arm([stop] noexcept { stop->request_stop(); });

   try {
      co_await async_run_with_owner_cancellation(
          stop,
          [open_stream = std::move(open_stream), request = std::move(request), max_message_size, stop,
           &result](boost::asio::cancellation_slot slot) mutable -> boost::asio::awaitable<void> {
             if (stop->stop_requested()) {
                FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay HOP exchange canceled before stream admission");
             }

             auto admission = make_owner_stream_admission(slot, stop);
             auto owned_stream = std::make_shared<forge::net::p2p::stream>(co_await open_stream(std::move(admission)));
             auto cancellation = owner_stream_cancellation{std::move(slot), owned_stream};
             if (stop->stop_requested()) {
                cancellation.request_cancel();
                FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay HOP exchange canceled during stream admission");
             }

             co_await owned_stream->async_write(relay::codec::encode_hop(request));
             auto buffered = std::vector<std::uint8_t>{};
             auto response = relay::codec::decode_hop(
                 co_await async_read_length_delimited(*owned_stream, buffered, max_message_size));
             result.emplace(relay_hop_exchange{
                 .stream = std::move(*owned_stream),
                 .buffered = std::move(buffered),
                 .response = std::move(response),
             });
          });
   } catch (...) {
      auto failure = std::current_exception();
      const auto completed = deadline.finish();
      if (!completed || deadline.timed_out()) {
         throw_operation_timeout(operation);
      }
      std::rethrow_exception(failure);
   }

   const auto completed = deadline.finish();
   if (!completed || deadline.timed_out()) {
      throw_operation_timeout(operation);
   }
   if (!result) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P relay HOP exchange stopped before completion");
   }
   co_return std::move(*result);
}

} // namespace forge::net::p2p::detail
