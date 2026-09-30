module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module forge.plugins.net.http.server.middleware;

import forge.net.http.middleware;
import forge.net.http.types;

export namespace forge::plugins::net::http::server {

namespace http = forge::net::http;

namespace detail {
struct middleware_bridge_access;
}

enum class middleware_phase {
   request_context = 1,
   security = 2,
   limits = 3,
   before_handler = 4,
   after_handler = 5,
   error = 6,
};

struct header_entry {
   std::string name;
   std::string value;
};

struct middleware_request {
   std::string method;
   std::string target;
   std::string path;
   std::vector<header_entry> headers;

   [[nodiscard]] std::optional<std::string_view> header(std::string_view name) const noexcept {
      for (const auto& entry : headers) {
         if (http::header_name_equal(entry.name, name)) {
            return std::string_view{entry.value};
         }
      }
      return std::nullopt;
   }
};

class middleware_response {
 public:
   [[nodiscard]] static middleware_response text(forge::net::http::status status_value, std::string body_value,
                                                 std::string content_type_value = "text/plain") {
      auto result = middleware_response{};
      result.status_ = status_value;
      result.body_ = std::move(body_value);
      result.content_type_ = std::move(content_type_value);
      return result;
   }

   [[nodiscard]] forge::net::http::status status() const noexcept {
      return status_;
   }

   [[nodiscard]] const std::vector<header_entry>& headers() const noexcept {
      return headers_;
   }

   [[nodiscard]] const std::string& body() const noexcept {
      return body_;
   }

   [[nodiscard]] const std::string& content_type() const noexcept {
      static const auto empty = std::string{};
      return content_type_.has_value() ? *content_type_ : empty;
   }

   void set_header(std::string name, std::string value) {
      for (auto& entry : headers_) {
         if (http::header_name_equal(entry.name, name)) {
            entry.value = std::move(value);
            return;
         }
      }
      headers_.push_back(header_entry{.name = std::move(name), .value = std::move(value)});
   }

   void append_header(std::string name, std::string value) {
      headers_.push_back(header_entry{.name = std::move(name), .value = std::move(value)});
   }

   void set_status(forge::net::http::status value) noexcept {
      status_ = value;
      stream_state_.clear();
   }

   void set_body(std::string value) {
      body_ = std::move(value);
      stream_state_.clear();
   }

   void clear_body() {
      body_.clear();
      stream_state_.clear();
   }

   void set_content_type(std::string value) {
      content_type_ = std::move(value);
   }

 private:
   forge::net::http::status status_ = forge::net::http::status::ok;
   std::vector<header_entry> headers_;
   std::string body_;
   std::optional<std::string> content_type_;
   forge::net::http::stream_pass_through_state stream_state_;

   friend struct detail::middleware_bridge_access;
};

namespace detail {

struct middleware_bridge_access {
   static void set_status(middleware_response& value, forge::net::http::status status) noexcept {
      value.status_ = status;
   }

   static void set_body(middleware_response& value, std::string body) {
      value.body_ = std::move(body);
   }

   [[nodiscard]] static std::string take_body(middleware_response& value) {
      return std::move(value.body_);
   }

   static void set_content_type(middleware_response& value, std::string content_type) {
      value.content_type_ = std::move(content_type);
   }

   [[nodiscard]] static const std::optional<std::string>& content_type(const middleware_response& value) noexcept {
      return value.content_type_;
   }

   static std::vector<header_entry>& headers(middleware_response& value) noexcept {
      return value.headers_;
   }

   static void set_stream_state(middleware_response& value, forge::net::http::stream_pass_through_state state) {
      value.stream_state_ = std::move(state);
   }

   [[nodiscard]] static const forge::net::http::stream_pass_through_state&
   stream_state(const middleware_response& value) noexcept {
      return value.stream_state_;
   }
};

} // namespace detail

using middleware_next = std::function<boost::asio::awaitable<middleware_response>()>;
using middleware_handler =
    std::function<boost::asio::awaitable<middleware_response>(const middleware_request&, middleware_next)>;

struct middleware_descriptor {
   std::string id;
   middleware_phase phase = middleware_phase::before_handler;
   int order = 0;
   std::string path_prefix = "/";
   middleware_handler handler;
};

} // namespace forge::plugins::net::http::server
