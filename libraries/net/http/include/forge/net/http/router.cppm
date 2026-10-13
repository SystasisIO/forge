module;

#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

namespace forge::net::http::detail {
struct router_server_access;
}

export module forge.net.http.router;

export import forge.asio.compute;

import forge.net.http.middleware;
import forge.net.http.assets;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.http.target;
import forge.net.http.types;
import forge.net.websocket.connection;

export namespace forge::net::http {

using websocket_route_handler = std::function<void(std::shared_ptr<forge::net::websocket::connection>)>;

struct websocket_upgrade_result {
   websocket_route_handler handler;
   std::optional<response> rejection;
};

// Runs before the handshake and may reject it. The accepted callback captures
// verified application context without exposing credentials to a connection.
using websocket_upgrade_handler = std::function<boost::asio::awaitable<websocket_upgrade_result>(route_context&)>;

class router;

class router {
 public:
   void use(middleware handler);
   void use(middleware_descriptor descriptor);

   void get(std::string path, route_handler handler);
   void head(std::string path, route_handler handler);
   void post(std::string path, route_handler handler);
   void put(std::string path, route_handler handler);
   void patch(std::string path, route_handler handler);
   void del(std::string path, route_handler handler);
   void get_stream(std::string path, stream_route_handler handler);
   void head_stream(std::string path, stream_route_handler handler);
   void post_stream(std::string path, stream_route_handler handler);
   void put_stream(std::string path, stream_route_handler handler);
   void patch_stream(std::string path, stream_route_handler handler);
   void del_stream(std::string path, stream_route_handler handler);
   void websocket(std::string path, websocket_route_handler handler);
   void websocket_guarded(std::string path, websocket_upgrade_handler handler);
   void mount_assets(asset_mount value, forge::asio::compute::executor read_executor);
   void mount_assets(asset_bundle value);
   void reserve_path_prefix(std::string path);

   template <typename Binding> void mount(const Binding& binding) {
      binding.mount(*this);
   }

   [[nodiscard]] boost::asio::awaitable<response> handle(route_context& context) const;
   [[nodiscard]] bool can_handle_stream(route_context& context) const;
   [[nodiscard]] boost::asio::awaitable<stream_response> handle_stream(stream_request& request) const;
   [[nodiscard]] std::optional<websocket_route_handler> match_websocket(route_context& context) const;
   [[nodiscard]] std::optional<websocket_upgrade_handler> match_websocket_guarded(route_context& context) const;

 private:
   friend struct detail::router_server_access;

   struct route_entry {
      method verb;
      std::string path;
      std::vector<std::string> segments;
      bool parameterized = false;
      route_handler handler;
   };

   struct websocket_route_entry {
      std::string path;
      std::vector<std::string> segments;
      bool parameterized = false;
      websocket_route_handler handler;
   };

   struct stream_route_entry {
      method verb;
      std::string path;
      std::vector<std::string> segments;
      bool parameterized = false;
      stream_route_handler handler;
   };

   struct websocket_guarded_route_entry {
      std::string path;
      std::vector<std::string> segments;
      bool parameterized = false;
      websocket_upgrade_handler handler;
   };

   struct middleware_entry {
      middleware_descriptor descriptor;
      std::vector<std::string> path_segments;
      bool trailing_slash = false;
   };

   void add_route(method verb, std::string path, route_handler handler);
   void add_stream_route(method verb, std::string path, stream_route_handler handler);

   std::vector<route_entry> routes_;
   std::vector<websocket_route_entry> websocket_routes_;
   std::vector<websocket_guarded_route_entry> websocket_guarded_routes_;
   std::vector<stream_route_entry> stream_routes_;
   std::vector<asset_bundle> asset_mounts_;
   std::vector<std::string> reserved_path_prefixes_;
   std::vector<middleware_entry> middlewares_;
   std::uint64_t anonymous_middleware_id_ = 0;
};

} // namespace forge::net::http
