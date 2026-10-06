module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

module forge.net.p2p.node;

import forge.net.p2p.stream;
import forge.net.transport.stream;

#include "details/secure_stream_concept.hxx"
#include "details/secure_io.hxx"

namespace forge::net::p2p::detail {

secure_stream_concept::secure_stream_concept(std::shared_ptr<secure_io> secure) : secure_(std::move(secure)) {}

[[nodiscard]] bool secure_stream_concept::valid() const noexcept {
   return secure_ && secure_->valid();
}

[[nodiscard]] std::int64_t secure_stream_concept::id() const noexcept {
   return secure_ ? secure_->id() : -1;
}

boost::asio::awaitable<void> secure_stream_concept::async_write(std::span<const std::uint8_t> bytes) {
   co_await secure_->async_write(bytes);
}

boost::asio::awaitable<std::vector<std::uint8_t>> secure_stream_concept::async_read() {
   co_return co_await secure_->async_read();
}

boost::asio::awaitable<void> secure_stream_concept::async_close() {
   co_await secure_->async_close();
}

void secure_stream_concept::cancel() {
   if (secure_) {
      secure_->cancel();
   }
}

void secure_stream_concept::request_cancel() noexcept {
   if (secure_) {
      secure_->request_cancel();
   }
}

[[nodiscard]] forge::net::transport::stream secure_transport_stream(std::shared_ptr<secure_io> secure) {
   auto model = std::make_shared<secure_stream_concept>(std::move(secure));
   auto weak = std::weak_ptr<secure_stream_concept>{model};
   return forge::net::transport::detail::stream_access::make_cancelable(std::move(model),
                                                                        [weak = std::move(weak)]() noexcept {
                                                                           if (auto stream = weak.lock()) {
                                                                              stream->request_cancel();
                                                                           }
                                                                        });
}

} // namespace forge::net::p2p::detail
