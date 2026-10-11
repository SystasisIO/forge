import forge.plugins.log.otlp.plugin;

int main() {
   const auto descriptor = forge::plugins::log::otlp::plugin::descriptor();
   return descriptor.id.value == "forge.plugins.log.otlp" ? 0 : 1;
}
