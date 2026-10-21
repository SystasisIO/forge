module;

#include <string>
#include <string_view>

#include <forge/exceptions/macros.hpp>

#include <boost/url.hpp>

module forge.net.http.base_url;

import forge.net.http.exceptions;

namespace forge::net::http {

bool base_url::secure() const {
   return scheme == "https" || scheme == "wss";
}

std::string base_url::origin() const {
   return scheme + "://" + host + ":" + port;
}

std::string base_url::host_header() const {
   if (port.empty() || port == (secure() ? "443" : "80")) {
      return host;
   }
   return host + ":" + port;
}

std::string base_url::make_target(std::string_view path) const {
   auto normalized = std::string{path};
   if (normalized.empty()) {
      normalized = "/";
   } else if (normalized.front() != '/') {
      normalized.insert(normalized.begin(), '/');
   }

   auto target = base_path;
   if (target.empty()) {
      target = "/";
   }

   if (target.back() == '/' && normalized.front() == '/') {
      target.pop_back();
   }
   if (target.empty()) {
      target = "/";
   }
   if (target == "/") {
      return normalized;
   }
   return target + normalized;
}

base_url parse_base_url(std::string_view value) {
   const auto parsed = boost::urls::parse_uri(value);
   if (!parsed.has_value()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "invalid HTTP base URL");
   }

   const auto& uri = parsed.value();
   auto result = base_url{
       .original = std::string{value},
       .scheme = std::string{uri.scheme()},
       .host = std::string{uri.host()},
       .port = std::string{uri.port()},
       .base_path = std::string{uri.encoded_path()},
   };

   if (result.scheme != "http" && result.scheme != "https" && result.scheme != "ws" && result.scheme != "wss") {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "unsupported HTTP base URL scheme");
   }
   if (result.host.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP base URL host must not be empty");
   }
   if (result.port.empty()) {
      result.port = result.secure() ? "443" : "80";
   }
   if (result.base_path.empty()) {
      result.base_path = "/";
   }

   return result;
}

} // namespace forge::net::http
