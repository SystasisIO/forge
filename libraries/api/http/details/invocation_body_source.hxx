#pragma once

namespace forge::api::http::detail {

class invocation_body_source final {
 public:
   invocation_body_source(forge::net::http::stream_response::body_source body,
                           forge::api::core::invocation_context context, std::function<void()> cancel);

   boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> async_read();
   boost::asio::awaitable<void> async_send(boost::asio::awaitable<void> operation,
                                          std::function<void()> abort);

 private:
   forge::net::http::stream_response::body_source body_;
   forge::api::core::invocation_context context_;
   std::function<void()> cancel_;
   std::chrono::steady_clock::time_point next_recheck_;
   bool sending_ = false;
};

} // namespace forge::api::http::detail
