#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "forge_interop_build_info.hxx"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/objects.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.crypto.pki.pem;
import forge.net.dns.types;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.multiformats.exceptions;
import forge.multiformats.multihash;
import forge.multiformats.multiaddr;
import forge.multiformats.types;
import forge.multiformats.varint;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.rendezvous;
import forge.net.p2p.relay;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.pnet.protector;

#include "forge_autonat_fixture.hxx"
#include "forge_connection_fixture.hxx"

namespace {

using namespace std::chrono_literals;

constexpr auto echo_protocol = std::string_view{"/forge/interop/relay-echo/1"};
constexpr auto maximum_echo_payload = std::size_t{256 * 1024};
constexpr auto large_echo_payload_size = std::size_t{192 * 1024};
constexpr auto rendezvous_namespace = std::string_view{"forge.discovery"};
constexpr auto pubsub_topic = std::string_view{"forge.pubsub.interop"};
constexpr auto pubsub_payload = std::string_view{"forge-gossipsub-live"};
constexpr auto dht_wire_timeout = 5s;

struct dht_value_fixture {
   forge::net::p2p::dht::key key;
   std::vector<std::uint8_t> value;
};

[[nodiscard]] bool is_dht_value_scenario(std::string_view scenario) noexcept {
   return scenario == "dht_pk_put_get" || scenario == "dht_ipns_put_get";
}

[[nodiscard]] dht_value_fixture value_fixture(std::string_view scenario) {
   constexpr auto identity_multihash =
       std::string_view{"00240801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"};
   if (scenario == "dht_pk_put_get") {
      return {
          .key = {.bytes = forge::codec::hex::decode(std::string{"2f706b2f"} + std::string{identity_multihash})},
          .value =
              forge::codec::hex::decode("0801122079b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664"),
      };
   }
   if (scenario == "dht_ipns_put_get") {
      return {
          .key = {.bytes = forge::codec::hex::decode(std::string{"2f69706e732f"} + std::string{identity_multihash})},
          .value = forge::codec::hex::decode(
              "0a1f2f697066732f6261666b716163336a6f627868676964736e3572777734796b1240b7be19b36e1955d2e1ccddd889d25c"
              "4eaef61aa72763bc44db9696697be7587e35d2efb2a625e7ac19b05f8c348086114103ee042a5a4041683e39c4ac0c460118"
              "00221e323033302d30312d30325430333a30343a30352e3132333435363738395a28073080f092cbdd0842408904024a1b09"
              "b52636334f17b9098f648f9a00214e6c6c89bb954c01300b00f54d085ddcacbe42952f2f819d70a48ff453d13329bb775d66"
              "e5a4b6165c38a40a4a76a56354544c1b00000045d964b8006556616c7565581f2f697066732f6261666b716163336a6f6278"
              "68676964736e3572777734796b6853657175656e6365076856616c6964697479581e323033302d30312d30325430333a30343a"
              "30352e3132333435363738395a6c56616c69646974795479706500"),
      };
   }
   throw std::runtime_error{"unknown DHT value fixture: " + std::string{scenario}};
}

struct bio_deleter {
   void operator()(BIO* value) const noexcept {
      BIO_free(value);
   }
};

struct evp_md_ctx_deleter {
   void operator()(EVP_MD_CTX* value) const noexcept {
      EVP_MD_CTX_free(value);
   }
};

struct evp_pkey_deleter {
   void operator()(EVP_PKEY* value) const noexcept {
      EVP_PKEY_free(value);
   }
};

struct asn1_object_deleter {
   void operator()(ASN1_OBJECT* value) const noexcept {
      ASN1_OBJECT_free(value);
   }
};

struct asn1_octet_string_deleter {
   void operator()(ASN1_OCTET_STRING* value) const noexcept {
      ASN1_OCTET_STRING_free(value);
   }
};

struct x509_deleter {
   void operator()(X509* value) const noexcept {
      X509_free(value);
   }
};

struct x509_extension_deleter {
   void operator()(X509_EXTENSION* value) const noexcept {
      X509_EXTENSION_free(value);
   }
};

struct libp2p_identity {
   std::string certificate_pem;
   std::string private_key_pem;
   std::vector<std::uint8_t> public_key;
   forge::net::p2p::peer_id peer;
};

std::string json_escape(std::string_view value) {
   auto out = std::string{};
   out.reserve(value.size() + 8);
   for (const auto ch : value) {
      switch (ch) {
      case '\\':
         out += "\\\\";
         break;
      case '"':
         out += "\\\"";
         break;
      case '\n':
         out += "\\n";
         break;
      case '\r':
         out += "\\r";
         break;
      case '\t':
         out += "\\t";
         break;
      default:
         out.push_back(ch);
         break;
      }
   }
   return out;
}

std::map<std::string, std::string> parse_args(int argc, char** argv) {
   auto out = std::map<std::string, std::string>{};
   if (argc < 2) {
      throw std::runtime_error{"missing command"};
   }
   out["command"] = argv[1];
   for (auto i = 2; i < argc; ++i) {
      auto key = std::string{argv[i]};
      if (!key.starts_with("--")) {
         throw std::runtime_error{"unexpected positional argument: " + key};
      }
      if (i + 1 >= argc) {
         throw std::runtime_error{"missing value for " + key};
      }
      out[key.substr(2)] = argv[++i];
   }
   return out;
}

const std::string& required(const std::map<std::string, std::string>& args, std::string_view key) {
   const auto it = args.find(std::string{key});
   if (it == args.end() || it->second.empty()) {
      throw std::runtime_error{"missing required argument --" + std::string{key}};
   }
   return it->second;
}

std::string optional_value(const std::map<std::string, std::string>& args, std::string_view key,
                           std::string_view fallback = {}) {
   const auto it = args.find(std::string{key});
   if (it == args.end() || it->second.empty()) {
      return std::string{fallback};
   }
   return it->second;
}

std::vector<std::string> read_lines(const std::filesystem::path& path) {
   auto in = std::ifstream{path};
   if (!in) {
      throw std::runtime_error{"failed to open " + path.string()};
   }
   auto out = std::vector<std::string>{};
   for (auto line = std::string{}; std::getline(in, line);) {
      if (!line.empty()) {
         out.push_back(line);
      }
   }
   return out;
}

void write_file(const std::filesystem::path& path, std::string_view value) {
   std::filesystem::create_directories(path.parent_path());
   auto out = std::ofstream{path};
   if (!out) {
      throw std::runtime_error{"failed to open " + path.string()};
   }
   out << value;
}

void require_openssl(bool ok, std::string_view message) {
   if (!ok) {
      throw std::runtime_error{std::string{message}};
   }
}

std::string bio_to_string(BIO* bio) {
   BUF_MEM* memory = nullptr;
   BIO_get_mem_ptr(bio, &memory);
   if (memory == nullptr) {
      throw std::runtime_error{"failed to read OpenSSL memory BIO"};
   }
   return std::string{memory->data, memory->length};
}

std::vector<std::uint8_t> public_key_spki_der(EVP_PKEY* key) {
   const auto length = i2d_PUBKEY(key, nullptr);
   require_openssl(length > 0, "failed to DER-encode libp2p certificate public key");
   auto out = std::vector<std::uint8_t>(static_cast<std::size_t>(length));
   auto* cursor = out.data();
   require_openssl(i2d_PUBKEY(key, &cursor) == length, "failed to DER-write libp2p certificate public key");
   return out;
}

std::vector<std::uint8_t> raw_public_key(EVP_PKEY* key) {
   auto size = std::size_t{};
   require_openssl(EVP_PKEY_get_raw_public_key(key, nullptr, &size) == 1 && size != 0,
                   "failed to size libp2p identity public key");
   auto out = std::vector<std::uint8_t>(size);
   require_openssl(EVP_PKEY_get_raw_public_key(key, out.data(), &size) == 1,
                   "failed to read libp2p identity public key");
   out.resize(size);
   return out;
}

std::vector<std::uint8_t> identity_sign(EVP_PKEY* key, std::span<const std::uint8_t> message) {
   auto context = std::unique_ptr<EVP_MD_CTX, evp_md_ctx_deleter>{EVP_MD_CTX_new()};
   require_openssl(context != nullptr, "failed to allocate libp2p signing context");
   require_openssl(EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key) == 1,
                   "failed to initialize libp2p certificate extension signer");
   auto size = std::size_t{};
   require_openssl(EVP_DigestSign(context.get(), nullptr, &size, message.data(), message.size()) == 1,
                   "failed to size libp2p certificate extension signature");
   auto out = std::vector<std::uint8_t>(size);
   require_openssl(EVP_DigestSign(context.get(), out.data(), &size, message.data(), message.size()) == 1,
                   "failed to sign libp2p certificate extension");
   out.resize(size);
   return out;
}

void append_der_length(std::vector<std::uint8_t>& out, std::size_t value) {
   if (value < 128) {
      out.push_back(static_cast<std::uint8_t>(value));
      return;
   }
   auto bytes = std::vector<std::uint8_t>{};
   while (value != 0) {
      bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
      value >>= 8U;
   }
   out.push_back(static_cast<std::uint8_t>(0x80U | bytes.size()));
   for (auto it = bytes.rbegin(); it != bytes.rend(); ++it) {
      out.push_back(*it);
   }
}

void append_der_octet_string(std::vector<std::uint8_t>& out, std::span<const std::uint8_t> value) {
   out.push_back(0x04);
   append_der_length(out, value.size());
   out.insert(out.end(), value.begin(), value.end());
}

std::vector<std::uint8_t> signed_key_der(std::span<const std::uint8_t> public_key,
                                         std::span<const std::uint8_t> signature) {
   auto content = std::vector<std::uint8_t>{};
   append_der_octet_string(content, public_key);
   append_der_octet_string(content, signature);
   auto out = std::vector<std::uint8_t>{0x30};
   append_der_length(out, content.size());
   out.insert(out.end(), content.begin(), content.end());
   return out;
}

libp2p_identity generate_libp2p_identity() {
   auto key = std::unique_ptr<EVP_PKEY, evp_pkey_deleter>{EVP_PKEY_Q_keygen(nullptr, nullptr, "ED25519")};
   require_openssl(key != nullptr, "failed to generate libp2p Ed25519 identity key");

   const auto spki = public_key_spki_der(key.get());
   auto signed_message = std::vector<std::uint8_t>{};
   constexpr auto prefix = std::string_view{"libp2p-tls-handshake:"};
   signed_message.insert(signed_message.end(), prefix.begin(), prefix.end());
   signed_message.insert(signed_message.end(), spki.begin(), spki.end());
   const auto signature = identity_sign(key.get(), signed_message);
   const auto public_key = forge::net::p2p::encode_public_key(forge::net::p2p::public_key{
       .type = forge::net::p2p::public_key::type::ed25519,
       .data = raw_public_key(key.get()),
   });
   const auto extension_value = signed_key_der(public_key, signature);

   auto certificate = std::unique_ptr<X509, x509_deleter>{X509_new()};
   require_openssl(certificate != nullptr, "failed to allocate libp2p certificate");
   require_openssl(X509_set_version(certificate.get(), 2) == 1, "failed to set libp2p certificate version");
   require_openssl(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) == 1,
                   "failed to set libp2p certificate serial");
   require_openssl(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) != nullptr,
                   "failed to set libp2p certificate notBefore");
   require_openssl(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 100L * 365L * 24L * 60L * 60L) != nullptr,
                   "failed to set libp2p certificate notAfter");
   require_openssl(X509_set_pubkey(certificate.get(), key.get()) == 1, "failed to set libp2p certificate key");
   auto* name = X509_get_subject_name(certificate.get());
   require_openssl(name != nullptr, "failed to allocate libp2p certificate subject");
   const auto serial = std::string{"forge-libp2p-interop"};
   require_openssl(X509_NAME_add_entry_by_txt(name, "serialNumber", MBSTRING_ASC,
                                              reinterpret_cast<const unsigned char*>(serial.data()),
                                              static_cast<int>(serial.size()), -1, 0) == 1,
                   "failed to set libp2p certificate subject");
   require_openssl(X509_set_issuer_name(certificate.get(), name) == 1, "failed to set libp2p certificate issuer");

   auto object = std::unique_ptr<ASN1_OBJECT, asn1_object_deleter>{OBJ_txt2obj("1.3.6.1.4.1.53594.1.1", 1)};
   require_openssl(object != nullptr, "failed to create libp2p extension OID");
   auto octets = std::unique_ptr<ASN1_OCTET_STRING, asn1_octet_string_deleter>{ASN1_OCTET_STRING_new()};
   require_openssl(octets != nullptr, "failed to allocate libp2p extension value");
   require_openssl(
       ASN1_OCTET_STRING_set(octets.get(), extension_value.data(), static_cast<int>(extension_value.size())) == 1,
       "failed to set libp2p extension value");
   auto extension = std::unique_ptr<X509_EXTENSION, x509_extension_deleter>{
       X509_EXTENSION_create_by_OBJ(nullptr, object.get(), 1, octets.get())};
   require_openssl(extension != nullptr, "failed to create libp2p public key extension");
   require_openssl(X509_add_ext(certificate.get(), extension.get(), -1) == 1,
                   "failed to add libp2p public key extension");
   require_openssl(X509_sign(certificate.get(), key.get(), nullptr) > 0, "failed to sign libp2p certificate");

   auto certificate_bio = std::unique_ptr<BIO, bio_deleter>{BIO_new(BIO_s_mem())};
   auto private_key_bio = std::unique_ptr<BIO, bio_deleter>{BIO_new(BIO_s_mem())};
   require_openssl(certificate_bio != nullptr && private_key_bio != nullptr, "failed to allocate libp2p PEM BIO");
   require_openssl(PEM_write_bio_X509(certificate_bio.get(), certificate.get()) == 1,
                   "failed to write libp2p certificate PEM");
   require_openssl(PEM_write_bio_PrivateKey(private_key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) ==
                       1,
                   "failed to write libp2p private key PEM");

   return libp2p_identity{
       .certificate_pem = bio_to_string(certificate_bio.get()),
       .private_key_pem = bio_to_string(private_key_bio.get()),
       .public_key = public_key,
       .peer = forge::net::p2p::make_peer_id(forge::net::p2p::public_key{
           .type = forge::net::p2p::public_key::type::ed25519,
           .data = raw_public_key(key.get()),
       }),
   };
}

