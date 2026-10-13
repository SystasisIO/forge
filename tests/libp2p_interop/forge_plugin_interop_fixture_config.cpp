#include "forge_plugin_interop_fixture.hxx"
#include "libp2p_identity.hxx"

namespace forge::test::libp2p_interop {
namespace {
using value = forge::config::core::value;
using object = forge::mutable_variant_object;

value secret(std::string id, std::string material, std::string purpose, std::string encoding = "raw") {
   return value::object_type{{"id", std::move(id)}, {"kind", "bytes"},
       {"source", value::object_type{{"type", "value"}, {"encoding", std::move(encoding)}, {"value", std::move(material)}}},
       {"purposes", value::array_type{value{std::move(purpose)}}},
       {"operations", value::array_type{value{"get_bytes"}}}, {"allow-raw-export", true}};
}

std::string psk_hex(const std::filesystem::path& path) {
   if (std::filesystem::file_size(path) > 128) { throw std::runtime_error{"oversized private fixture input"}; }
   auto input = std::ifstream{path, std::ios::binary};
   auto text = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
   const auto prefix = std::string_view{"/key/swarm/psk/1.0.0/\n/base16/\n"};
   if (!text.starts_with(prefix)) { throw std::runtime_error{"private fixture requires canonical swarm key"}; }
   text.erase(0, prefix.size());
   if (text.ends_with('\n')) { text.pop_back(); }
   const auto decoded = forge::codec::hex::decode(text);
   if (decoded.size() != 32) { throw std::runtime_error{"private fixture requires 32 decoded bytes"}; }
   return forge::codec::hex::encode(decoded);
}
} // namespace

forge::config::core::document forge_plugin_interop_fixture::configuration() {
   const auto scenario = argument("scenario");
   if (scenario.starts_with("dht_") && !_dht_value) {
      throw std::runtime_error{"unsupported configured DHT service scenario"};
   }
   if (_dht_value) {
      const auto mode = argument("payload");
      if ((!mode.empty() || argument("command") == "dial") && mode != "put_only" && mode != "get_only") {
         throw std::runtime_error{"DHT value service requires put_only or get_only; no combined self-query"};
      }
      if (!argument("pnet-control").empty()) {
         throw std::runtime_error{"DHT value service cannot run a private rejection control"};
      }
   }
   const auto transport = argument("transport");
   const auto private_transport = transport == "tcp-pnet-noise" || transport == "tcp-pnet-tls";
   const auto quic = transport == "quic";
   if (!quic && transport != "tcp" && transport != "tcp-tls" && !private_transport) {
      throw std::runtime_error{"unsupported configured plugin transport"};
   }
   const auto backend = argument("store-backend");
   auto available = false;
#if FORGE_HAS_MDBX
   available = available || backend == "mdbx";
#endif
#if FORGE_HAS_ROCKSDB
   available = available || backend == "rocksdb";
#endif
   if (!available) { throw std::runtime_error{"requested ObjectDB backend not built; no memory fallback"}; }
   const auto identity = generate_libp2p_identity();
   auto entries = value::array_type{
       secret("identity/certificate", identity.certificate_pem, "p2p.identity.certificate"),
       secret("identity/private-key", identity.private_key_pem, "p2p.identity.private-key")};
   auto config = forge::config::core::document{};
   config.set("plugins.db.store.stores", value::array_type{value::object_type{
       {"name", "peers"}, {"driver", backend}, {"path", argument("store-dir")},
       {"object", value::object_type{{"family", "objectdb"}, {"write-policy", "single-writer"}}}}});
   const auto root = std::string{"plugins.net.p2p.node."};
   config.set(root + "peer-store.store", "peers");
   config.set(root + "identity.certificate-secret", "identity/certificate");
   config.set(root + "identity.private-key-secret", "identity/private-key");
   config.set(root + "allow-insecure-test-mode", false);
   config.set(root + "listen", value::array_type{value{quic ? "/ip4/127.0.0.1/udp/0/quic-v1" : "/ip4/127.0.0.1/tcp/0"}});
   config.set(root + "topology.mode", "static-only");
   config.set(root + "tcp.security", transport == "tcp-tls" || transport == "tcp-pnet-tls" ? "tls" : "noise");
   config.set(root + "path.policy", "direct-only");
   config.set(root + "relay.client-enabled", false);
   config.set(root + "relay.server-enabled", false);
   config.set(root + "relay.auto-discovery-enabled", false);
   if (_dht_value) {
      config.set(root + "dht.profiles", value::array_type{value::object_type{
          {"kind", "amino-v1"}, {"mode", "server"}}});
   }
   const auto key = argument("pnet-key-file");
   if (!key.empty()) {
      if (!private_transport) { throw std::runtime_error{"PSK requires explicit private transport"}; }
      const auto hex = psk_hex(key);
      const auto decoded = forge::codec::hex::decode(hex);
      _key_fingerprint = forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{decoded}).str();
      entries.emplace_back(secret("network/psk", hex, "p2p.private-network.psk", "hex"));
      config.set(root + "private-network.psk-secret", "network/psk");
      config.set(root + "private-network.internet-egress", "deny-external");
   } else if (private_transport && argument("pnet-control") != "missing_key") {
      throw std::runtime_error{"private positive or mismatched-key case requires a key"};
   }
   const auto nameserver = argument("dns-server");
   if (!nameserver.empty()) {
      const auto split = nameserver.rfind(':');
      if (split == std::string::npos || nameserver.substr(0, split) != "127.0.0.1") {
         throw std::runtime_error{"DNS fixture must use an owned loopback nameserver"};
      }
      auto consumed = std::size_t{};
      const auto port = std::stoull(nameserver.substr(split + 1), &consumed);
      if (consumed != nameserver.size() - split - 1 || port == 0 || port > 65535) {
         throw std::runtime_error{"invalid DNS fixture port"};
      }
      config.set(root + "dns.resolver.nameservers", value::array_type{value::object_type{
          {"address", "127.0.0.1"}, {"port", static_cast<std::uint64_t>(port)}}});
      config.set(root + "dns.resolution.max-dns-lookups", std::uint64_t{2});
   }
   if (scenario == "pubsub_exchange") { configure_pubsub_document(config); }
   if (scenario == "autorelay") { configure_relay_document(config); }
   if (scenario == "autonat_v1" || scenario == "autonat_v2") { configure_autonat_document(config); }
   // The document is marked accepted only after official configure and startup complete.
   auto redacted = config;
   const auto encoded = forge::codec::json::write_document(redacted);
   if (!encoded.ok()) { throw std::runtime_error{"redacted configuration encoding failed"}; }
   auto parsed = forge::codec::json::read_value(encoded.text);
   if (!parsed.ok()) { throw std::runtime_error{"redacted configuration conversion failed"}; }
   _settings = object{}("source", "official-shell.configuration-document")("document", parsed.value)
       ("identity_source", "Secrets.get_bytes")("identity_key_type", "ed25519")
       ("psk_source", key.empty() ? "absent" : "Secrets.get_bytes")
       ("secret_material", "redacted")("backend_fallback", false);
   config.set("plugins.crypto.secrets.secrets", std::move(entries));
   return config;
}

} // namespace forge::test::libp2p_interop
