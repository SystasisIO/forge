#pragma once

#include <boost/beast/http.hpp>

namespace forge::net::http::detail {

template <bool IsRequest>
class complete_parser final : public boost::beast::http::parser<IsRequest, boost::beast::http::string_body> {
 private:
   void on_trailer_field_impl(boost::beast::http::field, boost::beast::string_view, boost::beast::string_view,
                              boost::system::error_code& error) override {
      // Beast may intentionally discard undeclared or unapproved trailers.
      // Reject the parsed event itself, before that normalization can hide it.
      error = boost::beast::http::error::bad_field;
   }
};

} // namespace forge::net::http::detail