const libp2p_identity& local_identity() {
   static const auto identity = generate_libp2p_identity();
   return identity;
}

forge::net::p2p::node::options node_options(const std::filesystem::path& store_path, const libp2p_identity& identity) {
   (void)store_path;
   auto out = forge::net::p2p::node::options{
       .certificate_pem = identity.certificate_pem,
       .private_key_pem = identity.private_key_pem,
       .explicit_peer_id = identity.peer,
       .capabilities =
           forge::net::p2p::capability_set{
               .bits = forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::peer_exchange |
                       forge::net::p2p::capabilities::autonat | forge::net::p2p::capabilities::relay |
                       forge::net::p2p::capabilities::hole_punching | forge::net::p2p::capabilities::relay_reservation |
                       forge::net::p2p::capabilities::rendezvous | forge::net::p2p::capabilities::pubsub},
       .public_key = identity.public_key,
       .peer_state =
           forge::net::p2p::peer_store::options{
               .persistence = forge::net::p2p::peer_store::make_memory_persistence(),
           },
       .allow_insecure_test_mode = true,
   };
   out.dht_profiles.push_back(forge::net::p2p::amino_v1(forge::net::p2p::dht::mode::server));
   out.limits.rendezvous.operating_role = forge::net::p2p::rendezvous::role::client_and_server;
   return out;
}

forge::net::p2p::node::options node_options(const std::filesystem::path& store_path) {
   return node_options(store_path, local_identity());
}

void configure_dns_server(forge::net::p2p::node::options& options,
                          const std::map<std::string, std::string>& args) {
   const auto value = optional_value(args, "dns-server");
   if (value.empty()) {
      return;
   }
   const auto separator = value.rfind(':');
   if (separator == std::string::npos) {
      throw std::runtime_error{"--dns-server requires numeric IP:port (bracket IPv6)"};
   }
   auto host = value.substr(0, separator);
   if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
      host = host.substr(1, host.size() - 2);
   } else if (host.find(':') != std::string::npos) {
      throw std::runtime_error{"--dns-server IPv6 must be bracketed"};
   }
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(host, error);
   auto port = unsigned{};
   const auto end = value.data() + value.size();
   const auto parsed = std::from_chars(value.data() + separator + 1, end, port);
   if (error || host.find('%') != std::string::npos || parsed.ec != std::errc{} || parsed.ptr != end ||
       port == 0 || port > 65'535) {
      throw std::runtime_error{"--dns-server requires numeric IP and port 1..65535"};
   }
   options.dns_resolver.nameservers = {{.address = address.to_string(), .port = static_cast<std::uint16_t>(port)}};
}

void configure_private_network(forge::net::p2p::node::options& options,
                               const std::map<std::string, std::string>& args, std::string_view transport) {
   if (transport != "tcp-pnet") {
      return;
   }

   // The missing-key control deliberately uses the same direct-TCP restrictions
   // without installing a protector, so it can prove rejection against pnet.
   options.capabilities = forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::peer_exchange};
   options.relay_policy.service_enabled = false;
   options.relay_policy.client_enabled = false;
   options.relay_policy.auto_discovery_enabled = false;
   options.path_policy.allow_direct = true;
   options.path_policy.allow_relay = false;
   options.path_policy.allow_hole_punch = false;

   const auto key_file = optional_value(args, "pnet-key-file");
   if (key_file.empty()) {
      return;
   }
   auto input = std::ifstream{key_file, std::ios::binary};
   if (!input) {
      throw std::runtime_error{"could not open pnet key file"};
   }
   auto text = std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
   auto key = forge::net::pnet::pre_shared_key::parse_swarm_key(text);
   options.private_network = forge::net::p2p::private_network::options{
       .protector = std::make_shared<const forge::net::pnet::protector>(
           forge::net::pnet::protector{std::move(key)}),
   };
}

forge::net::p2p::node::options autonat_node_options(std::string_view transport) {
   const auto& identity = local_identity();
   auto options = forge::net::p2p::node::options{
       .certificate_pem = identity.certificate_pem,
       .private_key_pem = identity.private_key_pem,
       .explicit_peer_id = identity.peer,
       .capabilities = forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::autonat},
       .relay_policy = {.service_enabled = false, .client_enabled = false, .public_relay_allowed = false,
                        .auto_discovery_enabled = false},
       .path_policy = {.allow_direct = true, .allow_hole_punch = false, .allow_relay = false},
       .public_key = identity.public_key,
       .peer_state = forge::net::p2p::peer_store::options{
           .persistence = forge::net::p2p::peer_store::make_memory_persistence(),
       },
       .allow_insecure_test_mode = false,
   };
   if (transport == "quic") {
      options.capabilities.add(forge::net::p2p::capabilities::direct_quic);
   }
   options.limits.topology.operating_mode = forge::net::p2p::topology::mode::static_only;
   options.limits.topology.dht_enabled = false;
   options.limits.topology.rendezvous_enabled = false;
   options.limits.topology.peer_exchange_enabled = false;
   return options;
}

forge::test::libp2p_interop::autonat_fixture_node
make_autonat_node(forge::asio::runtime& runtime, const forge::test::libp2p_interop::fixture_arguments& args,
                  bool service) {
   const auto scenario = required(args, "scenario");
   const auto transport = optional_value(args, "transport", "quic");
   static_cast<void>(required(args, "store-dir"));
   auto options = autonat_node_options(transport);
   configure_private_network(options, args, transport);
   if (transport == "tcp-pnet") {
      const auto egress = required(args, "internet-egress");
      if (!options.private_network || (egress != "allow" && egress != "deny")) {
         throw std::runtime_error{"tcp-pnet AutoNAT requires --pnet-key-file and --internet-egress allow|deny"};
      }
      options.private_network->internet_egress = egress == "allow"
          ? forge::net::p2p::private_network::internet_egress_policy::allow_internet
          : forge::net::p2p::private_network::internet_egress_policy::deny_external;
      options.capabilities = forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::autonat};
   }
   const auto v1 = scenario == "autonat_v1";
   options.reachability_policy.client_v1_enabled = !service && v1;
   options.reachability_policy.client_v2_enabled = !service && !v1;
   options.reachability_policy.service_v1_enabled = service && v1;
   options.reachability_policy.service_v2_enabled = service && !v1;
   auto [gater, observation] =
       forge::test::libp2p_interop::make_autonat_connection_observer(options.connection_gater);
   options.connection_gater = std::move(gater);
   return {.value = std::make_unique<forge::net::p2p::node>(runtime, std::move(options)),
           .connection_observation = std::move(observation)};
}

std::string pnet_evidence(const std::map<std::string, std::string>& args) {
   return "\"pnet_enabled\":true,\"negotiated_pnet\":true,\"pnet_fingerprint\":\"" +
          json_escape(required(args, "pnet-fingerprint")) + "\"";
}

std::string pnet_rejection_evidence(const std::map<std::string, std::string>& args, std::string_view role,
                                    const forge::net::p2p::node::metrics_snapshot& metrics,
                                    std::string_view expected_peer = {}) {
   const auto dialer = role == "dialer";
   const auto attempted_connections =
       dialer ? metrics.path_direct_attempts : metrics.handshakes_completed + metrics.handshakes_failed;
   const auto established_connections = metrics.sessions_opened;
   const auto post_authentication_streams =
       dialer ? metrics.protocol_streams_opened : metrics.protocol_streams_accepted;
   // Forge does not split protocol counters by protocol. A rejected pre-session
   // connection therefore proves that neither Identify nor application streams
   // could have opened; any established session is conservatively a failure.
   const auto identify_streams = established_connections == 0 ? 0 : post_authentication_streams;
   const auto application_streams = established_connections == 0 ? 0 : post_authentication_streams;
   const auto rejected_before_identify =
       established_connections == 0 && identify_streams == 0 && application_streams == 0;
   auto value = "\"implementation\":\"forge\",\"role\":\"" + std::string{role} +
                "\",\"scenario\":\"pnet\",\"status\":\"rejected\",\"control_kind\":\"" +
                json_escape(required(args, "pnet-control")) + "\",\"correlation_token\":\"" +
                json_escape(required(args, "pnet-correlation")) +
                "\",\"counter_source\":\"forge.node.metrics\",\"attempted_connections\":" +
                std::to_string(attempted_connections) +
                ",\"established_connections\":" + std::to_string(established_connections) +
                ",\"identify_streams\":" + std::to_string(identify_streams) +
                ",\"application_streams\":" + std::to_string(application_streams) +
                ",\"rejected_before_identify\":" + (rejected_before_identify ? "true" : "false");
   if (!expected_peer.empty()) {
      value += ",\"expected_peer_id\":\"" + json_escape(expected_peer) + "\"";
   }
   return value;
}

std::string_view negotiated_security(const forge::net::p2p::peer_authentication authentication) {
   switch (authentication) {
   case forge::net::p2p::peer_authentication::noise:
      return "/noise";
   case forge::net::p2p::peer_authentication::libp2p_tls:
      return "/tls/1.0.0";
   default:
      throw std::runtime_error{"PNET stream did not expose an authenticated Noise/TLS channel"};
   }
}

void configure_rendezvous_lifecycle_ttls(forge::net::p2p::node::options& options, const std::string_view scenario) {
   if (scenario != "rendezvous_lifecycle") {
      return;
   }
   options.limits.rendezvous.min_ttl = 1s;
   options.limits.rendezvous.default_ttl = 2s;
   options.limits.rendezvous.max_ttl = 3s;
}

std::string endpoint_json(const forge::net::p2p::endpoint& endpoint) {
   return "\"" + json_escape(endpoint.to_string()) + "\"";
}

forge::net::p2p::endpoint loopback_quic_endpoint(std::uint16_t port = 0) {
   return forge::net::p2p::endpoint{.transport = {.host_type = forge::net::p2p::endpoint::host_kind::ip4,
                                                  .protocol = forge::net::p2p::endpoint::protocol_kind::quic_v1,
                                                  .host = "127.0.0.1",
                                                  .port = port}};
}

forge::net::p2p::endpoint loopback_tcp_endpoint(std::uint16_t port = 0) {
   return forge::net::p2p::endpoint{.transport = {.host_type = forge::net::p2p::endpoint::host_kind::ip4,
                                                  .protocol = forge::net::p2p::endpoint::protocol_kind::tcp,
                                                  .host = "127.0.0.1",
                                                  .port = port}};
}

forge::net::p2p::endpoint loopback_endpoint_for(std::string_view transport) {
   if (transport == "tcp" || transport == "tcp-tls" || transport == "tcp-pnet") {
      return loopback_tcp_endpoint();
   }
   if (transport == "quic" || transport.empty()) {
      return loopback_quic_endpoint();
   }
   throw std::runtime_error{"unsupported FORGE fixture transport: " + std::string{transport}};
}

forge::net::p2p::endpoint p2p_endpoint_for(forge::net::p2p::endpoint value, const forge::net::p2p::peer_id& peer) {
   value.peer = peer;
   return value;
}

forge::net::p2p::dht::key seed_probe_key() {
   constexpr auto source = std::string_view{"forge-libp2p-dht-provider"};
   auto digest = forge::multiformats::multihash::sha2_256(
       std::span<const std::uint8_t>{reinterpret_cast<const std::uint8_t*>(source.data()), source.size()});
   return forge::net::p2p::dht::key{.bytes = digest.encode()};
}

std::vector<std::uint8_t> signed_rendezvous_record(const libp2p_identity& identity,
                                                   const forge::net::p2p::endpoint& endpoint, std::uint64_t sequence) {
   const auto key = forge::net::p2p::decode_public_key(identity.public_key);
   return forge::net::p2p::rendezvous::codec::seal_peer_record(
              forge::net::p2p::rendezvous::peer_record{
                  .peer = identity.peer,
                  .endpoints = {endpoint.to_multiaddr()},
                  .sequence = sequence,
              },
              key, forge::crypto::pki::pem::read_private_key(identity.private_key_pem))
       .encode();
}

std::vector<std::uint8_t> wrap_length_delimited(std::span<const std::uint8_t> payload);
boost::asio::awaitable<std::vector<std::uint8_t>> read_length_delimited(forge::net::p2p::stream& stream,
                                                                        std::size_t max_payload_size,
                                                                        std::vector<std::uint8_t>* observed_frame = nullptr);

void register_echo(forge::net::p2p::node& value) {
   value.register_protocol_handler(
       forge::net::p2p::protocol_id{.value = std::string{echo_protocol}},
       [](forge::net::p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          auto payload = co_await read_length_delimited(incoming.stream, maximum_echo_payload);
          co_await incoming.stream.async_write(wrap_length_delimited(payload));
       });
}

struct pubsub_stress_state {
   std::mutex mutex;
   std::set<std::string> payloads;
   std::uint64_t duplicates = 0;
   std::uint64_t invalid = 0;
};

void register_pubsub_listener(forge::asio::runtime& runtime, forge::net::p2p::node& value,
                              std::filesystem::path result_file) {
   forge::asio::blocking::run(
       runtime, value.async_subscribe(
                    forge::net::p2p::pubsub::topic{.value = std::string{pubsub_topic}},
                    [result_file = std::move(result_file)](forge::net::p2p::pubsub::event event)
                        -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                       const auto payload = std::string{event.value.data.begin(), event.value.data.end()};
                       write_file(result_file,
                                  "{\"implementation\":\"forge\",\"scenario\":\"gossipsub_publish\",\"status\":\"" +
                                      std::string{payload == pubsub_payload ? "ok" : "mismatch"} + "\",\"topic\":\"" +
                                      json_escape(event.value.subject.value) + "\",\"payload\":\"" +
                                      json_escape(payload) + "\",\"source\":\"" +
                                      json_escape(event.value.from ? event.value.from->to_string() : std::string{}) +
                                      "\"}\n");
                       co_return forge::net::p2p::pubsub::validation_result::accept;
                    }));
}

