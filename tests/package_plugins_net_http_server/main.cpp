import forge.net.http.assets;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.types;

int main() {
   const auto descriptor = forge::plugins::net::http::server::plugin::descriptor();
   const auto mount = forge::net::http::asset_mount{.path = "/admin"};
   const auto config = forge::plugins::net::http::server::config{.access_log = true};
   const auto publication = forge::plugins::net::http::server::publish_options{.invocation = {}};
   return descriptor.id.value == "forge.plugins.net.http.server" &&
                  forge::plugins::net::http::server::api::ref().major == 2U && mount.path == "/admin" &&
                  config.access_log && !publication.invocation
              ? 0
              : 1;
}
