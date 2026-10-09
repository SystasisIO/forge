module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

module forge.plugins.net.http.server.api;

import forge.plugins.net.http.server.exceptions;

namespace forge::plugins::net::http::server {

boost::asio::awaitable<void> api::mount_routes(route_mount)
{
   FORGE_THROW_EXCEPTION(exceptions::unsupported_route_mount, "HTTP route mounts are not supported by this implementation");
   co_return;
}

} // namespace forge::plugins::net::http::server