std::shared_ptr<pubsub_stress_state> register_pubsub_stress_listener(forge::asio::runtime& runtime,
                                                                     forge::net::p2p::node& value) {
   auto state = std::make_shared<pubsub_stress_state>();
   forge::asio::blocking::run(
       runtime, value.async_subscribe(forge::net::p2p::pubsub::topic{.value = std::string{pubsub_topic}},
                                      [state](forge::net::p2p::pubsub::event event)
                                          -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
                                         const auto payload =
                                             std::string{event.value.data.begin(), event.value.data.end()};
                                         auto lock = std::scoped_lock{state->mutex};
                                         if (!state->payloads.insert(payload).second) {
                                            ++state->duplicates;
                                         }
                                         co_return forge::net::p2p::pubsub::validation_result::accept;
                                      }));
   return state;
}

void prepare_pubsub_publisher(forge::asio::runtime& runtime, forge::net::p2p::node& value) {
   forge::asio::blocking::run(
       runtime,
       value.async_subscribe(
           forge::net::p2p::pubsub::topic{.value = std::string{pubsub_topic}},
           [](forge::net::p2p::pubsub::event) -> boost::asio::awaitable<forge::net::p2p::pubsub::validation_result> {
              co_return forge::net::p2p::pubsub::validation_result::accept;
           }));

   const auto deadline = std::chrono::steady_clock::now() + 8s;
   while (std::chrono::steady_clock::now() < deadline) {
      const auto snapshot = value.pubsub_snapshot();
      if (snapshot.peers > 0 || snapshot.mesh_edges > 0) {
         return;
      }
      std::this_thread::sleep_for(100ms);
   }
}

void write_pubsub_stress_result(const std::filesystem::path& result_file, std::string_view implementation,
                                const pubsub_stress_state& state, std::uint64_t expected,
                                const forge::net::p2p::node& value) {
   auto payloads = std::string{};
   auto first = true;
   for (const auto& payload : state.payloads) {
      if (!first) {
         payloads += ",";
      }
      first = false;
      payloads += "\"" + json_escape(payload) + "\"";
   }
   const auto metrics = value.metrics();
   write_file(result_file,
              "{\"implementation\":\"" + std::string{implementation} +
                  "\",\"scenario\":\"gossipsub_mixed_mesh_stress\",\"status\":\"" +
                  std::string{state.payloads.size() >= expected && state.duplicates == 0 ? "ok" : "mismatch"} +
                  "\",\"received\":" + std::to_string(state.payloads.size()) +
                  ",\"expected\":" + std::to_string(expected) + ",\"duplicates\":" + std::to_string(state.duplicates) +
                  ",\"invalid\":" + std::to_string(metrics.pubsub_invalid_messages) + ",\"rejected\":" +
                  std::to_string(metrics.protocol_rejections) + ",\"payloads\":[" + payloads + "]}\n");
}

std::vector<std::uint8_t> unwrap_length_delimited(std::span<const std::uint8_t> bytes, std::size_t max_payload_size) {
   auto decoded = forge::multiformats::decoded_varint{};
   try {
      decoded = forge::multiformats::varint_decode(bytes);
   } catch (const std::exception& error) {
      throw std::runtime_error{std::string{"failed to decode libp2p protobuf length: "} + error.what()};
   }
   if (decoded.value > max_payload_size) {
      throw std::runtime_error{"libp2p protobuf message exceeds max size"};
   }
   const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
   if (total != bytes.size()) {
      throw std::runtime_error{"libp2p protobuf message length mismatch"};
   }
   return {bytes.begin() + static_cast<std::ptrdiff_t>(decoded.size), bytes.end()};
}

std::vector<std::uint8_t> wrap_length_delimited(std::span<const std::uint8_t> payload) {
   auto out = forge::multiformats::varint_encode(payload.size());
   out.insert(out.end(), payload.begin(), payload.end());
   return out;
}

boost::asio::awaitable<std::vector<std::uint8_t>> read_length_delimited(forge::net::p2p::stream& stream,
                                                                        std::size_t max_payload_size,
                                                                        std::vector<std::uint8_t>* observed_frame) {
   auto buffer = std::vector<std::uint8_t>{};
   while (true) {
      try {
         const auto decoded = forge::multiformats::varint_decode(buffer);
         if (decoded.value > max_payload_size) {
            throw std::runtime_error{"libp2p protobuf message exceeds max size"};
         }
         const auto total = decoded.size + static_cast<std::size_t>(decoded.value);
         if (buffer.size() >= total) {
            auto frame = std::vector<std::uint8_t>{buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(total)};
            if (observed_frame) {
               if (buffer.size() != total) {
                  throw std::runtime_error{"single-response evidence contains trailing wire bytes"};
               }
               *observed_frame = frame;
            }
            co_return unwrap_length_delimited(frame, max_payload_size);
         }
      } catch (const forge::multiformats::exceptions::invalid_format& error) {
         if (std::string_view{error.what()}.find("unterminated") == std::string_view::npos) {
            throw;
         }
      }
      auto chunk = co_await stream.async_read();
      if (chunk.empty()) {
         throw std::runtime_error{"FORGE wire stream ended before a complete frame"};
      }
      buffer.insert(buffer.end(), chunk.begin(), chunk.end());
   }
}

void validate_dht_wire_response(const forge::net::p2p::dht::message& request,
                                const forge::net::p2p::dht::message& response) {
   // One request per stream correlates Rust's omitted key, never a wrong key.
   if (response.type != request.type ||
       (!response.key_value.bytes.empty() && response.key_value != request.key_value)) {
      throw std::runtime_error{"FORGE DHT wire response does not match the locally emitted request"};
   }
}

// Every access, including cancellation delivery, belongs to the exchange strand.
struct dht_wire_exchange_state {
   explicit dht_wire_exchange_state(boost::asio::any_io_executor executor,
                                     std::chrono::steady_clock::time_point deadline)
       : timer{executor, deadline} {}

   boost::asio::steady_timer timer;
   boost::asio::cancellation_signal cancel_io;
   forge::asio::notification timer_finished;
   forge::net::p2p::stream stream;
   forge::net::p2p::dht::message response;
   std::exception_ptr error;
   const std::exception_ptr timeout_error =
       std::make_exception_ptr(std::runtime_error{"FORGE DHT wire exchange timed out"});
   const std::exception_ptr canceled_error =
       std::make_exception_ptr(boost::system::system_error{boost::asio::error::operation_aborted});
   bool terminal = false;
   bool timer_done = true;

   void finish(std::exception_ptr failure = {}) {
      if (!terminal) {
         terminal = true;
         error = failure;
      }
   }

   void request_cancel(std::exception_ptr failure) {
      if (!terminal) {
         finish(failure);
         cancel_io.emit(boost::asio::cancellation_type::all);
         stream.request_cancel();
      }
   }

   std::chrono::milliseconds remaining() {
      const auto now = std::chrono::steady_clock::now();
      if (!terminal && now >= timer.expiry()) {
         request_cancel(timeout_error);
      }
      if (error) {
         std::rethrow_exception(error);
      }
      return std::chrono::ceil<std::chrono::milliseconds>(timer.expiry() - now);
   }
};

boost::asio::awaitable<void> async_dht_wire_io(
    std::shared_ptr<dht_wire_exchange_state> state, forge::net::p2p::node& node,
    forge::net::p2p::peer_id peer, forge::net::p2p::dht::message request,
    forge::net::p2p::dht::profile profile, std::optional<forge::net::p2p::endpoint> seed_endpoint) {
   try {
      if (seed_endpoint) {
         const auto session = co_await node.async_connect(
             *seed_endpoint, forge::net::p2p::node::connect_options{
                                 .expected_peer = peer, .allow_relay = false, .timeout = state->remaining(),
                                 .allow_hole_punch = false});
         if (session.identify_state != forge::net::p2p::identify::state::identified) {
            throw std::runtime_error{"DHT seed did not complete authenticated Identify"};
         }
      }
      state->stream = co_await node.async_open_protocol_stream(
          peer, forge::net::p2p::builtins::kad_dht,
          forge::net::p2p::node::open_options{
              .allow_relay = false, .timeout = state->remaining(), .allow_hole_punch = false});
      static_cast<void>(state->remaining());
      co_await state->stream.async_write(forge::net::p2p::dht::codec::encode(request, profile));
      static_cast<void>(state->remaining());
      const auto payload = co_await read_length_delimited(state->stream, profile.limits.max_inbound_message_size);
      state->response = forge::net::p2p::dht::codec::decode(wrap_length_delimited(payload), profile);
      validate_dht_wire_response(request, state->response);
      static_cast<void>(state->remaining());
      // Normal FIN completion is part of the exchange, not unbounded cleanup.
      co_await state->stream.async_close();
      static_cast<void>(state->remaining());
      state->finish();
   } catch (...) {
      state->finish(std::current_exception());
   }
}

boost::asio::awaitable<forge::net::p2p::dht::message> async_dht_wire_on_strand(
    forge::net::p2p::node& node, forge::net::p2p::peer_id peer,
    forge::net::p2p::dht::message request, std::chrono::steady_clock::time_point deadline,
    std::optional<forge::net::p2p::endpoint> seed_endpoint) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto state = std::make_shared<dht_wire_exchange_state>(executor, deadline);
   const auto inherited = (co_await boost::asio::this_coro::cancellation_state).cancelled();
   co_await boost::asio::this_coro::reset_cancellation_state([state](boost::asio::cancellation_type type) {
      if (type != boost::asio::cancellation_type::none) {
         state->request_cancel(state->canceled_error);
      }
      // The supervisor must join I/O even when its caller has canceled.
      return boost::asio::cancellation_type::none;
   });
   if (inherited != boost::asio::cancellation_type::none) {
      state->request_cancel(state->canceled_error);
   }
   try {
      state->timer.async_wait([state](boost::system::error_code error) {
         if (!error) {
            state->request_cancel(state->timeout_error);
         }
         state->timer_done = true;
         state->timer_finished.notify();
      });
      state->timer_done = false;
      co_await boost::asio::co_spawn(
          executor, async_dht_wire_io(state, node, std::move(peer), std::move(request),
                                     forge::net::p2p::amino_v1(forge::net::p2p::dht::mode::client),
                                     std::move(seed_endpoint)),
          boost::asio::bind_cancellation_slot(state->cancel_io.slot(), boost::asio::use_awaitable));
   } catch (...) {
      state->finish(std::current_exception());
   }

   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   state->timer.cancel();
   while (!state->timer_done) {
      const auto epoch = state->timer_finished.epoch();
      if (!state->timer_done) {
         co_await state->timer_finished.async_wait(epoch);
      }
   }
   // Also leave any notifying handler's stack before releasing its resources.
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   if (state->error) {
      state->stream.request_cancel();
      try {
         co_await state->stream.async_close();
      } catch (...) {
         // The terminal cleanup barrier has completed; retain the primary error.
      }
      std::rethrow_exception(state->error);
   }
   co_return std::move(state->response);
}

boost::asio::awaitable<forge::net::p2p::dht::message> async_bounded_dht_wire_exchange(
    forge::net::p2p::node& node, forge::net::p2p::peer_id peer,
    forge::net::p2p::dht::message request, std::chrono::steady_clock::time_point deadline,
    std::optional<forge::net::p2p::endpoint> seed_endpoint = std::nullopt) {
   const auto executor = co_await boost::asio::this_coro::executor;
   co_return co_await boost::asio::co_spawn(
       boost::asio::make_strand(executor),
       async_dht_wire_on_strand(node, std::move(peer), std::move(request), deadline, std::move(seed_endpoint)),
       boost::asio::bind_executor(executor, boost::asio::use_awaitable));
}

struct dht_wire_self_test_state {
   boost::asio::any_io_executor executor;
   boost::asio::cancellation_signal cancel;
   std::atomic<unsigned> requests = 0;
};

boost::asio::awaitable<void> async_dht_wire_self_test_reply(
    std::shared_ptr<dht_wire_self_test_state> state, forge::net::p2p::node::incoming_protocol_stream incoming) {
   try {
      const auto profile = forge::net::p2p::amino_v1(forge::net::p2p::dht::mode::server);
      const auto payload = co_await read_length_delimited(incoming.stream, profile.limits.max_inbound_message_size);
      auto response = forge::net::p2p::dht::codec::decode(wrap_length_delimited(payload), profile);
      auto key = forge::multiformats::multihash::decode(response.key_value.bytes);
      const auto test_case = key.digest.at(0);
      state->requests.fetch_add(1);
      if (test_case == 5) {
         boost::asio::post(state->executor, [state] { state->cancel.emit(boost::asio::cancellation_type::all); });
      }
      if (test_case >= 4) {
         // No reply: only the client's timeout/slot cancellation ends this read.
         static_cast<void>(co_await incoming.stream.async_read());
      } else {
         if (test_case == 0) {
            response.key_value.bytes.clear();
         } else if (test_case == 2) {
            response.type = forge::net::p2p::dht::message_type::find_node;
         } else if (test_case == 3) {
            key.digest.back() ^= 0xff;
            response.key_value.bytes = key.encode();
         }
         co_await incoming.stream.async_write(forge::net::p2p::dht::codec::encode(response, profile));
      }
   } catch (...) {
      // Reset/EOF is expected for the deliberately stalled or rejected replies.
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   co_await incoming.stream.async_close();
}

boost::asio::awaitable<void> async_dht_wire_self_test(forge::asio::runtime& runtime) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto state = std::make_shared<dht_wire_self_test_state>();
   state->executor = executor;
   auto server_options = node_options({}, generate_libp2p_identity());
   server_options.dht_profiles.clear();
   auto client_options = node_options({}, generate_libp2p_identity());
   client_options.dht_profiles.clear();
   auto server = forge::net::p2p::node{runtime, std::move(server_options)};
   auto client = forge::net::p2p::node{runtime, std::move(client_options)};
   std::exception_ptr failure;
   try {
      server.register_protocol_handler(forge::net::p2p::builtins::kad_dht,
                                       [state](forge::net::p2p::node::incoming_protocol_stream incoming) {
                                          return async_dht_wire_self_test_reply(state, std::move(incoming));
                                       });
      co_await server.async_listen(loopback_endpoint_for("quic"));
      const auto endpoint = p2p_endpoint_for(*server.local_endpoint(), server.local_peer());
      client.peers().learn_endpoint(server.local_peer(), endpoint,
                                   forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic});
      const auto names = std::vector<std::string_view>{
          "dht_wire_empty_key", "dht_wire_matching_key", "dht_wire_wrong_kind", "dht_wire_wrong_key",
          "dht_wire_stalled_read_timeout", "dht_wire_stalled_read_parent_cancel"};
      for (std::size_t test_case = 0; test_case < names.size(); ++test_case) {
         auto key = forge::multiformats::multihash::decode(seed_probe_key().bytes);
         // Keep the SHA-256 multihash envelope valid; only its digest carries the case marker.
         key.digest.at(0) = static_cast<std::uint8_t>(test_case);
         auto request = forge::net::p2p::dht::message{
             .type = forge::net::p2p::dht::message_type::get_providers,
             .key_value = {.bytes = key.encode()}};
         auto error = std::string{};
         auto canceled = false;
         try {
            static_cast<void>(co_await boost::asio::co_spawn(
                executor, async_bounded_dht_wire_exchange(client, server.local_peer(), std::move(request),
                                                          std::chrono::steady_clock::now() + dht_wire_timeout),
                boost::asio::bind_cancellation_slot(state->cancel.slot(), boost::asio::use_awaitable)));
         } catch (const boost::system::system_error& caught) {
            canceled = caught.code() == boost::asio::error::operation_aborted;
            error = caught.what();
         } catch (const std::exception& caught) {
            error = caught.what();
         }
         const auto expected_error = test_case == 2 || test_case == 3
             ? "FORGE DHT wire response does not match the locally emitted request"
             : test_case == 4 ? "FORGE DHT wire exchange timed out" : "";
         if (state->requests.load() != test_case + 1 ||
             (test_case == 5 ? !canceled : error != expected_error)) {
            throw std::runtime_error{std::string{names[test_case]} + " failed: " + error};
         }
      }
   } catch (...) {
      failure = std::current_exception();
   }
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   for (auto* node : {&client, &server}) {
      try {
         co_await node->async_stop();
      } catch (...) {
         if (!failure) {
            failure = std::current_exception();
         }
      }
   }
   if (failure) {
      std::rethrow_exception(failure);
   }
}

