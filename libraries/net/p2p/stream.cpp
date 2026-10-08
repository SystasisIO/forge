module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>

module forge.net.p2p.stream;

import forge.net.p2p.exceptions;
import forge.net.transport.stream;

namespace forge::net::p2p {

struct stream::impl {
   forge::net::transport::stream transport;
   peer_authentication authentication = peer_authentication::unverified;
};

stream::stream() = default;

stream::stream(forge::net::transport::stream value) : impl_(std::make_shared<impl>()) {
   impl_->transport = std::move(value);
}

stream::stream(forge::net::transport::stream value, std::vector<std::uint8_t> buffered)
    : stream{forge::net::transport::detail::stream_access::with_buffer(std::move(value), std::move(buffered))} {}

stream::~stream() = default;
stream::stream(stream&&) noexcept = default;
stream& stream::operator=(stream&&) noexcept = default;

bool stream::valid() const noexcept {
   return impl_ && impl_->transport.valid();
}

std::int64_t stream::id() const noexcept {
   return impl_ ? impl_->transport.id() : -1;
}

peer_authentication stream::authentication() const noexcept {
   return impl_ ? impl_->authentication : peer_authentication::unverified;
}

boost::asio::awaitable<void> stream::async_write(std::span<const std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   return impl_->transport.async_write(forge::net::transport::chunk{bytes});
}

boost::asio::awaitable<void> stream::async_write(forge::net::transport::chunk bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   return impl_->transport.async_write(std::move(bytes));
}

boost::asio::awaitable<std::vector<std::uint8_t>> stream::async_read() {
   auto value = co_await async_read_chunk();
   co_return std::move(value).into_vector();
}

boost::asio::awaitable<forge::net::transport::chunk> stream::async_read_chunk() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   co_return co_await impl_->transport.async_read_chunk();
}

boost::asio::awaitable<void> stream::async_write_frame(std::span<const std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   return impl_->transport.async_write_frame(forge::net::transport::chunk{bytes});
}

boost::asio::awaitable<void> stream::async_write_frame(forge::net::transport::chunk bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   return impl_->transport.async_write_frame(std::move(bytes));
}

boost::asio::awaitable<std::vector<std::uint8_t>> stream::async_read_frame() {
   co_return co_await async_read_frame(forge::net::transport::frame_options{});
}

boost::asio::awaitable<std::vector<std::uint8_t>> stream::async_read_frame(forge::net::transport::frame_options options) {
   auto value = co_await async_read_frame_chunk(options);
   co_return std::move(value).into_vector();
}

boost::asio::awaitable<forge::net::transport::chunk> stream::async_read_frame_chunk() {
   co_return co_await async_read_frame_chunk(forge::net::transport::frame_options{});
}

boost::asio::awaitable<forge::net::transport::chunk>
stream::async_read_frame_chunk(forge::net::transport::frame_options options) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   co_return co_await impl_->transport.async_read_frame_chunk(options);
}

boost::asio::awaitable<void> stream::async_close() {
   if (!impl_) {
      co_return;
   }
   co_await impl_->transport.async_close();
}

void stream::cancel() {
   if (impl_) {
      impl_->transport.cancel();
   }
}

void stream::request_cancel() noexcept {
   if (impl_) {
      impl_->transport.request_cancel();
   }
}

forge::net::transport::stream stream::into_transport_stream() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid P2P stream");
   }
   auto transport = std::move(impl_->transport);
   impl_.reset();
   return transport;
}

stream detail::stream_access::with_buffer(stream value, std::vector<std::uint8_t> buffered) {
   if (!value.impl_ || buffered.empty()) {
      return value;
   }
   value.impl_->transport = forge::net::transport::detail::stream_access::with_buffer(
       std::move(value.impl_->transport), std::move(buffered));
   return value;
}

void detail::stream_access::set_authentication(stream& value,
                                               peer_authentication authentication) noexcept {
   if (value.impl_) {
      value.impl_->authentication = authentication;
   }
}

} // namespace forge::net::p2p
