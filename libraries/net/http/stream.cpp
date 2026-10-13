module;

#include "details/stream_server_access.hxx"

#include <optional>
#include <utility>

#include <boost/asio/awaitable.hpp>

module forge.net.http.stream;

namespace forge::net::http {

stream_response::body_source stream_response::body_source::with_callback(callback_type callback) const {
   return body_source{std::move(callback), request_body_marker_};
}

streaming_response streaming_response::from_source(streaming_response_options options) {
   auto result = streaming_response{};
   result.head_.version(11);
   result.head_.set(field::content_type, options.content_type);
   result.source_ = std::move(options.body);
   return result;
}

streaming_response streaming_response::from_body(response head, body_reader body) {
   auto result = streaming_response{};
   result.head_ = std::move(head);
   result.reader_ = std::move(body);
   return result;
}

status streaming_response::status_code() const noexcept {
   return head_.result();
}

const response& streaming_response::head() const noexcept {
   return head_;
}

std::string streaming_response::content_type() const {
   if (auto found = head_.find(field::content_type); found != head_.end()) {
      return std::string{found->value()};
   }
   return {};
}

body_reader& streaming_response::body() noexcept {
   return reader_;
}

stream_response streaming_response::materialize(const request& request_value, status success_status) && {
   return std::move(*this).materialize_impl(request_value, success_status);
}

stream_response streaming_response::materialize(const stream_request& request_value, status success_status) && {
   return std::move(*this).materialize_impl(request_value.context.request, success_status);
}

stream_response streaming_response::materialize_impl(const request& request_value, status success_status) && {
   if (source_) {
      head_.result(success_status);
      head_.version(request_value.version());
      head_.keep_alive(request_value.keep_alive());
      return stream_response{.head = std::move(head_), .body = std::move(source_)};
   }
   if (reader_.valid()) {
      head_.version(request_value.version());
      head_.keep_alive(request_value.keep_alive());
      auto reader = std::move(reader_);
      const auto request_body_marker = reader.requires_continue_before_response()
                                          ? detail::stream_server_access::continue_before_response_marker(reader)
                                          : nullptr;
      auto callback =
         stream_response::body_source::callback_type{[reader = std::move(reader)]() mutable
                                                        -> boost::asio::awaitable<std::optional<body_chunk>> {
            co_return co_await reader.async_read();
         }};
      auto body = request_body_marker != nullptr
                     ? stream_response::body_source{std::move(callback), std::move(request_body_marker)}
                     : stream_response::body_source{std::move(callback)};
      return stream_response{.head = std::move(head_),
                             .body = std::move(body)};
   }
   return stream_response::buffered(std::move(head_));
}

} // namespace forge::net::http