int dht_wire_self_test_mode() {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   forge::asio::blocking::run(
       runtime, boost::asio::co_spawn(boost::asio::make_strand(runtime.context()), async_dht_wire_self_test(runtime),
                                     boost::asio::use_awaitable));
   std::cout << "DHT wire self-test: PASS (6 cases; client and server cleanup joined)\n";
   return 0;
}

// The DHT peer id carries the identity binding. A /p2p suffix can reinforce
// that binding, but standard peers may advertise a plain transport multiaddr.
std::string usable_wire_peer_address(const forge::net::p2p::dht::peer& wire_peer,
                                     const forge::net::p2p::peer_id& expected,
                                     std::string_view operation) {
   if (wire_peer.id != expected) {
      throw std::runtime_error{std::string{operation} + " wire reply returned the wrong peer id"};
   }
   for (const auto& address : wire_peer.endpoints) {
      const auto& components = address.components();
      if (components.empty()) {
         continue;
      }
      if (components.back().code == forge::multiformats::protocol_code::p2p &&
          components.back().value != expected.to_string()) {
         throw std::runtime_error{std::string{operation} + " wire reply has a contradictory /p2p suffix"};
      }
      try {
         const auto endpoint = forge::net::p2p::parse_endpoint(address.to_string());
         if (endpoint.is_direct_quic() || endpoint.is_direct_tcp()) {
            return address.to_string();
         }
      } catch (const forge::exceptions::base&) {
         // Try the remaining advertised addresses before rejecting the peer.
      }
   }
   throw std::runtime_error{std::string{operation} + " wire reply did not contain a usable peer address"};
}

// This is distinct from async_find_providers: it proves the concrete Q -> S
// GET_PROVIDERS reply that carried P and its provider-bound address.
std::string raw_get_providers_address(forge::asio::runtime& runtime, forge::net::p2p::node& querier,
                                      const forge::net::p2p::peer_id& listener,
                                      const forge::net::p2p::dht::key& key,
                                      const forge::net::p2p::peer_id& provider) {
   const auto response = forge::asio::blocking::run(
       runtime, async_bounded_dht_wire_exchange(
                    querier, listener,
                    forge::net::p2p::dht::message{
                        .type = forge::net::p2p::dht::message_type::get_providers,
                        .key_value = key,
                    },
                    std::chrono::steady_clock::now() + dht_wire_timeout));
   const auto found = std::ranges::find(response.provider_peers, provider, &forge::net::p2p::dht::peer::id);
   if (found == response.provider_peers.end()) {
      throw std::runtime_error{"FORGE GET_PROVIDERS wire reply did not contain the registered provider"};
   }
   return usable_wire_peer_address(*found, provider, "FORGE GET_PROVIDERS");
}

// This separate RPC proves the exact FIND_NODE reply from the sole seed after
// async_find_peer has independently returned the previously unknown target.
std::string raw_find_node_address(forge::asio::runtime& runtime, forge::net::p2p::node& querier,
                                  const forge::net::p2p::peer_id& seed,
                                  const forge::net::p2p::peer_id& target) {
   const auto key = forge::net::p2p::make_dht_key(target);
   const auto response = forge::asio::blocking::run(
       runtime, async_bounded_dht_wire_exchange(
                    querier, seed,
                    forge::net::p2p::dht::message{
                        .type = forge::net::p2p::dht::message_type::find_node,
                        .key_value = key,
                    },
                    std::chrono::steady_clock::now() + dht_wire_timeout));
   const auto found = std::ranges::find(response.closer_peers, target, &forge::net::p2p::dht::peer::id);
   if (found == response.closer_peers.end()) {
      throw std::runtime_error{"FORGE FIND_NODE wire reply did not contain the target peer"};
   }
   return usable_wire_peer_address(*found, target, "FORGE FIND_NODE");
}

void establish_dht_seed_route(forge::asio::runtime& runtime, forge::net::p2p::node& value,
                              const std::map<std::string, std::string>& args) {
   const auto seed = forge::net::p2p::peer_id::from_string(required(args, "seed-peer-id"));
   const auto endpoint = forge::net::p2p::parse_endpoint(required(args, "seed-addr"));
   if (!endpoint.peer || *endpoint.peer != seed) {
      throw std::runtime_error{"DHT seed address must carry the supplied peer id"};
   }
   value.peers().learn_endpoint(seed, endpoint,
                                forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic});
   static_cast<void>(forge::asio::blocking::run(
       runtime, async_bounded_dht_wire_exchange(
                    value, seed,
                    forge::net::p2p::dht::message{
                        .type = forge::net::p2p::dht::message_type::get_providers,
                        .key_value = seed_probe_key(),
                    },
                    std::chrono::steady_clock::now() + dht_wire_timeout, endpoint)));
   write_file(required(args, "result-file"),
              "{\"implementation\":\"forge\",\"role\":\"routing_listener\",\"scenario\":\"dht_hidden_find_peer\","
              "\"status\":\"ok\",\"seed_peer_id\":\"" +
                  json_escape(seed.to_string()) +
                  "\",\"authenticated_seed\":true,\"seed_route_source\":\"explicit_get_providers_wire_reply\","
                  "\"negotiated_protocol\":\"/ipfs/kad/1.0.0\"}\n");
}

forge::net::p2p::rendezvous::discover_response raw_rendezvous_discover(forge::asio::runtime& runtime,
                                                                       forge::net::p2p::node& value,
                                                                       const forge::net::p2p::peer_id& peer,
                                                                       std::vector<std::uint8_t> cookie = {}) {
   auto stream = forge::asio::blocking::run(
       runtime, value.async_open_protocol_stream(peer, forge::net::p2p::builtins::rendezvous,
                                                 forge::net::p2p::node::open_options{.allow_relay = false}));
   forge::asio::blocking::run(
       runtime, stream.async_write(forge::net::p2p::rendezvous::codec::encode(forge::net::p2p::rendezvous::message{
                    .type = forge::net::p2p::rendezvous::message_type::discover,
                    .discover_value =
                        forge::net::p2p::rendezvous::discover_request{
                            .namespace_name = std::string{rendezvous_namespace},
                            .limit = 10,
                            .cookie = std::move(cookie),
                        },
                })));
   const auto response = forge::net::p2p::rendezvous::codec::decode(
       wrap_length_delimited(forge::asio::blocking::run(runtime, read_length_delimited(stream, 1024 * 1024))));
   forge::asio::blocking::run(runtime, stream.async_close());
   if (response.type != forge::net::p2p::rendezvous::message_type::discover_response ||
       !response.discover_response_value) {
      throw std::runtime_error{"rendezvous expected discover response"};
   }
   if (response.discover_response_value->status_value != forge::net::p2p::rendezvous::status::ok) {
      throw std::runtime_error{"rendezvous discover returned non-ok status"};
   }
   return *response.discover_response_value;
}

void raw_rendezvous_unregister(forge::asio::runtime& runtime, forge::net::p2p::node& value,
                               const forge::net::p2p::peer_id& peer) {
   auto stream = forge::asio::blocking::run(
       runtime, value.async_open_protocol_stream(peer, forge::net::p2p::builtins::rendezvous,
                                                 forge::net::p2p::node::open_options{.allow_relay = false}));
   forge::asio::blocking::run(
       runtime, stream.async_write(forge::net::p2p::rendezvous::codec::encode(forge::net::p2p::rendezvous::message{
                    .type = forge::net::p2p::rendezvous::message_type::unregister_peer,
                    .unregister_value =
                        forge::net::p2p::rendezvous::unregister_request{
                            .namespace_name = std::string{rendezvous_namespace},
                        },
                })));
   forge::asio::blocking::run(runtime, stream.async_close());
}

std::size_t rendezvous_registration_count(const forge::net::p2p::rendezvous::discover_response& response,
                                          const forge::net::p2p::peer_id& peer,
                                          const forge::net::p2p::endpoint& endpoint,
                                          const std::uint64_t expected_sequence) {
   const auto expected_endpoint = endpoint.to_string();
   return std::ranges::count_if(
       response.registrations, [&](const forge::net::p2p::rendezvous::registration& registration) {
          if (registration.peer != peer || registration.signed_peer_record.empty() ||
              !std::ranges::any_of(registration.endpoints,
                                   [&](const auto& candidate) { return candidate.to_string() == expected_endpoint; })) {
             return false;
          }
          const auto record = forge::net::p2p::rendezvous::codec::open_peer_record(
              forge::net::p2p::signed_envelope::decode(registration.signed_peer_record), peer);
          return record.peer == peer && record.sequence == expected_sequence &&
                 std::ranges::any_of(record.endpoints,
                                     [&](const auto& candidate) { return candidate.to_string() == expected_endpoint; });
       });
}

std::size_t rendezvous_peer_registration_count(const forge::net::p2p::rendezvous::discover_response& response,
                                               const forge::net::p2p::peer_id& peer) {
   return std::ranges::count_if(
       response.registrations,
       [&](const forge::net::p2p::rendezvous::registration& registration) { return registration.peer == peer; });
}

[[nodiscard]] bool
persistence_contains(forge::asio::runtime& runtime,
                     const std::shared_ptr<forge::net::p2p::dht::record_store::persistence>& persistence,
                     const dht_value_fixture& expected) {
   auto cursor = std::optional<std::vector<std::byte>>{};
   do {
      auto page =
          forge::asio::blocking::run(runtime, persistence->async_hydrate({
                                                  .kind = forge::net::p2p::dht::record_store::hydration_kind::values,
                                                  .cursor = cursor,
                                                  .limit = 256,
                                              }));
      if (std::ranges::any_of(page.values, [&](const auto& value) {
             return value.record.key_value == expected.key && value.record.value == expected.value;
          })) {
         return true;
      }
      cursor = std::move(page.cursor);
   } while (cursor);
   return false;
}

int listen_mode(const std::map<std::string, std::string>& args) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto scenario = optional_value(args, "scenario");
   const auto transport = optional_value(args, "transport", "quic");
   auto persistence = forge::net::p2p::dht::record_store::make_memory_persistence();
   auto options = node_options(required(args, "store-dir"));
   configure_dns_server(options, args);
   configure_private_network(options, args, transport);
   configure_rendezvous_lifecycle_ttls(options, scenario);
   options.dht_record_persistence.emplace(forge::net::p2p::builtins::kad_dht, persistence);
   auto value = forge::net::p2p::node{runtime, std::move(options)};
   register_echo(value);
   auto stress_state = std::shared_ptr<pubsub_stress_state>{};
   if (scenario == "gossipsub_publish") {
      register_pubsub_listener(runtime, value, required(args, "result-file"));
   } else if (scenario == "gossipsub_mixed_mesh_stress") {
      stress_state = register_pubsub_stress_listener(runtime, value);
   }
   forge::asio::blocking::run(runtime, value.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, value.async_listen(loopback_endpoint_for(transport)));
   const auto local = value.local_endpoint();
   if (!local) {
      throw std::runtime_error{"FORGE fixture did not expose a local endpoint"};
   }
   const auto endpoint = p2p_endpoint_for(*local, value.local_peer());
   write_file(required(args, "ready-file"), "{\"implementation\":\"forge\",\"role\":\"listener\",\"peer_id\":\"" +
                                                json_escape(value.local_peer().to_string()) + "\",\"listen_addrs\":[" +
                                                endpoint_json(endpoint) + "],\"status\":\"ready\"}\n");

   const auto stop_file = std::filesystem::path{required(args, "stop-file")};
   auto seeded = false;
   auto value_record_reported = false;
   auto pnet_result_reported = false;
   while (!std::filesystem::exists(stop_file)) {
      if (!pnet_result_reported && scenario == "pnet" && value.metrics().sessions_opened > 0) {
         write_file(required(args, "result-file"), "{\"implementation\":\"forge\",\"role\":\"listener\",\"scenario\":\"pnet\",\"status\":\"ok\"," + pnet_evidence(args) + "}\n");
         pnet_result_reported = true;
      }
      if (!value_record_reported && is_dht_value_scenario(scenario) &&
          persistence_contains(runtime, persistence, value_fixture(scenario))) {
         write_file(required(args, "result-file"),
                    "{\"implementation\":\"forge\",\"role\":\"listener\",\"scenario\":\"" + json_escape(scenario) +
                        "\",\"status\":\"ok\",\"record_persisted\":true}\n");
         value_record_reported = true;
      }
      if (!seeded && scenario == "gossipsub_mixed_mesh_stress") {
         const auto seed_file = std::filesystem::path{required(args, "seed-file")};
         if (std::filesystem::exists(seed_file)) {
            for (const auto& line : read_lines(seed_file)) {
               const auto remote = forge::net::p2p::parse_endpoint(line);
               if (!remote.peer || *remote.peer == value.local_peer()) {
                  continue;
               }
               value.peers().learn_endpoint(
                   *remote.peer, remote,
                   forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic |
                                                           forge::net::p2p::capabilities::pubsub});
               try {
                  (void)forge::asio::blocking::run(
                      runtime,
                      value.async_connect(remote, forge::net::p2p::node::connect_options{.expected_peer = *remote.peer,
                                                                                         .allow_relay = false,
                                                                                         .allow_hole_punch = false}));
               } catch (const forge::exceptions::base&) {
               }
            }
            seeded = true;
         }
      }
      if (!seeded && scenario == "dht_hidden_find_peer") {
         establish_dht_seed_route(runtime, value, args);
         seeded = true;
      }
      std::this_thread::sleep_for(100ms);
   }
   if (!pnet_result_reported && scenario == "pnet" && !optional_value(args, "pnet-control").empty()) {
      write_file(required(args, "result-file"),
                 "{" + pnet_rejection_evidence(args, "listener", value.metrics()) + "}\n");
   }
   const auto metrics = value.metrics();
   std::cerr << "forge listener metrics:"
             << " sessions_opened=" << metrics.sessions_opened
             << " handshakes_completed=" << metrics.handshakes_completed
             << " handshakes_failed=" << metrics.handshakes_failed
             << " protocol_streams_accepted=" << metrics.protocol_streams_accepted
             << " protocol_rejections=" << metrics.protocol_rejections
             << " pubsub_received=" << metrics.pubsub_messages_received
             << " pubsub_delivered=" << metrics.pubsub_messages_delivered
             << " pubsub_invalid=" << metrics.pubsub_invalid_messages
             << " pubsub_duplicates=" << metrics.pubsub_duplicates << "\n";
   if (stress_state) {
      auto lock = std::scoped_lock{stress_state->mutex};
      write_pubsub_stress_result(
          required(args, "result-file"), "forge", *stress_state,
          static_cast<std::uint64_t>(std::stoull(optional_value(args, "expected-messages", "3"))), value);
   }
   forge::asio::blocking::run(runtime, value.async_stop());
   return 0;
}

