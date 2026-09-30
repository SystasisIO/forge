import forge.net.http.assets;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.http.server.api;

int main() {
   const auto descriptor = forge::plugins::net::http::server::descriptor();
   const auto mount = forge::net::http::asset_mount{.path = "/admin"};
   return descriptor.id.value == "forge.plugins.net.http.server" && forge::plugins::net::http::server::api::ref().major == 2U &&
                  mount.path == "/admin"
              ? 0
              : 1;
}
