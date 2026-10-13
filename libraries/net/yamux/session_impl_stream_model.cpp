module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>

module forge.net.yamux.session;

import forge.asio.gate;
import forge.asio.notification;

#include "details/session_impl.hxx"
#include "details/session_impl_stream_state.hxx"
#include "details/session_impl_stream_model.hxx"

namespace forge::net::yamux {

session::impl::stream_model::stream_model(std::shared_ptr<impl> owner,
                                          std::shared_ptr<stream_state> state)
    : owner_(std::move(owner)), state_(std::move(state)) {}

bool session::impl::stream_model::valid() const noexcept {
   auto owner = owner_.lock();
   return owner && owner->stream_valid(state_);
}

std::int64_t session::impl::stream_model::id() const noexcept {
   return static_cast<std::int64_t>(state_->id);
}

boost::asio::awaitable<void> session::impl::stream_model::async_write(std::span<const std::uint8_t> value) {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session expired");
   }
   auto owned = detail::bytes{value.begin(), value.end()};
   co_await owner->write_stream(state_, std::move(owned));
}

boost::asio::awaitable<void> session::impl::stream_model::async_write_chunk(transport::chunk value) {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session expired");
   }
   auto [bytes, lifetime] = transport::detail::chunk_access::consume(std::move(value));
   co_await owner->write_stream(state_, std::move(bytes), std::move(lifetime));
}

boost::asio::awaitable<detail::bytes> session::impl::stream_model::async_read() {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session expired");
   }
   co_return co_await owner->read_stream(state_);
}

boost::asio::awaitable<transport::chunk> session::impl::stream_model::async_read_chunk() {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session expired");
   }
   co_return transport::chunk{co_await owner->read_stream(state_)};
}

boost::asio::awaitable<void> session::impl::stream_model::async_close_write() {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "yamux session expired");
   }
   // Yamux FIN closes only the local write direction.
   co_await owner->close_stream(state_);
}

boost::asio::awaitable<void> session::impl::stream_model::async_close() {
   auto owner = owner_.lock();
   if (owner) {
      co_await owner->close_stream(state_);
   }
}

void session::impl::stream_model::cancel() {
   request_cancel();
}

void session::impl::stream_model::request_cancel() noexcept {
   auto owner = owner_.lock();
   if (owner) {
      owner->request_cancel_stream(state_);
   }
}

} // namespace forge::net::yamux
