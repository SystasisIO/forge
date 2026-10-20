module;

#include <cstdint>
#include <string>
#include <string_view>

export module forge.net.http.base_url;

export namespace forge::net::http {

struct base_url {
   std::string original;
   std::string scheme;
   std::string host;
   std::string port;
   std::string base_path = "/";

   [[nodiscard]] bool secure() const;
   [[nodiscard]] std::string origin() const;
   [[nodiscard]] std::string host_header() const;
   [[nodiscard]] std::string make_target(std::string_view path) const;
};

base_url parse_base_url(std::string_view value);

} // namespace forge::net::http
