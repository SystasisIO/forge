module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;

#include "details/owner_cancellation.hxx"

namespace forge::net::p2p::detail {

owner_stream_cancellation::owner_stream_cancellation(boost::asio::cancellation_slot slot,
                                                     std::shared_ptr<forge::net::p2p::stream> stream)
    : slot_(std::move(slot)), stream_(std::move(stream)) {
   auto weak = std::weak_ptr<forge::net::p2p::stream>{stream_};
   if (slot_.is_connected()) {
      slot_.assign([weak = std::move(weak)](boost::asio::cancellation_type) noexcept {
         if (auto stream = weak.lock()) {
            stream->request_cancel();
         }
      });
   }
}

owner_stream_cancellation::~owner_stream_cancellation() noexcept {
   clear_owner_cancellation(slot_);
}

void clear_owner_cancellation(boost::asio::cancellation_slot slot) noexcept {
   try {
      slot.clear();
   } catch (...) {
      // A custom cancellation slot cannot escape operation cleanup.
   }
}

void owner_stream_cancellation::request_cancel() noexcept {
   if (stream_) {
      stream_->request_cancel();
   }
}

stream_admission_handler make_owner_stream_admission(boost::asio::cancellation_slot slot,
                                                     std::shared_ptr<worker_stop_bridge> stop,
                                                     owner_stream_lifetime lifetime) {
   auto admitted = std::make_shared<std::weak_ptr<resource_stream>>();
   auto admitted_callback = [slot, stop, admitted](const std::shared_ptr<resource_stream>& resource) mutable {
      *admitted = resource;
      auto weak = std::weak_ptr<resource_stream>{resource};
      if (slot.is_connected()) {
         slot.assign([weak = std::move(weak)](boost::asio::cancellation_type) noexcept {
            if (auto owned = weak.lock()) {
               owned->request_cancel();
            }
         });
      }
      if (stop->stop_requested()) {
         resource->request_cancel();
      }
   };
   auto commit_callback = [slot = std::move(slot), stop = std::move(stop), admitted, lifetime]() mutable {
      if (stop->stop_requested()) {
         if (auto resource = admitted->lock()) {
            resource->request_cancel();
         }
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P stream owner stopped before admission commit");
      }
      if (lifetime == owner_stream_lifetime::negotiation) {
         clear_owner_cancellation(slot);
      }
   };
   return stream_admission_handler{std::move(admitted_callback), std::move(commit_callback)};
}

boost::asio::awaitable<void> async_run_with_owner_cancellation(std::shared_ptr<worker_stop_bridge> stop,
                                                               owner_cancellable_work work,
                                                               worker_stop_bridge_options options) {
   namespace asio = boost::asio;

   co_await async_run_with_stop_bridge(
       std::move(stop),
       [work = std::move(work)](std::shared_ptr<worker_terminal_owner> terminal) mutable -> asio::awaitable<void> {
          const auto executor = co_await asio::this_coro::executor;
          auto task_signal = std::make_shared<asio::cancellation_signal>();
          auto stream_signal = std::make_shared<asio::cancellation_signal>();
          co_await asio::co_spawn(
              executor,
              [terminal = std::move(terminal), task_signal, stream_signal,
               work = std::move(work)]() mutable -> asio::awaitable<void> {
                 static_cast<void>(terminal->publish(worker_terminal_owner::callback{
                     [task_signal, stream_signal]() noexcept {
                        try {
                           stream_signal->emit(asio::cancellation_type::all);
                        } catch (...) {
                           // A custom cancellation handler cannot escape the
                           // terminal boundary or corrupt sibling operations.
                        }
                        try {
                           task_signal->emit(asio::cancellation_type::all);
                        } catch (...) {
                           // Coroutine cancellation remains independent from
                           // the published stream's terminal handler.
                        }
                     },
                 }));
                 co_await work(stream_signal->slot());
              },
              asio::bind_cancellation_slot(task_signal->slot(), asio::use_awaitable));
       },
       std::move(options));
}

} // namespace forge::net::p2p::detail
