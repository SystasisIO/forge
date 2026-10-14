#include <string>
#include <string_view>

import forge.asio.compute;
import forge.asio.runtime;
import forge.net.http.assets;
import forge.net.http.cookie;
import forge.net.http.server;
import forge.net.http.router;
import forge.net.http.types;

int main() {
   const auto server_options = forge::net::http::server_config{.access_log = true};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto server = forge::net::http::server{runtime, server_options, forge::net::http::router{}};
   server.start();
   const auto listening = server.port() != 0;
   server.stop();
   auto file_reads = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   const auto formatted = forge::net::http::format_set_cookie({.name = "session", .value = "value"});
   const auto bundle = forge::net::http::asset_bundle{forge::net::http::asset_mount{.path = "/admin", .root = "."},
                                                      file_reads.get_executor()};
   return formatted == "session=value" && bundle.path() == std::string_view{"/admin"} && listening ? 0 : 1;
}
