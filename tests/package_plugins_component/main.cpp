import forge.plugins.net.p2p.node.plugin;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.log.otlp.plugin;
import forge.plugins.db.store.plugin;

int main() {
   const auto p2p = forge::plugins::net::p2p::node::descriptor();
   const auto secrets = forge::plugins::crypto::secrets::descriptor();
   const auto log_otlp = forge::plugins::log::otlp::descriptor();
   const auto object = forge::plugins::db::store::descriptor();
   return p2p.id.value == "forge.plugins.net.p2p.node" &&
                secrets.id.value == "forge.plugins.crypto.secrets" &&
                log_otlp.id.value == "forge.plugins.log.otlp" &&
                object.id.value == "forge.plugins.db.store"
             ? 0
             : 1;
}