int destination_mode(const std::map<std::string, std::string>& args) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto value = forge::net::p2p::node{runtime, node_options(required(args, "store-dir"))};
   register_echo(value);
   forge::asio::blocking::run(runtime, value.async_listen(loopback_quic_endpoint()));

   const auto relay_addr = forge::net::p2p::parse_endpoint(required(args, "relay-addr"));
   const auto relay_peer = forge::net::p2p::peer_id::from_string(required(args, "relay-peer-id"));
   value.peers().learn_endpoint(
       relay_peer, relay_addr,
       forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic |
                                               forge::net::p2p::capabilities::relay |
                                               forge::net::p2p::capabilities::relay_reservation});
   const auto reservation = forge::asio::blocking::run(runtime, value.async_reserve_relay(relay_peer));
   const auto local = value.local_endpoint();
   if (!local) {
      throw std::runtime_error{"FORGE destination did not expose a local endpoint"};
   }
   auto relay_addrs = std::string{};
   for (std::size_t i = 0; i < reservation.relay_endpoints.size(); ++i) {
      if (i != 0) {
         relay_addrs += ",";
      }
      relay_addrs += endpoint_json(reservation.relay_endpoints[i]);
   }
   write_file(required(args, "ready-file"),
              "{\"implementation\":\"forge\",\"role\":\"destination\",\"peer_id\":\"" +
                  json_escape(value.local_peer().to_string()) + "\",\"listen_addrs\":[" +
                  endpoint_json(p2p_endpoint_for(*local, value.local_peer())) + "],\"relay_addrs\":[" + relay_addrs +
                  "],\"relay_peer_id\":\"" + json_escape(relay_peer.to_string()) +
                  "\",\"native_relay_transport\":true,\"voucher\":" +
                  std::string{reservation.voucher ? "true" : "false"} + ",\"status\":\"ready\"}\n");

   const auto stop_file = std::filesystem::path{required(args, "stop-file")};
   while (!std::filesystem::exists(stop_file)) {
      std::this_thread::sleep_for(100ms);
   }
   forge::asio::blocking::run(runtime, value.async_stop());
   return 0;
}

