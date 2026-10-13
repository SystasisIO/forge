module;

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <boost/asio/awaitable.hpp>

namespace forge::net::http::detail {
struct stream_server_access;
}

export module forge.net.http.stream;

import forge.net.http.body;
import forge.net.http.route_context;
import forge.net.http.types;

export namespace forge::net::http {

struct stream_response {
   using send_guard_type = std::function<boost::asio::awaitable<void>(
       boost::asio::awaitable<void>, std::function<void()>)>;

   class body_source {
    public:
      using callback_type = std::function<boost::asio::awaitable<std::optional<body_chunk>>()>;

      body_source() = default;
      body_source(callback_type callback) : callback_(std::move(callback)) {}

      template <typename Callback>
         requires(!std::same_as<std::remove_cvref_t<Callback>, body_source>)
      body_source(Callback&& callback) : callback_(std::forward<Callback>(callback)) {}

      [[nodiscard]] explicit operator bool() const noexcept {
         return static_cast<bool>(callback_);
      }

      boost::asio::awaitable<std::optional<body_chunk>> operator()() {
         if (!callback_) {
            co_return std::nullopt;
         }
         co_return co_await callback_();
      }

      [[nodiscard]] body_source with_callback(callback_type callback) const;

    private:
      friend class streaming_response;
      friend struct stream_request;
      friend struct detail::stream_server_access;

      body_source(callback_type callback, std::shared_ptr<const void> request_body_marker)
          : callback_(std::move(callback)), request_body_marker_(std::move(request_body_marker)) {}

      callback_type callback_;
      std::shared_ptr<const void> request_body_marker_;
   };

   response head;
   body_source body;
   // Wraps the complete send. The callback aborts socket I/O; the guard must join the operation.
   send_guard_type send_guard;

   [[nodiscard]] static stream_response buffered(response response_value) {
      return stream_response{.head = std::move(response_value), .body = {}};
   }
};

struct stream_request {
   route_context& context;
   body_reader body;

   stream_request(route_context& context_value, body_reader body_value)
       : context(context_value), body(std::move(body_value)) {}

   [[nodiscard]] stream_response::body_source
   response_body(stream_response::body_source::callback_type callback) const {
      return stream_response::body_source{std::move(callback), request_body_marker_};
   }

 private:
   friend struct detail::stream_server_access;

   stream_request(route_context& context_value, body_reader body_value, std::shared_ptr<const void> request_body_marker)
       : context(context_value), body(std::move(body_value)), request_body_marker_(std::move(request_body_marker)) {}

   std::shared_ptr<const void> request_body_marker_;
};

struct streaming_response_options {
   std::string content_type = "application/octet-stream";
   stream_response::body_source body;
};

class streaming_response {
 public:
   streaming_response() = default;

   [[nodiscard]] static streaming_response from_source(streaming_response_options options);
   [[nodiscard]] static streaming_response from_body(response head, body_reader body);

   [[nodiscard]] status status_code() const noexcept;
   [[nodiscard]] const response& head() const noexcept;
   [[nodiscard]] std::string content_type() const;
   [[nodiscard]] body_reader& body() noexcept;

   [[nodiscard]] stream_response materialize(const request& request_value, status success_status) &&;
   [[nodiscard]] stream_response materialize(const stream_request& request_value, status success_status) &&;

 private:
   [[nodiscard]] stream_response materialize_impl(const request& request_value, status success_status) &&;

   response head_{status::ok, 11};
   stream_response::body_source source_;
   body_reader reader_;
};

using stream_route_handler = std::function<boost::asio::awaitable<stream_response>(stream_request&)>;

} // namespace forge::net::http
