#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.scoring;

#include "forge_autonat_fixture.hxx"

namespace forge::test::libp2p_interop {
namespace {

using namespace std::chrono_literals;
using forge::net::p2p::endpoint;
using forge::net::p2p::host_event;
using forge::net::p2p::node;
using forge::net::p2p::reachability;

[[nodiscard]] const std::string& required(const fixture_arguments& args, std::string_view key) {
   const auto found = args.find(std::string{key});
   if (found == args.end() || found->second.empty()) {
      throw std::runtime_error{"missing required argument --" + std::string{key}};
   }
   return found->second;
}
[[nodiscard]] std::string optional_value(const fixture_arguments& args, std::string_view key,
                                         std::string_view fallback = {}) {
   const auto found = args.find(std::string{key});
   return found == args.end() || found->second.empty() ? std::string{fallback} : found->second;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
   auto result = std::string{};
   result.reserve(value.size() + 8);
   for (const auto character : value) {
      switch (character) {
      case '\\':
         result += "\\\\";
         break;
      case '"':
         result += "\\\"";
         break;
      case '\n':
         result += "\\n";
         break;
      case '\r':
         result += "\\r";
         break;
      case '\t':
         result += "\\t";
         break;
      default:
         result.push_back(character);
         break;
      }
   }
   return result;
}

[[nodiscard]] std::string json_string(std::string_view value) {
   return "\"" + json_escape(value) + "\"";
}

[[nodiscard]] std::string json_optional(const std::optional<std::string>& value) {
   return value ? json_string(*value) : "null";
}

[[nodiscard]] std::string json_bool(bool value) {
   return value ? "true" : "false";
}

void write_file(const std::filesystem::path& path, std::string_view value) {
   std::filesystem::create_directories(path.parent_path());
   auto output = std::ofstream{path};
   if (!output) {
      throw std::runtime_error{"failed to open " + path.string()};
   }
   output << value;
   if (!output) {
      throw std::runtime_error{"failed to write " + path.string()};
   }
}

[[nodiscard]] std::string state_name(reachability::state value) {
   switch (value) {
   case reachability::state::unknown:
      return "unknown";
   case reachability::state::publicly_reachable:
      return "public";
   case reachability::state::private_network:
      return "private";
   case reachability::state::blocked:
      return "blocked";
   case reachability::state::relay_only:
      return "relay_only";
   }
   return "unrecognized";
}

[[nodiscard]] std::string lifecycle_name(forge::net::p2p::lifecycle_phase value) {
   switch (value) {
   case forge::net::p2p::lifecycle_phase::idle:
      return "idle";
   case forge::net::p2p::lifecycle_phase::hydrating:
      return "hydrating";
   case forge::net::p2p::lifecycle_phase::listening:
      return "listening";
   case forge::net::p2p::lifecycle_phase::bootstrapping:
      return "bootstrapping";
   case forge::net::p2p::lifecycle_phase::maintenance:
      return "maintenance";
   case forge::net::p2p::lifecycle_phase::stopping:
      return "stopping";
   case forge::net::p2p::lifecycle_phase::stopped:
      return "stopped";
   }
   return "unrecognized";
}

[[nodiscard]] endpoint normalize_endpoint(endpoint value) {
   value.peer.reset();
   return value;
}

[[nodiscard]] bool same_address(const endpoint& left, const endpoint& right) {
   return normalize_endpoint(left).to_string() == normalize_endpoint(right).to_string();
}

[[nodiscard]] std::string direction_name(forge::net::p2p::connection_direction value) {
   return value == forge::net::p2p::connection_direction::inbound ? "inbound" : "outbound";
}

struct connection_gater_event {
   bool upgraded = false;
   forge::net::p2p::connection_direction direction = forge::net::p2p::connection_direction::inbound;
   forge::net::p2p::peer_id peer;
   forge::net::p2p::connection_endpoints endpoints;
};

class observing_connection_gater final : public forge::net::p2p::connection_gater,
                                         public autonat_connection_observation {
 public:
   explicit observing_connection_gater(std::shared_ptr<forge::net::p2p::connection_gater> delegate)
       : delegate_(std::move(delegate)) {
      events_.reserve(maximum_events);
   }

   [[nodiscard]] bool intercept_peer_dial(const forge::net::p2p::peer_id& peer) noexcept override {
      return !delegate_ || delegate_->intercept_peer_dial(peer);
   }

   [[nodiscard]] bool intercept_address_dial(const forge::net::p2p::peer_id& peer,
                                             const endpoint& address) noexcept override {
      return !delegate_ || delegate_->intercept_address_dial(peer, address);
   }

   [[nodiscard]] bool intercept_accept(const forge::net::p2p::connection_endpoints& endpoints) noexcept override {
      return !delegate_ || delegate_->intercept_accept(endpoints);
   }

   [[nodiscard]] bool intercept_secured(forge::net::p2p::connection_direction direction,
                                        const forge::net::p2p::peer_id& peer,
                                        const forge::net::p2p::connection_endpoints& endpoints) noexcept override {
      if (delegate_ && !delegate_->intercept_secured(direction, peer, endpoints)) {
         return false;
      }
      record(false, direction, peer, endpoints);
      return true;
   }

   [[nodiscard]] bool intercept_upgraded(forge::net::p2p::connection_direction direction,
                                         const forge::net::p2p::peer_id& peer,
                                         const forge::net::p2p::connection_endpoints& endpoints) noexcept override {
      if (delegate_ && !delegate_->intercept_upgraded(direction, peer, endpoints)) {
         return false;
      }
      record(true, direction, peer, endpoints);
      return true;
   }

   [[nodiscard]] bool complete() const noexcept override {
      return !overflow_.load(std::memory_order_relaxed) && !write_failed_.load(std::memory_order_relaxed);
   }

   [[nodiscard]] std::string failure_reason() const override {
      if (overflow_.load(std::memory_order_relaxed)) {
         return "connection-gater trace exceeded its bounded capacity";
      }
      if (write_failed_.load(std::memory_order_relaxed)) {
         return "connection-gater trace could not retain an event";
      }
      return {};
   }

   [[nodiscard]] std::string json() const override {
      auto lock = std::scoped_lock{mutex_};
      auto encoded = std::string{"["};
      for (const auto& event : events_) {
         if (encoded.size() != 1) {
            encoded.push_back(',');
         }
         encoded += "{\"stage\":" + json_string(event.upgraded ? "upgraded" : "secured") +
             ",\"direction\":" + json_string(direction_name(event.direction)) +
             ",\"authenticated\":true,\"authenticated_peer\":" + json_string(event.peer.to_string()) +
             ",\"local_addr\":" + json_string(normalize_endpoint(event.endpoints.local).to_string()) +
             ",\"remote_addr\":" + json_string(normalize_endpoint(event.endpoints.remote).to_string()) + "}";
      }
      return encoded + "]";
   }

 private:
   void record(bool upgraded, forge::net::p2p::connection_direction direction, const forge::net::p2p::peer_id& peer,
               const forge::net::p2p::connection_endpoints& endpoints) noexcept {
      auto lock = std::scoped_lock{mutex_};
      if (events_.size() == maximum_events) {
         overflow_.store(true, std::memory_order_relaxed);
         return;
      }
      try {
         events_.push_back({.upgraded = upgraded, .direction = direction, .peer = peer, .endpoints = endpoints});
      } catch (...) {
         write_failed_.store(true, std::memory_order_relaxed);
      }
   }

   static constexpr auto maximum_events = std::size_t{64};

   std::shared_ptr<forge::net::p2p::connection_gater> delegate_;
   mutable std::mutex mutex_;
   std::vector<connection_gater_event> events_;
   std::atomic<bool> overflow_ = false;
   std::atomic<bool> write_failed_ = false;
};

// Keep fixture input acceptance aligned with node-owned public-address filtering.
[[nodiscard]] bool is_public_numeric_address(std::string_view value) {
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(std::string{value}, error);
   if (error || (address.is_v6() && address.to_v6().is_v4_mapped()) || address.is_loopback() ||
       address.is_unspecified() || address.is_multicast()) {
      return false;
   }
   if (address.is_v4()) {
      const auto raw = address.to_v4().to_uint();
      const auto private_or_shared = (raw & 0xff00'0000U) == 0x0a00'0000U ||
          (raw & 0xfff0'0000U) == 0xac10'0000U || (raw & 0xffff'0000U) == 0xc0a8'0000U ||
          (raw & 0xffc0'0000U) == 0x6440'0000U;
      const auto link_local = (raw & 0xffff'0000U) == 0xa9fe'0000U;
      const auto unroutable = (raw & 0xff00'0000U) == 0x0000'0000U ||
          (raw & 0xffff'ffc0U) == 0xc000'0000U || (raw & 0xffff'ff00U) == 0xc000'0200U ||
          (raw & 0xffff'ff00U) == 0xc058'6300U || (raw & 0xfffe'0000U) == 0xc612'0000U ||
          (raw & 0xffff'ff00U) == 0xc633'6400U || (raw & 0xffff'ff00U) == 0xcb00'7100U ||
          (raw & 0xf000'0000U) == 0xe000'0000U || (raw & 0xf000'0000U) == 0xf000'0000U;
      return !private_or_shared && !link_local && !unroutable;
   }
   const auto bytes = address.to_v6().to_bytes();
   const auto private_address = (bytes[0] & 0xfeU) == 0xfcU;
   const auto documentation = bytes[0] == 0x20U && bytes[1] == 0x01U && bytes[2] == 0x0dU && bytes[3] == 0xb8U;
   const auto global_unicast = (bytes[0] & 0xe0U) == 0x20U && !documentation;
   const auto well_known_nat64 = bytes[0] == 0 && bytes[1] == 0x64U && bytes[2] == 0xffU && bytes[3] == 0x9bU &&
       bytes[4] == 0 && bytes[5] == 0 && bytes[6] == 0 && bytes[7] == 0 && bytes[8] == 0 && bytes[9] == 0 &&
       bytes[10] == 0 && bytes[11] == 0;
   const auto local_nat64 = bytes[0] == 0 && bytes[1] == 0x64U && bytes[2] == 0xffU && bytes[3] == 0x9bU &&
       bytes[4] == 0 && bytes[5] == 1;
   return !address.to_v6().is_link_local() && !private_address && (global_unicast || well_known_nat64 || local_nat64);
}

[[nodiscard]] endpoint bind_endpoint(std::string_view ip, std::string_view transport) {
   auto error = boost::system::error_code{};
   const auto parsed = boost::asio::ip::make_address(std::string{ip}, error);
   if (error || !is_public_numeric_address(ip)) {
      throw std::runtime_error{"AutoNAT requires --bind-ip to be a numeric public-classified address"};
   }
   const auto protocol = transport == "quic" ? endpoint::protocol_kind::quic_v1 : endpoint::protocol_kind::tcp;
   return {.transport = {.host_type = parsed.is_v4() ? endpoint::host_kind::ip4 : endpoint::host_kind::ip6,
                         .protocol = protocol, .host = parsed.to_string(), .port = 0}};
}

void require_transport(std::string_view transport) {
   if (transport != "quic" && transport != "tcp" && transport != "tcp-tls" && transport != "tcp-pnet") {
      throw std::runtime_error{"unsupported AutoNAT fixture transport: " + std::string{transport}};
   }
}

[[nodiscard]] endpoint probe_endpoint(const fixture_arguments& args, const endpoint& bind, std::string_view transport,
                                      std::string_view scenario, bool expect_unreachable) {
   const auto input = optional_value(args, "probe-addr");
   if (input.empty()) {
      if (expect_unreachable) {
         throw std::runtime_error{"--expect-unreachable requires an explicit --probe-addr"};
      }
      return bind;
   }
   auto result = normalize_endpoint(forge::net::p2p::parse_endpoint(input));
   if (expect_unreachable && result.transport.host_type != endpoint::host_kind::ip4 &&
       result.transport.host_type != endpoint::host_kind::ip6) {
      throw std::runtime_error{"--expect-unreachable requires an ip4 or ip6 candidate, not DNS"};
   }
   if (result.relayed || (transport == "quic" ? !result.is_direct_quic() : !result.is_direct_tcp()) ||
       !is_public_numeric_address(result.transport.host)) {
      throw std::runtime_error{"--probe-addr must be a numeric public direct address for the selected transport"};
   }
   if (expect_unreachable) {
      result.transport.host = boost::asio::ip::make_address(result.transport.host).to_string();
   }
   if (scenario == "autonat_v1" && result.transport.host != bind.transport.host) {
      throw std::runtime_error{"AutoNAT v1 probe address must use the observed control IP"};
   }
   if (expect_unreachable && result.transport.port == 0) {
      throw std::runtime_error{"--expect-unreachable requires a nonzero closed port supplied by the isolated runner"};
   }
   return result;
}

[[nodiscard]] endpoint actual_listener(const node& value, const endpoint& requested) {
   auto matches = std::vector<endpoint>{};
   for (auto candidate : value.local_endpoints()) {
      candidate = normalize_endpoint(std::move(candidate));
      if (candidate.transport.host_type == requested.transport.host_type &&
          candidate.transport.protocol == requested.transport.protocol &&
          candidate.transport.host == requested.transport.host && candidate.transport.port != 0 &&
          (requested.transport.port == 0 || candidate.transport.port == requested.transport.port)) {
         matches.push_back(std::move(candidate));
      }
   }
   if (matches.size() != 1 || !is_public_numeric_address(matches.front().transport.host)) {
      throw std::runtime_error{"AutoNAT requested listener did not bind exactly one public actual address"};
   }
   return matches.front();
}

[[nodiscard]] endpoint observer_endpoint(const fixture_arguments& args, std::string_view transport,
                                         const forge::net::p2p::peer_id& observer) {
   auto value = forge::net::p2p::parse_endpoint(required(args, "addr"));
   if (value.peer && *value.peer != observer) {
      throw std::runtime_error{"--addr peer suffix disagrees with --peer-id"};
   }
   value.peer = observer;
   if (value.relayed || (transport == "quic" ? !value.is_direct_quic() : !value.is_direct_tcp()) ||
       value.transport.port == 0 || !is_public_numeric_address(value.transport.host)) {
      throw std::runtime_error{"AutoNAT observer requires a numeric public direct address"};
   }
   return value;
}

[[nodiscard]] std::string protocol_for(std::string_view scenario) {
   return scenario == "autonat_v1" ? forge::net::p2p::builtins::autonat_v1.value
                                    : forge::net::p2p::builtins::autonat_v2_dial_request.value;
}

[[nodiscard]] std::string host_event_json(const host_event& value) {
   auto addresses = std::vector<std::string>{};
   addresses.reserve(value.autonat_v2.size());
   for (const auto& item : value.autonat_v2) {
      addresses.push_back("{\"address\":" + json_string(normalize_endpoint(item.address).to_string()) +
                          ",\"state\":" + json_string(state_name(item.value)) + "}");
   }
   auto joined = std::string{};
   for (const auto& item : addresses) {
      if (!joined.empty()) {
         joined += ',';
      }
      joined += item;
   }
   return "{\"phase\":" + json_string(lifecycle_name(value.phase)) + ",\"effective\":" +
       json_string(state_name(value.effective)) + ",\"autonat_v1\":" + json_string(state_name(value.autonat_v1)) +
       ",\"autonat_v2\":[" + joined + "]}";
}

[[nodiscard]] std::string diagnostics_json(const forge::net::p2p::diagnostics::snapshot& value) {
   const auto& resources = value.resources;
   const auto& reachability = value.reachability;
   return "{\"lifecycle_phase\":" + json_string(lifecycle_name(value.lifecycle.phase)) +
       ",\"active_sessions\":" + std::to_string(value.connections.active_sessions) +
       ",\"resources\":{\"system_memory\":" + std::to_string(resources.system.memory) +
       ",\"inbound_connections\":" + std::to_string(resources.connections.inbound_connections) +
       ",\"outbound_connections\":" + std::to_string(resources.connections.outbound_connections) +
       ",\"active_dials\":" + std::to_string(resources.active_dials) +
       ",\"active_service_scopes\":" + std::to_string(resources.active_service_scopes) + "},\"reachability\":{\"host\":" +
       host_event_json(reachability.host) + ",\"client_v1_enabled\":" + json_bool(reachability.client_v1_enabled) +
       ",\"client_v2_enabled\":" + json_bool(reachability.client_v2_enabled) +
       ",\"service_v1_enabled\":" + json_bool(reachability.service_v1_enabled) +
       ",\"service_v2_enabled\":" + json_bool(reachability.service_v2_enabled) +
       ",\"internet_egress_allowed\":" + json_bool(reachability.internet_egress_allowed) +
       ",\"pending_probes\":" + std::to_string(reachability.pending_probes) +
       ",\"active_handlers\":" + std::to_string(reachability.active_handlers) +
       ",\"probe_errors\":" + std::to_string(reachability.probe_errors) + "}}";
}

struct result_record {
   std::string role;
   std::string scenario;
   std::string transport;
   std::string protocol;
   std::string internet_egress;
   std::uint32_t version = 0;
   std::string status = "ok";
   std::optional<std::string> error;
   std::optional<std::string> local_peer_id;
   std::optional<std::string> observer_peer_id;
   std::optional<std::string> control_authenticated_peer;
   bool control_identify_completed = false;
   std::optional<std::string> requested_addr;
   std::optional<std::string> response_addr;
   bool reached = false;
   std::string probe_state = "unknown";
   std::string effective_state = "unknown";
   std::string autonat_v1_state = "unknown";
   std::optional<std::string> v1_vote;
   std::optional<std::string> autonat_v2_address_state;
   std::uint32_t autonat_probe_api_calls = 0;
   std::uint32_t v2_fixture_candidate_count = 0;
   bool policy_denied = false;
   std::string nonce_proof = "not_exposed_by_public_API";
   bool hosts_closed = false;
   bool handlers_joined = false;
   bool trace_complete = false;
   // Partial hook output must never be accepted as connection evidence.
   std::string actual_connections = "null";
   bool connection_gater_trace_complete = false;
   std::optional<std::string> connection_gater_trace_failure;
   std::optional<std::string> diagnostics_before_stop;
   std::optional<std::string> diagnostics_after_stop;
   std::optional<bool> service_enabled;
   std::optional<std::string> expected_outcome;
   std::optional<std::string> candidate_basis;
   std::optional<std::int64_t> probe_elapsed_ms;

   [[nodiscard]] std::string json() const {
      auto fields = std::vector<std::string>{
          "\"implementation\":\"forge\"", "\"role\":" + json_string(role),
          "\"scenario\":" + json_string(scenario), "\"status\":" + json_string(status),
          "\"version\":" + std::to_string(version), "\"protocol\":" + json_string(protocol),
          "\"transport\":" + json_string(transport), "\"internet_egress\":" + json_string(internet_egress),
          "\"local_peer_id\":" + json_optional(local_peer_id),
          "\"observer_peer_id\":" + json_optional(observer_peer_id),
          "\"control_authenticated_peer\":" + json_optional(control_authenticated_peer),
          "\"control_identify_completed\":" + json_bool(control_identify_completed),
          "\"requested_addr\":" + json_optional(requested_addr),
          // The native public API exposes only the resulting state, not a raw AutoNAT reply address.
          "\"response_addr\":" + json_optional(response_addr),
          "\"response_addr_proof\":\"not_exposed_by_public_API\"",
          "\"reached\":" + json_bool(reached),
          "\"probe_state\":" + json_string(probe_state), "\"effective_state\":" + json_string(effective_state),
          "\"effective_confidence\":" + json_string(effective_state),
          "\"autonat_v1_state\":" + json_string(autonat_v1_state),
          "\"v1_vote\":" + json_optional(v1_vote),
          "\"autonat_v2_address_state\":" + json_optional(autonat_v2_address_state),
          "\"current_v2_address_state\":" + json_optional(autonat_v2_address_state),
          "\"autonat_probe_api_calls\":" + std::to_string(autonat_probe_api_calls),
          "\"v2_fixture_candidate_count\":" + std::to_string(v2_fixture_candidate_count),
          "\"nonce_proof\":" + json_string(nonce_proof), "\"hosts_closed\":" + json_bool(hosts_closed),
          "\"handlers_joined\":" + json_bool(handlers_joined), "\"trace_complete\":" + json_bool(trace_complete),
          "\"actual_connections\":" + actual_connections,
          "\"connection_gater_trace_complete\":" + json_bool(connection_gater_trace_complete),
          "\"connection_gater_trace_failure\":" + json_optional(connection_gater_trace_failure),
          "\"policy_denied\":" + json_bool(policy_denied),
          "\"diagnostics_before_stop\":" + json_optional(diagnostics_before_stop),
          "\"diagnostics_after_stop\":" + json_optional(diagnostics_after_stop),
      };
      if (service_enabled) {
         fields.push_back("\"service_enabled\":" + json_bool(*service_enabled));
      }
      if (expected_outcome) {
         fields.push_back("\"expected_outcome\":" + json_string(*expected_outcome));
      }
      if (candidate_basis) {
         fields.push_back("\"candidate_basis\":" + json_string(*candidate_basis));
      }
      if (probe_elapsed_ms) {
         fields.push_back("\"probe_elapsed_ms\":" + std::to_string(*probe_elapsed_ms));
      }
      if (error) {
         fields.push_back("\"error\":" + json_string(*error));
      }
      auto result = std::string{"{"};
      for (const auto& field : fields) {
         if (result.size() != 1) {
            result.push_back(',');
         }
         result += field;
      }
      return result + "}\n";
   }
};

void wait_for_stop_file(const std::filesystem::path& path) {
   while (!std::filesystem::exists(path)) {
      std::this_thread::sleep_for(50ms);
   }
}

} // namespace

autonat_connection_observation::~autonat_connection_observation() noexcept = default;

std::pair<std::shared_ptr<forge::net::p2p::connection_gater>, std::shared_ptr<autonat_connection_observation>>
make_autonat_connection_observer(std::shared_ptr<forge::net::p2p::connection_gater> existing) {
   auto observer = std::make_shared<observing_connection_gater>(std::move(existing));
   return {observer, observer};
}

int run_forge_autonat_fixture(const fixture_arguments& args, const autonat_fixture_support& support) {
   const auto command = required(args, "command");
   const auto scenario = required(args, "scenario");
   const auto transport = optional_value(args, "transport", "quic");
   if ((command != "listen" && command != "dial") || (scenario != "autonat_v1" && scenario != "autonat_v2")) {
      throw std::runtime_error{"AutoNAT fixture requires listen|dial and autonat_v1|autonat_v2"};
   }
   if (!support.make_node) {
      throw std::runtime_error{"AutoNAT fixture node factory is not configured"};
   }
   require_transport(transport);
   const auto service = command == "listen";
   const auto negative_flag = args.find("expect-unreachable");
   if (negative_flag != args.end() &&
       (service || scenario != "autonat_v1" || (negative_flag->second != "true" && negative_flag->second != "false"))) {
      throw std::runtime_error{"--expect-unreachable true|false is supported only for autonat_v1 dial"};
   }
   const auto expect_unreachable = negative_flag != args.end() && negative_flag->second == "true";
   const auto probe_count = optional_value(args, "probe-count", "1");
   if ((probe_count != "1" && probe_count != "2") ||
       (probe_count == "2" && (service || scenario != "autonat_v2" ||
          (transport == "tcp-pnet" && optional_value(args, "internet-egress") != "allow")))) {
      throw std::runtime_error{"--probe-count 2 requires an enabled autonat_v2 dialer"};
   }
   if (expect_unreachable && transport == "tcp-pnet" && optional_value(args, "internet-egress") != "allow") {
      throw std::runtime_error{"unreachable AutoNAT control requires private-network Internet egress allow"};
   }
   if (service && (optional_value(args, "ready-file").empty() || optional_value(args, "stop-file").empty())) {
      throw std::runtime_error{"AutoNAT listener requires --ready-file and --stop-file"};
   }
   if (!service && optional_value(args, "result-file").empty()) {
      throw std::runtime_error{"AutoNAT dialer requires --result-file"};
   }

   auto result = result_record{.role = service ? "listener" : "dialer", .scenario = scenario, .transport = transport,
                               .protocol = protocol_for(scenario), .internet_egress = optional_value(args, "internet-egress"),
                               .version = scenario == "autonat_v1" ? 1U : 2U};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto value = std::unique_ptr<node>{};
   auto connection_observation = std::shared_ptr<autonat_connection_observation>{};
   auto primary_error = std::optional<std::string>{};
   if (expect_unreachable) {
      result.expected_outcome = "unreachable";
      result.candidate_basis = "explicit_unreachable_negative_control";
   }

   try {
      const auto bind = bind_endpoint(required(args, "bind-ip"), transport);
      const auto desired_probe = probe_endpoint(args, bind, transport, scenario, expect_unreachable);
      auto fixture_node = support.make_node(runtime, args, service);
      value = std::move(fixture_node.value);
      connection_observation = std::move(fixture_node.connection_observation);
      if (!value) {
         throw std::runtime_error{"AutoNAT fixture node factory returned no node"};
      }
      if (!connection_observation) {
         throw std::runtime_error{"AutoNAT fixture node factory returned no connection observer"};
      }
      result.local_peer_id = value->local_peer().to_string();
      forge::asio::blocking::run(runtime, value->async_hydrate_peer_state());
      auto bound_control = bind;
      auto bound_probe = bound_control;
      auto fixture_listeners = std::vector<endpoint>{};
      if (expect_unreachable) {
         if (!value->local_endpoints().empty()) {
            throw std::runtime_error{"negative AutoNAT node must start without listeners or advertised addresses"};
         }
         // Do not bind either the control IP or the negative candidate. The
         // runner owns closed-port isolation; the node opens only outbound control.
         value->set_advertised_endpoints({desired_probe});
         const auto advertised = value->local_endpoints();
         if (advertised.size() != 1 || !same_address(advertised.front(), desired_probe)) {
            throw std::runtime_error{"negative AutoNAT node did not retain exactly the explicit candidate"};
         }
      } else if (!service && scenario == "autonat_v2") {
         // Pinned Rust selects one address with index 0. Keep the bilateral
         // request single-candidate; outbound control needs no listener on bind.
         forge::asio::blocking::run(runtime, value->async_listen(desired_probe));
         bound_probe = actual_listener(*value, desired_probe);
         if (value->local_endpoints().size() != 1) {
            throw std::runtime_error{"AutoNAT v2 dialer requires exactly one actual listener candidate"};
         }
         fixture_listeners.push_back(bound_probe);
      } else {
         forge::asio::blocking::run(runtime, value->async_listen(bind));
         bound_control = actual_listener(*value, bind);
         bound_probe = bound_control;
         if (!same_address(desired_probe, bind)) {
            forge::asio::blocking::run(runtime, value->async_listen(desired_probe));
            bound_probe = actual_listener(*value, desired_probe);
         }
         fixture_listeners.push_back(bound_control);
         if (!same_address(bound_probe, bound_control)) {
            fixture_listeners.push_back(bound_probe);
         }
      }
      result.v2_fixture_candidate_count = static_cast<std::uint32_t>(fixture_listeners.size());

      if (service) {
         const auto snapshot = value->diagnostics();
         result.service_enabled = scenario == "autonat_v1" ? snapshot.reachability.service_v1_enabled
                                                             : snapshot.reachability.service_v2_enabled;
         auto addresses = std::vector<std::string>{};
         for (const auto& item : value->local_endpoints()) {
            auto advertised = item;
            advertised.peer = value->local_peer();
            addresses.push_back(json_string(advertised.to_string()));
         }
         auto joined = std::string{};
         for (const auto& address : addresses) {
            if (!joined.empty()) {
               joined.push_back(',');
            }
            joined += address;
         }
         write_file(required(args, "ready-file"), "{\"implementation\":\"forge\",\"role\":\"listener\",\"status\":\"ready\",\"peer_id\":" +
                                                    json_string(*result.local_peer_id) + ",\"listen_addrs\":[" + joined +
                                                    "],\"version\":" + std::to_string(result.version) + ",\"protocol\":" +
                                                    json_string(result.protocol) + ",\"service_enabled\":" +
                                                    json_bool(*result.service_enabled) + "}\n");
         wait_for_stop_file(required(args, "stop-file"));
      } else {
         result.requested_addr = (expect_unreachable ? desired_probe :
             scenario == "autonat_v2" ? bound_probe : bound_control).to_string();
         const auto observer = forge::net::p2p::peer_id::from_string(required(args, "peer-id"));
         const auto remote = observer_endpoint(args, transport, observer);
         const auto control = forge::asio::blocking::run(
             runtime, value->async_connect(remote, node::connect_options{.expected_peer = observer, .allow_relay = false,
                                                                          .allow_hole_punch = false}));
         if (control.remote_peer != observer || control.path != forge::net::p2p::path::kind::direct ||
             control.identify_state != forge::net::p2p::identify::state::identified) {
            throw std::runtime_error{"AutoNAT control session did not complete authenticated direct Identify"};
         }
         result.observer_peer_id = observer.to_string();
         result.control_authenticated_peer = control.remote_peer.to_string();
         result.control_identify_completed = true;
         const auto denied = transport == "tcp-pnet" && optional_value(args, "internet-egress") == "deny";
         if (denied) {
            ++result.autonat_probe_api_calls;
            try {
               static_cast<void>(forge::asio::blocking::run(runtime, value->async_probe_reachability(observer)));
               throw std::runtime_error{"private-network AutoNAT policy unexpectedly allowed a probe"};
            } catch (const forge::exceptions::base& error) {
               if (!forge::net::p2p::exceptions::is(error, forge::net::p2p::exceptions::code::invalid_options)) {
                  throw;
               }
            }
            result.status = "rejected";
            result.policy_denied = true;
         } else {
            if (scenario == "autonat_v1") {
               ++result.autonat_probe_api_calls;
               const auto started = std::chrono::steady_clock::now();
               auto probe = reachability::state::unknown;
               try {
                  probe = forge::asio::blocking::run(runtime, value->async_probe_reachability(observer));
               } catch (...) {
                  if (expect_unreachable) {
                     result.probe_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started).count();
                  }
                  throw;
               }
               if (expect_unreachable) {
                  result.probe_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started).count();
               }
               const auto host = value->reachability_status();
               result.probe_state = state_name(probe);
               result.effective_state = state_name(host.effective);
               result.autonat_v1_state = state_name(host.autonat_v1);
               result.v1_vote = result.probe_state;
               result.reached = probe == reachability::state::publicly_reachable;
               if (expect_unreachable && probe != reachability::state::private_network) {
                  throw std::runtime_error{"negative AutoNAT v1 control did not return a private_network vote"};
               }
            } else {
               const auto expected = forge::net::p2p::parse_endpoint(*result.requested_addr);
               // Two explicit successful calls exercise same-peer dialback
               // session reuse; a failed first call is never retried.
               for (auto attempt = 0; attempt < (probe_count == "2" ? 2 : 1); ++attempt) {
                  ++result.autonat_probe_api_calls;
                  const auto probe = forge::asio::blocking::run(runtime, value->async_probe_reachability(observer));
                  const auto host = value->reachability_status();
                  result.probe_state = state_name(probe);
                  result.effective_state = state_name(host.effective);
                  result.autonat_v1_state = state_name(host.autonat_v1);
                  const auto found = std::ranges::find_if(host.autonat_v2, [&](const auto& item) {
                     return same_address(item.address, expected);
                  });
                  if (found != host.autonat_v2.end()) {
                     result.autonat_v2_address_state =
                         "{\"address\":" + json_string(normalize_endpoint(found->address).to_string()) +
                         ",\"state\":" + json_string(state_name(found->value)) + "}";
                  }
                  if (probe != reachability::state::publicly_reachable || found == host.autonat_v2.end() ||
                      found->value != reachability::state::publicly_reachable) {
                     throw std::runtime_error{
                         "AutoNAT v2 did not report the actual requested listener as publicly reachable"};
                  }
                  result.reached = true;
               }
            }
         }
      }
      result.diagnostics_before_stop = diagnostics_json(value->diagnostics());
   } catch (const std::exception& error) {
      primary_error = error.what();
   } catch (...) {
      primary_error = "AutoNAT fixture raised a non-standard exception";
   }

   if (value) {
      if (!result.diagnostics_before_stop) {
         try {
            result.diagnostics_before_stop = diagnostics_json(value->diagnostics());
         } catch (const std::exception& error) {
            if (!primary_error) {
               primary_error = error.what();
            } else {
               *primary_error += "; diagnostics-before-stop failure: " + std::string{error.what()};
            }
         }
      }
      try {
         forge::asio::blocking::run(runtime, value->async_stop());
         result.handlers_joined = true;
         result.hosts_closed = true;
         result.diagnostics_after_stop = diagnostics_json(value->diagnostics());
      } catch (const std::exception& error) {
         if (!primary_error) {
            primary_error = error.what();
         } else {
            *primary_error += "; AutoNAT shutdown failure: " + std::string{error.what()};
         }
      } catch (...) {
         if (!primary_error) {
            primary_error = "AutoNAT shutdown raised a non-standard exception";
         } else {
            *primary_error += "; AutoNAT shutdown raised a non-standard exception";
         }
      }
   } else {
      result.hosts_closed = true;
   }

   if (connection_observation) {
      try {
         result.connection_gater_trace_complete = connection_observation->complete();
         if (result.connection_gater_trace_complete) {
            result.actual_connections = connection_observation->json();
         } else {
            result.connection_gater_trace_failure = connection_observation->failure_reason();
            const auto failure = "AutoNAT " + *result.connection_gater_trace_failure;
            if (!primary_error) {
               primary_error = failure;
            } else {
               *primary_error += "; " + failure;
            }
         }
      } catch (const std::exception& error) {
         if (!primary_error) {
            primary_error = error.what();
         } else {
            *primary_error += "; connection-gater trace capture failure: " + std::string{error.what()};
         }
      }
   }

   if (primary_error) {
      result.status = "error";
      result.error = *primary_error;
      result.reached = false;
   }
   const auto result_file = optional_value(args, "result-file");
   if (!result_file.empty()) {
      try {
         write_file(result_file, result.json());
      } catch (const std::exception& error) {
         if (!primary_error) {
            primary_error = error.what();
         } else {
            *primary_error += "; result write failure: " + std::string{error.what()};
         }
      }
   }
   if (primary_error) {
      throw std::runtime_error{*primary_error};
   }
   return 0;
}

} // namespace forge::test::libp2p_interop
