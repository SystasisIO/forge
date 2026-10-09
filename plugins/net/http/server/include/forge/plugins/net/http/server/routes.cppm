module;

#include <functional>
#include <string>
#include <vector>

export module forge.plugins.net.http.server.routes;

export import forge.net.http.router;

export namespace forge::plugins::net::http::server {

// Trusted application registration, closed before the listener starts. Handlers
// use the same router, streaming substrate and lifecycle as typed bindings.
struct route_mount {
   std::string id;
   std::vector<std::string> reserved_paths;
   std::function<void(forge::net::http::router&)> apply;
};

} // namespace forge::plugins::net::http::server
