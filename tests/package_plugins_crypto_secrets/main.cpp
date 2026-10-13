import forge.plugins.crypto.secrets.plugin;

int main() {
   const auto descriptor = forge::plugins::crypto::secrets::plugin::descriptor();
   return descriptor.id.value == "forge.plugins.crypto.secrets" ? 0 : 1;
}
