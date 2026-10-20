#include "details/beast_message.hxx"

#include <string>
#include <string_view>
#include <utility>

import forge.net.http.types;

namespace forge::net::http::detail {

boost::beast::http::verb to_beast_method(method value) noexcept {
   switch (value) {
   case method::delete_:
      return boost::beast::http::verb::delete_;
   case method::get:
      return boost::beast::http::verb::get;
   case method::head:
      return boost::beast::http::verb::head;
   case method::options:
      return boost::beast::http::verb::options;
   case method::patch:
      return boost::beast::http::verb::patch;
   case method::post:
      return boost::beast::http::verb::post;
   case method::put:
      return boost::beast::http::verb::put;
   case method::unknown:
      return boost::beast::http::verb::unknown;
   }
   return boost::beast::http::verb::unknown;
}

method to_http_method(boost::beast::http::verb value) noexcept {
   switch (value) {
   case boost::beast::http::verb::delete_:
      return method::delete_;
   case boost::beast::http::verb::get:
      return method::get;
   case boost::beast::http::verb::head:
      return method::head;
   case boost::beast::http::verb::options:
      return method::options;
   case boost::beast::http::verb::patch:
      return method::patch;
   case boost::beast::http::verb::post:
      return method::post;
   case boost::beast::http::verb::put:
      return method::put;
   default:
      return method::unknown;
   }
}

status to_http_status(boost::beast::http::status value) noexcept {
   return static_cast<status>(static_cast<unsigned>(value));
}

boost::beast::http::request<boost::beast::http::string_body> to_beast_request(const request& source) {
   auto target = boost::beast::http::request<boost::beast::http::string_body>{
       to_beast_method(source.method()), std::string{source.target()}, source.version()};
   for (const auto& header : source.header_fields()) {
      target.insert(header.name, header.text);
   }
   target.keep_alive(source.keep_alive());
   target.body() = source.body();
   target.prepare_payload();
   return target;
}

request to_http_request(boost::beast::http::request<boost::beast::http::string_body>&& source) {
   auto target = request{to_http_method(source.method()), std::string{source.target().data(), source.target().size()},
                         source.version()};
   target.keep_alive(source.keep_alive());
   for (const auto& header : source) {
      target.insert(header.name_string(), header.value());
   }
   target.body() = std::move(source.body());
   return target;
}

response to_http_response(boost::beast::http::response<boost::beast::http::string_body>&& source) {
   auto target = response{to_http_status(source.result()), source.version()};
   target.keep_alive(source.keep_alive());
   for (const auto& header : source) {
      target.insert(header.name_string(), header.value());
   }
   target.body() = std::move(source.body());
   return target;
}

} // namespace forge::net::http::detail
