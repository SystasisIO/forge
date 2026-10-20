#pragma once

#include <boost/beast/http.hpp>

namespace forge::net::http {

enum class method;
enum class status : unsigned;
class request;
class response;

namespace detail {

boost::beast::http::verb to_beast_method(method value) noexcept;
method to_http_method(boost::beast::http::verb value) noexcept;
status to_http_status(boost::beast::http::status value) noexcept;
boost::beast::http::request<boost::beast::http::string_body> to_beast_request(const request& source);
request to_http_request(boost::beast::http::request<boost::beast::http::string_body>&& source);
response to_http_response(boost::beast::http::response<boost::beast::http::string_body>&& source);

} // namespace detail
} // namespace forge::net::http