std::string run_scenario(forge::asio::runtime& runtime, forge::net::p2p::node& value, std::string_view scenario,
                         std::string_view payload, const forge::net::p2p::peer_id& peer,
                         const forge::net::p2p::endpoint& remote, std::string_view target_peer_id = {}) {
   if (scenario == "ping") {
      const auto rtt = forge::asio::blocking::run(
          runtime,
          value.async_ping(peer, forge::net::p2p::node::open_options{.allow_relay = false, .allow_hole_punch = false}));
      return "\"rtt_ms\":" + std::to_string(rtt.count());
   }
   if (scenario == "identify") {
      const auto record = value.peers().find(peer);
      if (!record || record->protocols.empty() || record->signed_peer_record.empty()) {
         auto detail = std::string{"FORGE Identify did not retain a verified signed peer record"};
         if (record) {
            detail += " (protocols=" + std::to_string(record->protocols.size()) +
                      ", signed-bytes=" + std::to_string(record->signed_peer_record.size()) + ")";
         }
         const auto snapshot = value.diagnostics();
         const auto session =
             std::ranges::find(snapshot.sessions, peer, &forge::net::p2p::diagnostics::session::remote_peer);
         if (session != snapshot.sessions.end() && !session->identify_error.empty()) {
            detail += ": " + session->identify_error;
         }
         throw std::runtime_error{std::move(detail)};
      }
      return "\"protocol_count\":" + std::to_string(record->protocols.size()) + ",\"agent_version\":\"" +
             json_escape(record->agent_version) + "\",\"signed_peer_record\":true";
   }
   if (scenario == "relay_reserve") {
      const auto started = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch());
      const auto reservation = forge::asio::blocking::run(runtime, value.async_reserve_relay(peer));
      const auto received = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch());
      const auto snapshot = value.diagnostics();
      const auto session =
          std::ranges::find(snapshot.sessions, reservation.relay_peer, &forge::net::p2p::diagnostics::session::remote_peer);
      if (reservation.relay_peer != peer || session == snapshot.sessions.end() || !session->remote_endpoint) {
         throw std::runtime_error{"FORGE reservation lacks its authenticated relay session"};
      }
      auto addresses = std::string{"["};
      for (const auto& address : reservation.relay_endpoints) {
         if (addresses.size() > 1) {
            addresses += ',';
         }
         addresses += endpoint_json(address);
      }
      addresses += ']';
      return "\"reservation_basis\":\"forge.node.async_reserve_relay\",\"relay_peer_id\":\"" +
             json_escape(reservation.relay_peer.to_string()) + "\",\"reservation_client_peer_id\":\"" +
             json_escape(value.local_peer().to_string()) + "\",\"authenticated_remote_peer_id\":\"" +
             json_escape(session->remote_peer.to_string()) + "\",\"authenticated_remote_address\":" +
             endpoint_json(*session->remote_endpoint) + ",\"reservation_started_at_unix_seconds\":" +
             std::to_string(started.count()) + ",\"reservation_received_at_unix_seconds\":" +
             std::to_string(received.count()) + ",\"reservation_expires_at_unix_seconds\":" +
             std::to_string(reservation.expires_at.count()) + ",\"relay_endpoints\":" + addresses +
             ",\"voucher_present\":" + (reservation.voucher ? "true" : "false") + ",\"voucher_bytes\":" +
             std::to_string(reservation.voucher ? reservation.voucher->encode().size() : 0U);
   }
   if (scenario == "dht_find_peer") {
      const auto result =
          forge::asio::blocking::run(runtime, value.async_find_peer(forge::net::p2p::builtins::kad_dht, peer));
      return "\"closest_peers\":" + std::to_string(result.closest_peers.size()) +
             ",\"complete\":" + std::string{result.complete ? "true" : "false"};
   }
   if (scenario == "dht_hidden_find_peer") {
      const auto target = forge::net::p2p::peer_id::from_string(std::string{target_peer_id});
      const auto target_key = forge::net::p2p::make_dht_key(target);
      if (target == peer) {
         throw std::runtime_error{"hidden target must differ from the known routing peer"};
      }
      if (value.peers().find(target)) {
         throw std::runtime_error{"hidden target was present before FORGE FindPeer"};
      }
      const auto result =
          forge::asio::blocking::run(runtime, value.async_find_peer(forge::net::p2p::builtins::kad_dht, target));
      const auto found = std::ranges::find_if(result.closest_peers, [&](const forge::net::p2p::dht::peer& candidate) {
         return candidate.id == target && !candidate.endpoints.empty();
      });
      if (found == result.closest_peers.end()) {
         throw std::runtime_error{"FORGE FindPeer did not return the hidden target"};
      }
      const auto wire_address = raw_find_node_address(runtime, value, peer, target);
      const auto querier_text = json_escape(value.local_peer().to_string());
      const auto seed_text = json_escape(peer.to_string());
      const auto target_text = json_escape(target.to_string());
      const auto target_key_text = forge::codec::hex::encode(target_key.bytes);
      const auto address_text = json_escape(wire_address);
      return "\"preexisting_target\":false,\"found_peer\":\"" + json_escape(target.to_string()) +
             "\",\"addr_count\":" + std::to_string(found->endpoints.size()) +
             ",\"negotiated_protocol\":\"/ipfs/kad/1.0.0\",\"find_peer_proof\":{\"schema\":"
             "\"forge.libp2p.hidden-find-peer-network-proof.v1\",\"api\":\"async_find_peer\",\"api_succeeded\":true,"
             "\"querier_peer\":\"" + querier_text + "\",\"seed_peer\":\"" + seed_text + "\",\"target_peer\":\"" + target_text +
             "\",\"target_key\":\"" + target_key_text + "\",\"preexisting_target\":false,"
             "\"api_returned_target\":\"" + target_text + "\",\"address\":\"" + address_text +
             "\",\"protocol\":\"/ipfs/kad/1.0.0\",\"query_context\":\"fresh_hidden_target\","
             "\"wire_confirmation\":{\"kind\":\"forge_find_node_wire_reply\",\"explicit_wire_confirmation\":true,"
             "\"seed_peer\":\"" + seed_text + "\",\"target_peer\":\"" + target_text +
             "\",\"target_key\":\"" + target_key_text + "\",\"returned_target_peer\":\"" + target_text +
             "\",\"address\":\"" + address_text +
             "\",\"opened_stream_protocol\":\"/ipfs/kad/1.0.0\"}}";
   }
   if (scenario == "dht_provide_find_provider") {
      const auto provider_identity = generate_libp2p_identity();
      const auto querier_identity = generate_libp2p_identity();
      const auto key = forge::net::p2p::make_dht_key(provider_identity.peer);
      const auto key_text = forge::codec::hex::encode(key.bytes);
      auto provider = forge::net::p2p::node{runtime, node_options({}, provider_identity)};
      auto querier = forge::net::p2p::node{runtime, node_options({}, querier_identity)};
      const auto stop_nodes = [&] {
         auto failures = std::string{};
         const auto stop = [&](forge::net::p2p::node& node, std::string_view name) {
            try {
               forge::asio::blocking::run(runtime, node.async_stop());
            } catch (const std::exception& error) {
               if (!failures.empty()) {
                  failures += "; ";
               }
               failures += std::string{name} + ": " + error.what();
            } catch (...) {
               if (!failures.empty()) {
                  failures += "; ";
               }
               failures += std::string{name} + ": non-standard exception";
            }
         };
         stop(querier, "querier stop failed");
         stop(provider, "provider stop failed");
         if (!failures.empty()) {
            throw std::runtime_error{"provider fixture cleanup failed: " + failures};
         }
      };
      try {
         forge::asio::blocking::run(runtime, provider.async_hydrate_peer_state());
         forge::asio::blocking::run(runtime, provider.async_listen(loopback_quic_endpoint()));
         provider.peers().learn_endpoint(
             peer, remote, forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic});
         static_cast<void>(forge::asio::blocking::run(
             runtime, provider.async_connect(
                          remote, forge::net::p2p::node::connect_options{
                                      .expected_peer = peer, .allow_relay = false, .allow_hole_punch = false})));
         const auto provider_peer = provider.local_peer();
         if (provider_peer == querier_identity.peer || provider_peer == peer || querier_identity.peer == peer) {
            throw std::runtime_error{"DHT provider proof requires pairwise independent P, Q and S identities"};
         }
         auto registration =
             forge::asio::blocking::run(runtime, provider.async_provide(forge::net::p2p::builtins::kad_dht, key));
         if (!registration.active()) {
            throw std::runtime_error{"FORGE DHT provider registration did not become active"};
         }
         forge::asio::blocking::run(runtime, querier.async_hydrate_peer_state());
         forge::asio::blocking::run(runtime, querier.async_listen(loopback_quic_endpoint()));
         if (querier.peers().find(provider_peer)) {
            throw std::runtime_error{"FORGE querier learned provider before its DHT lookup"};
         }
         querier.peers().learn_endpoint(
             peer, remote, forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic});
         static_cast<void>(forge::asio::blocking::run(
             runtime, querier.async_connect(
                          remote, forge::net::p2p::node::connect_options{
                                      .expected_peer = peer, .allow_relay = false, .allow_hole_punch = false})));
         constexpr auto retry_interval = 50ms;
         const auto deadline = std::chrono::steady_clock::now() + 5s;
         while (true) {
            if (!registration.active()) {
               throw std::runtime_error{"FORGE provider registration stopped while querier lookup was active"};
            }
            const auto providers = forge::asio::blocking::run(
                runtime, querier.async_find_providers(forge::net::p2p::builtins::kad_dht, key,
                                                      {.requested_count = 1, .quorum = 1, .timeout = 1s}));
            const auto found = std::ranges::find(providers, provider_peer, &forge::net::p2p::dht::peer::id);
            if (found != providers.end()) {
               if (found->endpoints.empty() || !std::ranges::all_of(found->endpoints, [&](const auto& address) {
                      const auto& components = address.components();
                      return !components.empty() &&
                             components.back().code == forge::multiformats::protocol_code::p2p &&
                              components.back().value == provider_peer.to_string();
                   })) {
                  throw std::runtime_error{"FORGE DHT provider query did not preserve provider-bound endpoints"};
               }
               break;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
               throw std::runtime_error{"FORGE DHT provider query did not return the independent provider"};
            }
            // ADD_PROVIDER is one-way, so wait for the listener's store to become visible to a different peer.
            std::this_thread::sleep_for(retry_interval);
         }
         if (!registration.active()) {
            throw std::runtime_error{"FORGE provider registration stopped during querier lookup"};
         }
         const auto wire_address = raw_get_providers_address(runtime, querier, peer, key, provider_peer);
         stop_nodes();
         const auto provider_text = json_escape(provider_peer.to_string());
         const auto querier_text = json_escape(querier_identity.peer.to_string());
         const auto listener_text = json_escape(peer.to_string());
         const auto address_text = json_escape(wire_address);
         return "\"network_proof\":{\"schema\":\"forge.libp2p.provider-network-proof.v1\","
                "\"implementation\":\"forge\",\"provider_peer\":\"" +
                provider_text + "\",\"querier_peer\":\"" + querier_text + "\",\"listener_peer\":\"" +
                listener_text + "\",\"provider_key\":\"" + key_text +
                "\",\"key_binding\":{\"kind\":\"provider_identity_multihash\",\"provider_peer\":\"" +
                provider_text + "\",\"provider_key\":\"" + key_text +
                "\",\"derived_per_run\":true},\"provider_registration\":{\"api\":\"async_provide\","
                "\"succeeded\":true,\"provider_peer\":\"" + provider_text + "\",\"provider_key\":\"" +
                key_text + "\"},\"api_lookup\":{\"api\":\"async_find_providers\",\"succeeded\":true,"
                "\"querier_peer\":\"" + querier_text + "\",\"returned_provider_peer\":\"" + provider_text +
                "\",\"provider_key\":\"" + key_text +
                "\"},\"address_proof\":{\"source\":\"get_providers_wire_reply\",\"provider_peer\":\"" +
                provider_text + "\",\"address\":\"" + address_text +
                "\"},\"protocol_proof\":{\"protocol\":\"/ipfs/kad/1.0.0\",\"derivation\":"
                "\"successful_async_open_protocol_stream_with_get_providers_wire_reply\",\"successful_query\":true,"
                "\"opened_stream_protocol\":\"/ipfs/kad/1.0.0\"},\"source_query_proof\":{\"kind\":"
                "\"forge_async_find_providers_result\",\"querier_peer\":\"" + querier_text +
                "\",\"returned_provider_peer\":\"" + provider_text + "\",\"provider_key\":\"" + key_text +
                "\",\"opened_stream_protocol\":\"/ipfs/kad/1.0.0\"},\"wire_proof\":{\"kind\":"
                "\"forge_get_providers_wire_reply\",\"explicit_wire_confirmation\":true,\"listener_peer\":\"" +
                listener_text + "\",\"returned_provider_peer\":\"" + provider_text + "\",\"address\":\"" +
                address_text + "\",\"opened_stream_protocol\":\"/ipfs/kad/1.0.0\"}}";
      } catch (...) {
         const auto primary = std::current_exception();
         try {
            stop_nodes();
         } catch (const std::exception& cleanup) {
            try {
               std::rethrow_exception(primary);
            } catch (const std::exception& error) {
               throw std::runtime_error{std::string{"FORGE provider evidence failed: "} + error.what() +
                                        "; cleanup also failed: " + cleanup.what()};
            } catch (...) {
               throw std::runtime_error{std::string{"FORGE provider evidence failed with non-standard exception; "} +
                                        "cleanup also failed: " + cleanup.what()};
            }
         }
         std::rethrow_exception(primary);
      }
   }
   if (is_dht_value_scenario(scenario)) {
      const auto fixture = value_fixture(scenario);
      const auto record = forge::net::p2p::dht::record{
          .key_value = fixture.key,
          .value = fixture.value,
      };
      auto accepted = std::size_t{};
      if (payload != "get_only") {
         const auto put = forge::asio::blocking::run(
             runtime, value.async_put_value(forge::net::p2p::builtins::kad_dht, record,
                                            {.requested_count = 20, .quorum = 1, .timeout = 20s}));
         if (!put.quorum_reached || put.accepted == 0) {
            throw std::runtime_error{"FORGE DHT value publication did not reach remote quorum"};
         }
         accepted = put.accepted;
      }
      if (payload == "put_only") {
         return "\"operation\":\"put_only\",\"accepted\":" + std::to_string(accepted) +
                ",\"value_bytes\":" + std::to_string(fixture.value.size());
      }
      const auto get = forge::asio::blocking::run(
          runtime, value.async_get_value(forge::net::p2p::builtins::kad_dht, fixture.key,
                                         {.requested_count = 20, .quorum = 1, .timeout = 20s}));
      if (!get.quorum_reached || !get.selected || get.selected->value != fixture.value) {
         throw std::runtime_error{"FORGE DHT value query did not return the published record"};
      }
      return "\"operation\":\"" + std::string{payload == "get_only" ? "get_only" : "put_get"} +
             "\",\"accepted\":" + std::to_string(accepted) + ",\"valid_records\":" + std::to_string(get.valid_records) +
             ",\"value_bytes\":" + std::to_string(fixture.value.size()) +
             ",\"remote_get\":" + std::string{payload == "get_only" ? "true" : "false"};
   }
   if (scenario == "rendezvous_register_discover") {
      const auto local = value.local_endpoint();
      if (!local) {
         throw std::runtime_error{"FORGE rendezvous scenario requires local endpoint"};
      }
      const auto endpoint = p2p_endpoint_for(*local, value.local_peer());
      const auto record = signed_rendezvous_record(
          local_identity(), endpoint,
          static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count()));
      const auto registration = forge::asio::blocking::run(
          runtime, value.async_rendezvous_register(peer, forge::net::p2p::rendezvous::register_request{
                                                             .namespace_name = std::string{rendezvous_namespace},
                                                             .signed_peer_record = record,
                                                             .ttl = std::chrono::seconds{7'200},
                                                         }));
      if (registration.status_value != forge::net::p2p::rendezvous::status::ok) {
         throw std::runtime_error{"rendezvous register failed"};
      }
      if (registration.ttl != 7'200s) {
         throw std::runtime_error{"rendezvous register did not return the donor default TTL"};
      }
      const auto wire = raw_rendezvous_discover(runtime, value, peer);
      if (wire.registrations.size() != 1 || wire.cookie.empty()) {
         throw std::runtime_error{"rendezvous raw discover did not return exactly one registration and cookie"};
      }
      const auto& wire_registration = wire.registrations.front();
      if (wire_registration.namespace_name != rendezvous_namespace || wire_registration.peer != value.local_peer() ||
          wire_registration.ttl != 7'200s || wire_registration.signed_peer_record.empty()) {
         throw std::runtime_error{"rendezvous raw registration does not match the requested namespace, peer or TTL"};
      }
      const auto wire_record = forge::net::p2p::rendezvous::codec::open_peer_record(
          forge::net::p2p::signed_envelope::decode(wire_registration.signed_peer_record), value.local_peer());
      if (wire_record.sequence == 0 || wire_record.peer != value.local_peer() || wire_record.endpoints.size() != 1 ||
          wire_record.endpoints.front().to_string() != endpoint.to_string()) {
         throw std::runtime_error{"rendezvous raw signed peer record did not match the local peer and endpoint"};
      }
      const auto sanitized = forge::asio::blocking::run(
          runtime, value.async_rendezvous_discover(peer, forge::net::p2p::rendezvous::discover_request{
                                                             .namespace_name = std::string{rendezvous_namespace},
                                                             .limit = 10,
                                                         }));
      if (sanitized.status_value != forge::net::p2p::rendezvous::status::ok || !sanitized.registrations.empty()) {
         throw std::runtime_error{"rendezvous high-level discovery did not filter the third-party loopback record"};
      }
      return "\"negotiated_protocol\":\"/rendezvous/1.0.0\",\"wire_registration_count\":1,"
             "\"signed_peer_record_valid\":true,\"matching_peer_record\":true,\"record_sequence\":" +
             std::to_string(wire_record.sequence) +
             ",\"record_address_count\":" + std::to_string(wire_record.endpoints.size()) +
             ",\"registered_ttl_seconds\":" + std::to_string(registration.ttl.count()) +
             ",\"discovered_ttl_seconds\":" + std::to_string(wire_registration.ttl.count()) +
             ",\"cookie_bytes\":" + std::to_string(wire.cookie.size()) +
             ",\"sanitized_registration_count\":0,\"loopback_filtered\":true";
   }
   if (scenario == "rendezvous_lifecycle") {
      const auto local = value.local_endpoint();
      if (!local) {
         throw std::runtime_error{"FORGE rendezvous lifecycle requires local endpoint"};
      }
      const auto identity = local_identity();
      const auto initial_endpoint = p2p_endpoint_for(*local, value.local_peer());
      auto updated_endpoint = initial_endpoint;
      if (updated_endpoint.transport.port == std::numeric_limits<std::uint16_t>::max()) {
         throw std::runtime_error{"FORGE rendezvous lifecycle cannot derive updated endpoint"};
      }
      ++updated_endpoint.transport.port;
      constexpr auto timing_margin = 200ms;
      const auto register_record = [&](const forge::net::p2p::endpoint& record_endpoint, std::uint64_t sequence) {
         const auto registration = forge::asio::blocking::run(
             runtime, value.async_rendezvous_register(
                          peer, forge::net::p2p::rendezvous::register_request{
                                    .namespace_name = std::string{rendezvous_namespace},
                                    .signed_peer_record = signed_rendezvous_record(identity, record_endpoint, sequence),
                                    .ttl = 3s,
                                }));
         if (registration.status_value != forge::net::p2p::rendezvous::status::ok || registration.ttl < 2s ||
             registration.ttl > 3s) {
            throw std::runtime_error{"rendezvous lifecycle registration returned a TTL too short for bounded renewal"};
         }
         return registration;
      };

      const auto initial = register_record(initial_endpoint, 100);
      const auto first_discovery = raw_rendezvous_discover(runtime, value, peer);
      const auto initial_visible_count =
          rendezvous_registration_count(first_discovery, value.local_peer(), initial_endpoint, 100);
      if (first_discovery.cookie.empty() || initial_visible_count != 1) {
         throw std::runtime_error{"rendezvous lifecycle initial signed legacy record was not discoverable"};
      }

      const auto updated_requested_at = std::chrono::steady_clock::now();
      const auto updated = register_record(updated_endpoint, 101);
      const auto updated_confirmed_at = std::chrono::steady_clock::now();
      const auto delta_discovery = raw_rendezvous_discover(runtime, value, peer, first_discovery.cookie);
      const auto updated_visible_count =
          rendezvous_registration_count(delta_discovery, value.local_peer(), updated_endpoint, 101);
      if (delta_discovery.cookie.empty() || delta_discovery.cookie == first_discovery.cookie ||
          updated_visible_count != 1) {
         throw std::runtime_error{"rendezvous lifecycle did not return a cookie delta for the updated legacy record"};
      }

      const auto original_renewal_deadline = updated_requested_at + updated.ttl - timing_margin;
      const auto renewal_due = updated_confirmed_at + updated.ttl / 2;
      if (renewal_due >= original_renewal_deadline) {
         throw std::runtime_error{"rendezvous lifecycle TTL left no bounded renewal window"};
      }
      std::this_thread::sleep_until(renewal_due);
      const auto renewed_requested_at = std::chrono::steady_clock::now();
      if (renewed_requested_at >= original_renewal_deadline) {
         throw std::runtime_error{"rendezvous lifecycle renewal started after the original expiry window"};
      }
      const auto renewed = register_record(updated_endpoint, 102);
      const auto renewed_confirmed_at = std::chrono::steady_clock::now();
      if (renewed_confirmed_at >= original_renewal_deadline) {
         throw std::runtime_error{"rendezvous lifecycle renewal was not confirmed before original expiry"};
      }

      const auto original_expiry_latest = updated_confirmed_at + updated.ttl;
      const auto renewed_expiry_earliest = renewed_requested_at + renewed.ttl;
      const auto renewed_visibility_time = original_expiry_latest + timing_margin;
      const auto renewed_visibility_deadline = renewed_expiry_earliest - timing_margin;
      if (renewed_visibility_time >= renewed_visibility_deadline) {
         throw std::runtime_error{"rendezvous lifecycle TTL left no bounded post-expiry renewal proof window"};
      }
      std::this_thread::sleep_until(renewed_visibility_time);
      const auto renewed_visibility_started_at = std::chrono::steady_clock::now();
      if (renewed_visibility_started_at >= renewed_visibility_deadline) {
         throw std::runtime_error{"rendezvous lifecycle renewal visibility probe started after renewed expiry"};
      }
      const auto renewed_discovery = raw_rendezvous_discover(runtime, value, peer);
      const auto renewed_visibility_completed_at = std::chrono::steady_clock::now();
      if (renewed_visibility_completed_at >= renewed_visibility_deadline) {
         throw std::runtime_error{"rendezvous lifecycle renewal visibility probe completed after renewed expiry"};
      }
      const auto renewed_visible_count =
          rendezvous_registration_count(renewed_discovery, value.local_peer(), updated_endpoint, 102);
      if (renewed_visible_count != 1) {
         throw std::runtime_error{
             "rendezvous lifecycle renewed signed legacy record was not visible after original expiry"};
      }

      std::this_thread::sleep_until(renewed_confirmed_at + renewed.ttl + timing_margin);
      const auto expired_discovery = raw_rendezvous_discover(runtime, value, peer);
      const auto expired_registration_count = rendezvous_peer_registration_count(expired_discovery, value.local_peer());
      if (expired_registration_count != 0) {
         throw std::runtime_error{"rendezvous lifecycle registration did not expire after the returned TTL"};
      }

      const auto pre_unregister_requested_at = std::chrono::steady_clock::now();
      const auto pre_unregister = register_record(updated_endpoint, 103);
      const auto pre_unregister_deadline = pre_unregister_requested_at + pre_unregister.ttl - timing_margin;
      const auto pre_unregister_discovery = raw_rendezvous_discover(runtime, value, peer);
      const auto pre_unregister_count =
          rendezvous_registration_count(pre_unregister_discovery, value.local_peer(), updated_endpoint, 103);
      if (pre_unregister_count != 1) {
         throw std::runtime_error{"rendezvous lifecycle registration was not visible before unregister"};
      }
      raw_rendezvous_unregister(runtime, value, peer);
      const auto final_discovery = raw_rendezvous_discover(runtime, value, peer);
      if (std::chrono::steady_clock::now() >= pre_unregister_deadline) {
         throw std::runtime_error{"rendezvous lifecycle unregister proof exceeded the registration TTL window"};
      }
      const auto final_registration_count = rendezvous_peer_registration_count(final_discovery, value.local_peer());
      if (final_registration_count != 0) {
         throw std::runtime_error{"rendezvous lifecycle unregister did not remove the confirmed registration"};
      }
      return "\"negotiated_protocol\":\"/rendezvous/1.0.0\",\"legacy_signed_peer_record\":true,"
             "\"initial_record_sequence\":100,\"updated_record_sequence\":101,\"renewed_record_sequence\":102,"
             "\"initial_ttl_seconds\":" +
             std::to_string(initial.ttl.count()) + ",\"updated_ttl_seconds\":" + std::to_string(updated.ttl.count()) +
             ",\"renewed_ttl_seconds\":" + std::to_string(renewed.ttl.count()) +
             ",\"initial_cookie_bytes\":" + std::to_string(first_discovery.cookie.size()) +
             ",\"delta_cookie_bytes\":" + std::to_string(delta_discovery.cookie.size()) +
             ",\"cookie_changed\":true,\"initial_visible_count\":" + std::to_string(initial_visible_count) +
             ",\"updated_visible_count\":" + std::to_string(updated_visible_count) +
             ",\"renewed_visible_after_original_expiry\":true,\"renewed_visible_count\":" +
             std::to_string(renewed_visible_count) +
             ",\"expired_registration_count\":" + std::to_string(expired_registration_count) +
             ",\"pre_unregister_record_sequence\":103,"
             "\"pre_unregister_count\":" +
             std::to_string(pre_unregister_count) +
             ",\"final_registration_count\":" + std::to_string(final_registration_count);
   }
   if (scenario == "gossipsub_publish" || scenario == "gossipsub_mixed_mesh_stress") {
      prepare_pubsub_publisher(runtime, value);
      const auto message =
          forge::asio::blocking::run(runtime, value.async_publish(
                                                  forge::net::p2p::pubsub::topic{
                                                      .value = std::string{pubsub_topic},
                                                  },
                                                  std::vector<std::uint8_t>{payload.begin(), payload.end()}));
      std::this_thread::sleep_for(2s);
      return "\"topic\":\"" + json_escape(message.subject.value) +
             "\",\"payload_bytes\":" + std::to_string(message.data.size()) +
             ",\"signed\":" + std::string{message.signature.empty() ? "false" : "true"};
   }
   if (scenario == "echo" || scenario == "echo_large" || scenario == "pnet") {
      auto stream = forge::asio::blocking::run(
          runtime, value.async_open_protocol_stream(
                       peer, forge::net::p2p::protocol_id{.value = std::string{echo_protocol}},
                       forge::net::p2p::node::open_options{.allow_relay = false, .allow_hole_punch = false}));
      auto bytes = std::vector<std::uint8_t>{payload.begin(), payload.end()};
      if (scenario == "echo_large") {
         bytes.resize(large_echo_payload_size);
         for (auto index = std::size_t{}; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::uint8_t>(index % 251U);
         }
      }
      const auto request = wrap_length_delimited(bytes);
      auto response = std::vector<std::uint8_t>{};
      const auto stream_id = stream.id();
      const auto authentication = stream.authentication();
      forge::asio::blocking::run(runtime, stream.async_write(request));
      const auto echoed = forge::asio::blocking::run(runtime, read_length_delimited(stream, maximum_echo_payload, &response));
      if (echoed != bytes) {
         throw std::runtime_error{"FORGE echo mismatch"};
      }
      forge::asio::blocking::run(runtime, stream.async_close());
      auto evidence = "\"protocol\":\"" + json_escape(echo_protocol) + "\",\"payload_bytes\":" +
                      std::to_string(echoed.size()) + ",\"echo_ok\":true" +
                      ",\"application_stream_id\":" + std::to_string(stream_id) +
                      ",\"application_protocol\":\"" + json_escape(echo_protocol) +
                      "\",\"application_close_returned\":true,\"application_request_framed_bytes\":" +
                      std::to_string(request.size()) + ",\"application_response_framed_bytes\":" +
                      std::to_string(response.size()) + ",\"application_request_framed_sha256\":\"" +
                      forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{request}).str() +
                      "\",\"application_response_framed_sha256\":\"" +
                      forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{response}).str() + "\"";
      if (remote.transport.protocol != forge::net::p2p::endpoint::protocol_kind::quic_v1) {
         evidence += ",\"negotiated_security\":\"" + std::string{negotiated_security(authentication)} +
                     "\",\"negotiated_muxer\":\"/yamux/1.0.0\"";
      }
      return evidence;
   }
   if (scenario == "dcutr") {
      const auto status = forge::asio::blocking::run(runtime, value.async_attempt_hole_punch(peer));
      return "\"hole_punch_status\":" + std::to_string(static_cast<int>(status));
   }
   if (scenario == "unknown_protocol") {
      try {
         (void)forge::asio::blocking::run(
             runtime, value.async_open_protocol_stream(
                          peer, forge::net::p2p::protocol_id{.value = "/forge/interop/unknown/1"},
                          forge::net::p2p::node::open_options{.allow_relay = false, .allow_hole_punch = false}));
      } catch (const forge::exceptions::base& error) {
         if (forge::net::p2p::exceptions::code_of(error).value() !=
             forge::net::p2p::exceptions::code::unsupported_protocol) {
            throw;
         }
         return "\"expected_error\":\"" + json_escape(error.what()) + "\"";
      }
      throw std::runtime_error{"unknown protocol unexpectedly succeeded"};
   }
   throw std::runtime_error{"unknown FORGE fixture scenario: " + std::string{scenario}};
}

