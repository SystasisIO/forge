module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

module forge.log.async_sink;

import forge.asio.compute;
import forge.asio.notification;

#include "details/async_sink_impl.hxx"

namespace forge {

async_sink::async_sink(std::shared_ptr<sink> downstream, forge::asio::compute::executor executor,
                       async_sink_options options)
    : _impl{std::make_shared<impl>(std::move(downstream), std::move(executor), options)} {}

async_sink::~async_sink() {
   close();
}

void async_sink::log(const log_record& record) noexcept {
   const auto owner = _impl;
   owner->enqueue(record);
}

void async_sink::close() noexcept {
   const auto owner = _impl;
   owner->close();
}

async_sink_metrics async_sink::snapshot() const noexcept {
   const auto owner = _impl;
   return owner->snapshot();
}

boost::asio::awaitable<bool> async_sink::async_flush(std::chrono::milliseconds timeout) {
   auto owner = _impl;
   const auto deadline = impl::deadline_after(timeout);
   const auto fence = owner->capture_fence(false);
   return impl::wait_fence(std::move(owner), fence, deadline);
}

boost::asio::awaitable<bool> async_sink::async_shutdown(std::chrono::milliseconds timeout) {
   auto owner = _impl;
   const auto deadline = impl::deadline_after(timeout);
   const auto fence = owner->capture_fence(true);
   return impl::wait_fence(std::move(owner), fence, deadline);
}

} // namespace forge
