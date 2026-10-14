#include <string>
#include <string_view>

import forge.asio.compute;
import forge.net.http.assets;
import forge.net.http.cookie;
import forge.net.http.server;
import forge.net.http.types;

int main() {
   const auto server_options = forge::net::http::server_config{.access_log = true};
   auto file_reads = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   const auto formatted = forge::net::http::format_set_cookie({.name = "session", .value = "value"});
   const auto bundle = forge::net::http::asset_bundle{forge::net::http::asset_mount{.path = "/admin", .root = "."},
                                                      file_reads.get_executor()};
   return formatted == "session=value" && bundle.path() == std::string_view{"/admin"} && server_options.access_log ? 0 : 1;
}