int dial_mode(const std::map<std::string, std::string>& args) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto scenario = required(args, "scenario");
   const auto transport = optional_value(args, "transport", "quic");
   auto options = node_options(required(args, "store-dir"));
   configure_dns_server(options, args);
   configure_private_network(options, args, transport);
   auto value = forge::net::p2p::node{runtime, std::move(options)};
   forge::asio::blocking::run(runtime, value.async_hydrate_peer_state());
   forge::asio::blocking::run(runtime, value.async_listen(loopback_endpoint_for(transport)));

   const auto root = forge::multiformats::multiaddr::parse(required(args, "addr"));
   const auto dns_root = std::ranges::any_of(root.components(), [](const auto& component) {
      return component.code == forge::multiformats::protocol_code::dnsaddr;
   });
   if (dns_root && optional_value(args, "dns-server").empty()) {
      throw std::runtime_error{"DNSADDR fixture dial requires --dns-server"};
   }
   if (dns_root) {
      const auto peer = forge::net::p2p::peer_id::from_string(required(args, "peer-id"));
      auto session = forge::net::p2p::node::session_info{};
      try {
         session = forge::asio::blocking::run(
             runtime, value.async_connect(root, forge::net::p2p::node::connect_options{
                                                   .expected_peer = peer, .allow_relay = false, .allow_hole_punch = false}));
      } catch (const std::exception&) {
         if (scenario != "pnet" || optional_value(args, "pnet-control").empty()) {
            throw;
         }
         const auto metrics = value.metrics();
         forge::asio::blocking::run(runtime, value.async_stop());
         write_file(required(args, "result-file"),
                    "{" + pnet_rejection_evidence(args, "dialer", metrics, peer.to_string()) + "}\n");
         return 0;
      }
      if (scenario == "pnet" && !optional_value(args, "pnet-control").empty()) {
         throw std::runtime_error{"pnet rejection control unexpectedly established an authenticated session"};
      }
      if (session.remote_peer != peer || session.path != forge::net::p2p::path::kind::direct) {
         throw std::runtime_error{"DNSADDR dial did not authenticate the expected direct peer"};
      }
      if ((scenario == "identify" || scenario == "pnet") &&
          session.identify_state != forge::net::p2p::identify::state::identified) {
         throw std::runtime_error{"DNSADDR connection did not complete authenticated Identify"};
      }
      const auto snapshot = value.diagnostics();
      const auto observed = std::ranges::find_if(snapshot.sessions, [&](const auto& current) {
         return !current.closed && current.remote_peer == peer && current.remote_endpoint.has_value();
      });
      if (observed == snapshot.sessions.end()) {
         throw std::runtime_error{"DNSADDR dial has no observed authenticated connection"};
      }
      const auto details = run_scenario(
          runtime, value, scenario,
          optional_value(args, "payload", pubsub_payload), peer, *observed->remote_endpoint,
          optional_value(args, "target-peer-id"));
      const auto identify_evidence = scenario == "echo"
          ? run_scenario(runtime, value, "identify", {}, peer, *observed->remote_endpoint, {})
          : std::string{};
      const auto negotiated_transport = observed->remote_endpoint->transport.protocol ==
                                                forge::net::p2p::endpoint::protocol_kind::quic_v1
                                            ? "/quic-v1" : "tcp";
      forge::asio::blocking::run(runtime, value.async_stop());
      write_file(required(args, "result-file"),
                 "{\"implementation\":\"forge\",\"role\":\"dialer\",\"scenario\":\"" + json_escape(scenario) +
                 "\",\"status\":\"ok\",\"dns_resolver_configured\":true,\"dns_input_address\":\"" +
                 json_escape(required(args, "addr")) +
                 "\",\"expected_peer_id\":\"" + json_escape(peer.to_string()) +
                 "\",\"authenticated_remote_peer_id\":\"" + json_escape(session.remote_peer.to_string()) +
                 "\",\"dns_server\":\"" + json_escape(optional_value(args, "dns-server")) +
                 "\",\"connection_remote_addr\":\"" + json_escape(observed->remote_endpoint->to_string()) +
                 "\",\"negotiated_transport\":\"" + negotiated_transport + "\"," + details +
                 (identify_evidence.empty() ? "" : "," + identify_evidence) +
                 (scenario == "pnet" ? ",\"identify_observed\":true" : "") +
                 (transport == "tcp-pnet" ? "," + pnet_evidence(args) : "") + "}\n");
      return 0;
   }

   auto remote = forge::net::p2p::parse_endpoint(required(args, "addr"));
   auto peer = forge::net::p2p::peer_id::from_string(required(args, "peer-id"));
   const auto pnet_profile = transport == "tcp-pnet";
   const auto direct_evidence = scenario == "identify" || scenario == "echo";
   if (direct_evidence) {
      forge::test::libp2p_interop::require_fresh_identify(value, peer);
   }
   value.peers().learn_endpoint(
       peer, remote,
       forge::net::p2p::capability_set{
           .bits = pnet_profile
                       ? forge::net::p2p::capabilities::peer_exchange
                       : forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::peer_exchange |
                             forge::net::p2p::capabilities::autonat | forge::net::p2p::capabilities::relay |
                             forge::net::p2p::capabilities::hole_punching |
                             forge::net::p2p::capabilities::relay_reservation |
                             forge::net::p2p::capabilities::rendezvous | forge::net::p2p::capabilities::pubsub});

   auto connection_evidence = std::string{};
   auto captured_connection = std::optional<forge::net::p2p::diagnostics::session>{};
   if (direct_evidence || scenario.starts_with("dht_") || scenario == "gossipsub_publish" ||
       scenario == "gossipsub_mixed_mesh_stress" || scenario == "pnet") {
      auto session = forge::net::p2p::node::session_info{};
      try {
         session = forge::asio::blocking::run(
             runtime,
             value.async_connect(remote, forge::net::p2p::node::connect_options{
                                             .expected_peer = peer, .allow_relay = false, .allow_hole_punch = false}));
      } catch (const std::exception&) {
         if (scenario != "pnet" || optional_value(args, "pnet-control").empty()) {
            throw;
         }
         const auto metrics = value.metrics();
         forge::asio::blocking::run(runtime, value.async_stop());
         write_file(required(args, "result-file"),
                    "{" + pnet_rejection_evidence(args, "dialer", metrics, peer.to_string()) + "}\n");
         return 0;
      }
      if (scenario == "pnet" && !optional_value(args, "pnet-control").empty()) {
         forge::asio::blocking::run(runtime, value.async_stop());
         throw std::runtime_error{"pnet rejection control unexpectedly established an authenticated session"};
      }
      if (scenario == "identify" && session.identify_state != forge::net::p2p::identify::state::identified) {
         throw std::runtime_error{"FORGE automatic Identify did not complete, state=" +
                                  std::to_string(static_cast<int>(session.identify_state))};
      }
      if (direct_evidence) {
         if (session.remote_peer != peer || session.path != forge::net::p2p::path::kind::direct) {
            throw std::runtime_error{"FORGE Identify connection did not retain the authenticated direct peer"};
         }
         captured_connection = forge::test::libp2p_interop::capture_identified_connection(value, peer, remote);
         const auto transport = remote.transport.protocol == forge::net::p2p::endpoint::protocol_kind::quic_v1
                                    ? std::string_view{"/quic-v1"}
                                    : std::string_view{"tcp"};
         connection_evidence = "\"negotiated_transport\":\"" + std::string{transport} +
                               "\",\"authenticated_remote_peer_id\":\"" + json_escape(session.remote_peer.to_string()) +
                               "\",\"local_peer_id\":\"" + json_escape(value.local_peer().to_string()) +
                               "\",\"connection_remote_addr\":\"" + json_escape(captured_connection->remote_endpoint->to_string()) +
                               "\",\"application_connection_id\":" + std::to_string(captured_connection->id) +
                               ",\"identify_event_connection_id\":" + std::to_string(captured_connection->id) +
                               ",\"identify_event_basis\":\"automatic_identify_single_fresh_connection\"";
      }
      if (scenario == "pnet") {
         if (session.identify_state != forge::net::p2p::identify::state::identified ||
             session.remote_peer != peer || session.path != forge::net::p2p::path::kind::direct) {
            throw std::runtime_error{"FORGE pnet connection did not complete authenticated direct Identify"};
         }
         connection_evidence = "\"negotiated_transport\":\"tcp\",\"authenticated_remote_peer_id\":\"" +
                               json_escape(session.remote_peer.to_string()) + "\",\"identify_observed\":true";
      }
   }

   const auto details = run_scenario(runtime, value, scenario, optional_value(args, "payload", pubsub_payload), peer,
                                     remote, optional_value(args, "target-peer-id"));
   const auto identify_evidence = scenario == "echo"
       ? run_scenario(runtime, value, "identify", {}, peer, remote) : std::string{};
   if (captured_connection) {
      forge::test::libp2p_interop::require_same_connection(value, *captured_connection);
      connection_evidence += ",\"single_fresh_connection_retained\":true";
   }
   forge::asio::blocking::run(runtime, value.async_stop());
   write_file(required(args, "result-file"), "{\"implementation\":\"forge\",\"role\":\"dialer\",\"scenario\":\"" +
                                                 json_escape(scenario) + "\",\"status\":\"ok\"," + details +
                                                 (connection_evidence.empty() ? "" : "," + connection_evidence) +
                                                 (identify_evidence.empty() ? "" : "," + identify_evidence) +
                                                 (scenario == "pnet" ? "," + pnet_evidence(args) : "") +
                                                 "}\n");
   return 0;
}

