#include <concepts>
#include <memory>
#include <string>
#include <vector>

import forge.app.plugin;
import forge.app.plugin_registry;
import forge.plugins.crypto.secrets.plugin;
import forge.plugins.crypto.wallet.plugin;
import forge.plugins.chain.signer.plugin;
import forge.plugins.db.store.plugin;
import forge.plugins.log.otlp.plugin;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.p2p.node.plugin;
import forge.plugins.net.p2p.resolver.plugin;

template <typename Plugin>
bool matches(const std::string& id, const std::vector<forge::app::plugin_id>& dependencies = {},
             bool enabled_by_default = true) {
   static_assert(std::same_as<decltype(Plugin::descriptor()), forge::app::plugin_descriptor>);
   const auto value = Plugin::descriptor();
   if (value.id.value != id || value.dependencies != dependencies ||
       value.enabled_by_default != enabled_by_default || !value.factory) {
      return false;
   }
   const auto instance = value.factory();
   const auto second = value.factory();
   return instance && second && instance.get() != second.get() &&
          instance->id() == value.id && second->id() == value.id &&
          !instance->version().empty() && instance->version() == second->version();
}

int main() {
   return matches<forge::plugins::crypto::secrets::plugin>("forge.plugins.crypto.secrets") &&
          matches<forge::plugins::crypto::wallet::plugin>("forge.plugins.crypto.wallet", {}, false) &&
          matches<forge::plugins::chain::signer::plugin>("forge.plugins.chain.signer", {}, false) &&
          matches<forge::plugins::db::store::plugin>("forge.plugins.db.store") &&
          matches<forge::plugins::log::otlp::plugin>("forge.plugins.log.otlp") &&
          matches<forge::plugins::net::http::server::plugin>("forge.plugins.net.http.server") &&
          matches<forge::plugins::net::p2p::node::plugin>(
              "forge.plugins.net.p2p.node",
              {{.value = "forge.plugins.db.store"}, {.value = "forge.plugins.crypto.secrets"}}) &&
          matches<forge::plugins::net::p2p::resolver::plugin>(
              "forge.plugins.net.p2p.resolver", {{.value = "forge.plugins.net.p2p.node"}})
              ? 0 : 1;
}