int dial_relay_mode(const std::map<std::string, std::string>& args) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto value = forge::net::p2p::node{runtime, node_options(required(args, "store-dir"))};
   forge::asio::blocking::run(runtime, value.async_listen(loopback_quic_endpoint()));

   const auto relay_addr = forge::net::p2p::parse_endpoint(required(args, "relay-addr"));
   const auto relay_peer = forge::net::p2p::peer_id::from_string(required(args, "relay-peer-id"));
   const auto target_peer = forge::net::p2p::peer_id::from_string(required(args, "peer-id"));
   value.peers().learn_endpoint(
       relay_peer, relay_addr,
       forge::net::p2p::capability_set{
           .bits = forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::relay |
                   forge::net::p2p::capabilities::relay_reservation | forge::net::p2p::capabilities::hole_punching});

   auto status = forge::net::p2p::hole_punch::status::failed;
   auto relay_echo = false;
   if (required(args, "scenario") == "relay_echo_topology") {
      auto stream = forge::asio::blocking::run(
          runtime, value.async_open_protocol_stream(target_peer,
                                                    forge::net::p2p::protocol_id{.value = std::string{echo_protocol}},
                                                    forge::net::p2p::node::open_options{
                                                        .allow_relay = true,
                                                        .relay_peer = relay_peer,
                                                        .timeout = 15s,
                                                        .allow_hole_punch = false,
                                                    }));
      const auto payload = std::vector<std::uint8_t>{'r', 'e', 'l', 'a', 'y', '-', 'e', 'c', 'h', 'o'};
      forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(payload)));
      const auto echoed = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024));
      if (echoed != payload) {
         throw std::runtime_error{"FORGE relay echo mismatch"};
      }
      relay_echo = true;
   } else {
      status = forge::asio::blocking::run(runtime, value.async_attempt_hole_punch(target_peer, relay_peer, 15s));
      relay_echo = status == forge::net::p2p::hole_punch::status::succeeded;
   }
   const auto metrics = value.metrics();
   forge::asio::blocking::run(runtime, value.async_stop());
   write_file(required(args, "result-file"),
              "{\"implementation\":\"forge\",\"role\":\"relay_dialer\",\"scenario\":\"" +
                  json_escape(required(args, "scenario")) + "\",\"status\":\"ok\",\"relay_peer\":\"" +
                  json_escape(relay_peer.to_string()) + "\",\"target_peer\":\"" + json_escape(target_peer.to_string()) +
                  "\",\"hole_punch_status\":" + std::to_string(static_cast<int>(status)) +
                  ",\"hole_punch_successes\":" + std::to_string(metrics.hole_punch_successes) +
                  ",\"hole_punch_failures\":" + std::to_string(metrics.hole_punch_failures) +
                  ",\"path_relay_attempts\":" + std::to_string(metrics.path_relay_attempts) +
                  ",\"path_relay_opens\":" + std::to_string(metrics.path_relay_opens) +
                  ",\"relay_failures\":" + std::to_string(metrics.relay_failures) +
                  ",\"direct_failures\":" + std::to_string(metrics.direct_failures) +
                  ",\"relay_echo\":" + std::string{relay_echo ? "true" : "false"} +
                  ",\"relay_bytes\":" + std::to_string(metrics.relay_bytes) + "}\n");
   if (required(args, "scenario") == "dcutr_relay_topology" &&
       status != forge::net::p2p::hole_punch::status::succeeded) {
      throw std::runtime_error{"FORGE relay topology DCUtR did not succeed"};
   }
   return 0;
}

int topology_mode(const std::map<std::string, std::string>& args) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 6}};
   const auto root = std::filesystem::path{required(args, "store-dir")};
   std::filesystem::create_directories(root);

   const auto relay_identity = generate_libp2p_identity();
   const auto source_identity = generate_libp2p_identity();
   const auto destination_identity = generate_libp2p_identity();

   auto relay_options = node_options(root / "relay-store", relay_identity);
   relay_options.capabilities = forge::net::p2p::capability_set{
       .bits = forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::relay |
               forge::net::p2p::capabilities::relay_reservation | forge::net::p2p::capabilities::hole_punching};
   auto source_options = node_options(root / "source-store", source_identity);
   source_options.capabilities = forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic |
                                                                         forge::net::p2p::capabilities::hole_punching};
   auto destination_options = node_options(root / "destination-store", destination_identity);
   destination_options.capabilities = forge::net::p2p::capability_set{
       .bits = forge::net::p2p::capabilities::direct_quic | forge::net::p2p::capabilities::relay_reservation |
               forge::net::p2p::capabilities::hole_punching};

   auto relay = forge::net::p2p::node{runtime, std::move(relay_options)};
   auto source = forge::net::p2p::node{runtime, std::move(source_options)};
   auto destination = forge::net::p2p::node{runtime, std::move(destination_options)};
   register_echo(destination);

   forge::asio::blocking::run(runtime, relay.async_listen(loopback_quic_endpoint()));
   forge::asio::blocking::run(runtime, source.async_listen(loopback_quic_endpoint()));
   forge::asio::blocking::run(runtime, destination.async_listen(loopback_quic_endpoint()));

   const auto relay_endpoint = relay.local_endpoint();
   const auto source_endpoint = source.local_endpoint();
   const auto destination_endpoint = destination.local_endpoint();
   if (!relay_endpoint || !source_endpoint || !destination_endpoint) {
      throw std::runtime_error{"FORGE topology failed to start all listeners"};
   }

   source.peers().learn_endpoint(
       relay.local_peer(), *relay_endpoint,
       forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic |
                                               forge::net::p2p::capabilities::relay |
                                               forge::net::p2p::capabilities::relay_reservation});
   destination.peers().learn_endpoint(
       relay.local_peer(), *relay_endpoint,
       forge::net::p2p::capability_set{.bits = forge::net::p2p::capabilities::direct_quic |
                                               forge::net::p2p::capabilities::relay |
                                               forge::net::p2p::capabilities::relay_reservation});

   const auto reservation = forge::asio::blocking::run(runtime, destination.async_reserve_relay(relay.local_peer()));
   auto status = forge::net::p2p::hole_punch::status::failed;
   auto relay_echo = false;
   if (required(args, "scenario") == "relay_echo_topology") {
      auto stream = forge::asio::blocking::run(
          runtime, source.async_open_protocol_stream(destination.local_peer(),
                                                     forge::net::p2p::protocol_id{.value = std::string{echo_protocol}},
                                                     forge::net::p2p::node::open_options{
                                                         .allow_relay = true,
                                                         .relay_peer = relay.local_peer(),
                                                         .timeout = 10s,
                                                         .allow_hole_punch = false,
                                                     }));
      const auto payload = std::vector<std::uint8_t>{'r', 'e', 'l', 'a', 'y', '-', 'e', 'c', 'h', 'o'};
      forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(payload)));
      relay_echo = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024)) == payload;
      if (!relay_echo) {
         throw std::runtime_error{"FORGE topology relay echo mismatch"};
      }
   } else {
      status = forge::asio::blocking::run(
          runtime, source.async_attempt_hole_punch(destination.local_peer(), relay.local_peer(), 10s));
      if (status == forge::net::p2p::hole_punch::status::succeeded) {
         auto stream = forge::asio::blocking::run(
             runtime, source.async_open_protocol_stream(
                          destination.local_peer(), forge::net::p2p::protocol_id{.value = std::string{echo_protocol}},
                          forge::net::p2p::node::open_options{
                              .allow_relay = false,
                              .timeout = 10s,
                              .direct_attempt_timeout = 10s,
                              .max_direct_endpoints = 4,
                              .allow_hole_punch = false,
                          }));
         const auto payload = std::vector<std::uint8_t>{'d', 'c', 'u', 't', 'r', '-', 'e', 'c', 'h', 'o'};
         forge::asio::blocking::run(runtime, stream.async_write(wrap_length_delimited(payload)));
         relay_echo = forge::asio::blocking::run(runtime, read_length_delimited(stream, 16 * 1024)) == payload;
         forge::asio::blocking::run(runtime, stream.async_close());
      }
   }

   const auto source_metrics = source.metrics();
   const auto relay_metrics = relay.metrics();
   const auto destination_metrics = destination.metrics();
   if (required(args, "scenario") == "dcutr_relay_topology" &&
       (status != forge::net::p2p::hole_punch::status::succeeded || !relay_echo ||
        source_metrics.hole_punch_successes == 0 || relay_metrics.relay_bytes == 0)) {
      throw std::runtime_error{"FORGE local DCUtR topology did not prove a successful direct echo"};
   }
   write_file(
       required(args, "result-file"),
       "{\"implementation\":\"forge\",\"role\":\"topology\",\"scenario\":\"" + json_escape(required(args, "scenario")) +
           "\","
           "\"status\":\"ok\",\"relay_peer\":\"" +
           json_escape(relay.local_peer().to_string()) + "\",\"source_peer\":\"" +
           json_escape(source.local_peer().to_string()) + "\",\"destination_peer\":\"" +
           json_escape(destination.local_peer().to_string()) + "\",\"relay_addr\":\"" +
           json_escape(p2p_endpoint_for(*relay_endpoint, relay.local_peer()).to_string()) + "\",\"source_addr\":\"" +
           json_escape(p2p_endpoint_for(*source_endpoint, source.local_peer()).to_string()) +
           "\",\"destination_addr\":\"" +
           json_escape(p2p_endpoint_for(*destination_endpoint, destination.local_peer()).to_string()) +
           "\",\"reservation_voucher_bytes\":" +
           std::to_string(reservation.voucher ? reservation.voucher->encode().size() : 0U) + ",\"hole_punch_status\":" +
           std::to_string(static_cast<int>(status)) + ",\"relay_echo\":" + std::string{relay_echo ? "true" : "false"} +
           ",\"source_hole_punch_successes\":" + std::to_string(source_metrics.hole_punch_successes) +
           ",\"relay_bytes\":" + std::to_string(relay_metrics.relay_bytes) +
           ",\"destination_hole_punch_attempts\":" + std::to_string(destination_metrics.hole_punch_attempts) + "}\n");

   forge::asio::blocking::run(runtime, destination.async_stop());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, relay.async_stop());
   return 0;
}

int build_info_mode() {
   const auto exact_identity =
       std::string{"git:"} + FORGE_INTEROP_BUILD_FORGE_HEAD + ";worktree-sha256:" + FORGE_INTEROP_BUILD_WORKTREE_SHA256;
   std::cout << "{\"schema_version\":2,\"forge\":{\"head\":\"" << json_escape(FORGE_INTEROP_BUILD_FORGE_HEAD)
             << "\",\"worktree_sha256\":\"" << json_escape(FORGE_INTEROP_BUILD_WORKTREE_SHA256)
             << "\",\"dirty\":" << (FORGE_INTEROP_BUILD_WORKTREE_DIRTY != 0 ? "true" : "false")
             << ",\"exact_identity\":\"" << json_escape(exact_identity) << "\"},\"compiler\":{\"path\":\""
             << json_escape(FORGE_INTEROP_BUILD_COMPILER_PATH) << "\",\"id\":\""
             << json_escape(FORGE_INTEROP_BUILD_COMPILER_ID) << "\",\"version\":\""
             << json_escape(FORGE_INTEROP_BUILD_COMPILER_VERSION) << "\"},\"build_profile\":\""
             << json_escape(FORGE_INTEROP_BUILD_PROFILE) << "\"}\n";
   return 0;
}

} // namespace

int main(int argc, char** argv) {
   try {
      const auto args = parse_args(argc, argv);
      if (args.at("command") == "--self-test") {
         return dht_wire_self_test_mode();
      }
      if (args.at("command") == "build-info") {
         return build_info_mode();
      }
      const auto scenario = optional_value(args, "scenario");
      if ((args.at("command") == "listen" || args.at("command") == "dial") &&
          (scenario == "autonat_v1" || scenario == "autonat_v2")) {
         return forge::test::libp2p_interop::run_forge_autonat_fixture(
             args, {.make_node = make_autonat_node});
      }
      if (args.at("command") == "listen") {
         return listen_mode(args);
      }
      if (args.at("command") == "destination") {
         return destination_mode(args);
      }
      if (args.at("command") == "dial") {
         return dial_mode(args);
      }
      if (args.at("command") == "dial-relay") {
         return dial_relay_mode(args);
      }
      if (args.at("command") == "topology") {
         return topology_mode(args);
      }
      throw std::runtime_error{"unknown command: " + args.at("command")};
   } catch (const std::exception& error) {
      std::cerr << "forge_interop_fixture: " << error.what() << "\n";
      return 2;
   }
}
