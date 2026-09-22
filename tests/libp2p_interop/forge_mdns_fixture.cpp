#include <chrono>
#include <array>
#include <charconv>
#include <exception>
#include <filesystem>
#include <future>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <unistd.h>

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.codec.hex;
import forge.codec.json;
import forge.crypto.digest.sha256;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.node;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.pnet.network_fingerprint;
import forge.net.pnet.protector;
import forge.net.transport.endpoint;

#include "forge_connection_fixture.hxx"
#include "forge_mdns_fixture.hxx"

namespace forge::test::libp2p_interop::forge_mdns_fixture {
namespace {
using namespace std::chrono_literals;
using namespace forge::net::p2p;

const std::string& required(const arguments& args, const std::string& key) {
   const auto found = args.find(key);
   if (found == args.end() || found->second.empty()) {
      throw std::runtime_error{"missing mDNS argument --" + key};
   }
   return found->second;
}

template <typename T>
void save(const std::string& path, const T& value) {
   const auto parent = std::filesystem::path{path}.parent_path();
   if (!parent.empty()) {
      std::filesystem::create_directories(parent);
   }
   if (!forge::codec::json::save(path, value).ok()) {
      throw std::runtime_error{"could not write mDNS receipt"};
   }
}

boost::asio::awaitable<void> pause() {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(20ms);
   co_await timer.async_wait(boost::asio::use_awaitable);
}

boost::asio::awaitable<void> observe_lifecycle(node& owner, const std::string& control_path,
                                             const std::string& receipt_path, const std::string& stop_path,
                                             std::string capture_phase = "after_echo_before_stop",
                                             std::function<void()> check = {}) {
   auto last_epoch = std::uint64_t{};
   while (!std::filesystem::exists(stop_path)) {
      if (check) {
         check();
      }
      if (std::filesystem::exists(control_path)) {
         auto input = std::ifstream{control_path, std::ios::binary};
         auto bytes = std::array<char, 65>{};
         input.read(bytes.data(), bytes.size());
         const auto size = static_cast<std::size_t>(input.gcount());
         if (input.bad() || size == bytes.size()) {
            throw std::runtime_error{"invalid or oversized mDNS lifecycle request"};
         }
         auto epoch = std::uint64_t{};
         const auto parsed = std::from_chars(bytes.data(), bytes.data() + size, epoch);
         if (parsed.ec != std::errc{} || epoch == 0 ||
             std::string_view{parsed.ptr, static_cast<std::size_t>(bytes.data() + size - parsed.ptr)} != " snapshot\n" ||
             epoch < last_epoch) {
            throw std::runtime_error{"mDNS lifecycle requires monotonic '<epoch> snapshot' requests"};
         }
         if (epoch > last_epoch) {
            auto result = lifecycle_receipt{.epoch = epoch, .pid = static_cast<std::int64_t>(::getpid())};
            result.capture_phase = capture_phase;
            try {
               result.local_peer_id = owner.local_peer().value;
               const auto snapshot = owner.diagnostics({.max_peers = 0, .max_sessions = 0, .max_dht_profiles = 0});
               result.topology_phase = snapshot.topology.phase;
               result.mdns_observations = snapshot.topology.mdns_observations;
               result.active_operations = snapshot.topology.active_operations;
               result.failed_refreshes = snapshot.topology.failed_refreshes;
               result.sessions_opened = snapshot.metrics.sessions_opened;
               result.sessions_closed = snapshot.metrics.sessions_closed;
               result.memory = snapshot.resources.system.memory;
               result.file_descriptors = snapshot.resources.system.file_descriptors;
               result.active_dials = snapshot.resources.active_dials;
               result.inbound_connections = snapshot.resources.system.inbound_connections;
               result.outbound_connections = snapshot.resources.system.outbound_connections;
               result.inbound_streams = snapshot.resources.system.inbound_streams;
               result.outbound_streams = snapshot.resources.system.outbound_streams;
            } catch (const std::exception& error) {
               result.capture_error = error.what();
            } catch (...) {
               result.capture_error = "non-standard diagnostic capture failure";
            }
            const auto path = std::filesystem::path{receipt_path};
            if (!path.parent_path().empty()) {
               std::filesystem::create_directories(path.parent_path());
            }
            const auto temporary = receipt_path + ".tmp";
            if (!forge::codec::json::save(temporary, result).ok()) {
               throw std::runtime_error{"could not write mDNS lifecycle receipt"};
            }
            std::filesystem::rename(temporary, path);
            last_epoch = epoch;
         }
      }
      co_await pause();
   }
}

boost::asio::awaitable<void> bounded(boost::asio::awaitable<void> operation,
                                    std::chrono::seconds timeout) {
   using namespace boost::asio::experimental::awaitable_operators;
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   timer.expires_after(timeout);
   const auto completed = co_await (std::move(operation) || timer.async_wait(boost::asio::use_awaitable));
   if (completed.index() != 0) {
      throw std::runtime_error{"mDNS fixture deadline expired"};
   }
}

boost::asio::awaitable<diagnostics::session> identified(node& value, bool listener) {
   for (;;) {
      const auto snapshot = value.diagnostics();
      if (snapshot.metrics.sessions_opened > 1 || snapshot.metrics.sessions_closed != 0) {
         throw std::runtime_error{"mDNS fixture observed replacement or multiple connections"};
      }
      if (snapshot.sessions.size() == 1 && snapshot.topology.mdns_observations > 0) {
         const auto& session = snapshot.sessions.front();
         if (session.identify_state == identify::state::identified && session.remote_endpoint) {
            co_return capture_identified_connection(value, session.remote_peer, *session.remote_endpoint,
                listener ? diagnostics::session_direction::inbound : diagnostics::session_direction::outbound);
         }
      }
      co_await pause();
   }
}

receipt exchanged(node& value, const diagnostics::session& session, const connection_observer& observer,
                  receipt result, const stream& application, std::span<const std::uint8_t> payload) {
   require_same_connection(value, session);
   const auto observed = observer.snapshot();
   const auto count = value.diagnostics().topology.mdns_observations;
   if (observed.invalid || observed.upgraded != 1 || !observed.peer || *observed.peer != session.remote_peer ||
       !observed.endpoints || count == 0 ||
       (result.role == "dialer" && observed.observations_before_dial == 0)) {
      throw std::runtime_error{"mDNS connection/discovery evidence is incomplete"};
   }
   auto security = std::optional<std::string>{};
   switch (application.authentication()) {
   case peer_authentication::noise: security = "/noise"; break;
   case peer_authentication::libp2p_tls: security = "/tls/1.0.0"; break;
   case peer_authentication::quic_tls: security = "quic-tls"; break;
   default: throw std::runtime_error{"mDNS echo stream is not authenticated"};
   }
   const auto& endpoints = *observed.endpoints;
   auto actual_remote = endpoints.remote;
   auto session_remote = *session.remote_endpoint;
   actual_remote.peer.reset();
   session_remote.peer.reset();
   if (actual_remote.to_string() != session_remote.to_string()) {
      throw std::runtime_error{"mDNS socket observation does not match authenticated session"};
   }
   const auto transport = endpoints.remote.is_direct_quic() ? "/quic-v1" :
                          endpoints.remote.is_direct_tcp() ? "tcp" : "";
   if (std::string_view{transport}.empty()) {
      throw std::runtime_error{"unexpected mDNS connection transport"};
   }
   result.status = "ok";
   result.discovery = discovery_receipt{.runtime_observation_count = count,
       .observations_before_outbound_dial = observed.observations_before_dial};
   result.connection = connection_receipt{.id = std::to_string(session.id),
       .local_peer_id = value.local_peer().value, .remote_peer_id = session.remote_peer.value,
       .direction = result.role == "listener" ? "inbound" : "outbound",
       .local_address = endpoints.local.to_string(), .remote_address = endpoints.remote.to_string(),
       .transport = transport, .security = std::move(security)};
   result.echo = echo_receipt{.bytes = payload.size(),
       .sha256 = forge::crypto::digest::sha256::hash(payload).str(),
       .connection_id = std::to_string(session.id), .remote_peer_id = session.remote_peer.value,
       .stream_id = application.id()};
   return result;
}
} // namespace

connection_observer::connection_observer(bool listener, std::function<std::size_t()> observations)
    : listener_(listener), observations_(std::move(observations)) {}

bool connection_observer::intercept_peer_dial(const peer_id&) noexcept {
   try {
      {
         auto lock = std::lock_guard{mutex_};
         ++evidence_.peer_dial_gate_calls;
      }
      if (listener_) {
         return false;
      }
      const auto count = observations_();
      auto lock = std::lock_guard{mutex_};
      if (count == 0) {
         evidence_.invalid = true;
         return false;
      }
      evidence_.observations_before_dial = count;
      return !evidence_.invalid;
   } catch (...) {
      return false;
   }
}

bool connection_observer::intercept_accept(const connection_endpoints&) noexcept {
   try {
      auto lock = std::lock_guard{mutex_};
      ++evidence_.inbound_gate_calls;
      return listener_;
   } catch (...) {
      return false;
   }
}

bool connection_observer::intercept_upgraded(connection_direction direction, const peer_id& peer,
                                             const connection_endpoints& endpoints) noexcept {
   try {
      auto lock = std::lock_guard{mutex_};
      ++evidence_.upgraded;
      if (evidence_.upgraded != 1 ||
          direction != (listener_ ? connection_direction::inbound : connection_direction::outbound)) {
         evidence_.invalid = true;
         return false;
      }
      evidence_.peer = peer;
      evidence_.endpoints = endpoints;
      return true;
   } catch (...) {
      return false;
   }
}

connection_observer::evidence connection_observer::snapshot() const {
   auto lock = std::lock_guard{mutex_};
   return evidence_;
}

int run(const arguments& args, const support& helpers) {
   // An allowlist prevents all remote coordinates and other discovery inputs.
   for (const auto& [key, ignored] : args) {
      if (key != "command" && key != "scenario" && key != "bind-ip" && key != "transport" &&
          key != "payload" && key != "ready-file" && key != "result-file" && key != "stop-file" &&
          key != "pnet-key-file" && key != "lifecycle-control-file" && key != "lifecycle-receipt-file" &&
          key != "mdns-outcome") {
         throw std::runtime_error{"unsupported hidden-peer mDNS argument --" + key};
      }
   }
   const auto listener = required(args, "command") == "listen";
   const auto outcome = args.contains("mdns-outcome") ? required(args, "mdns-outcome") : "echo";
   if (outcome != "echo" && outcome != "quiet") {
      throw std::runtime_error{"mDNS outcome must be echo or quiet"};
   }
   const auto quiet = outcome == "quiet";
   const auto& transport = required(args, "transport");
   if (transport != "tcp" && transport != "tcp-tls" && transport != "tcp-pnet" && transport != "quic") {
      throw std::runtime_error{"unsupported mDNS transport"};
   }
   const auto& bind = required(args, "bind-ip");
   if (bind.find('\0') != std::string::npos || bind.find('%') != std::string::npos) {
      throw std::runtime_error{"mDNS bind IP must be an unscoped literal"};
   }
   const auto address = boost::asio::ip::make_address(bind);
   if (address.is_loopback() || address.is_unspecified() || address.is_multicast() ||
       (address.is_v6() && address.to_v6().is_link_local())) {
      throw std::runtime_error{"mDNS bind IP must be a usable non-loopback interface address"};
   }
   const auto& payload_text = required(args, "payload");
   if (payload_text.size() > 4096) {
      throw std::runtime_error{"mDNS payload exceeds 4096 bytes"};
   }
   const auto payload = std::vector<std::uint8_t>{payload_text.begin(), payload_text.end()};
   const auto ready_path = required(args, "ready-file");
   const auto result_path = required(args, "result-file");
   const auto stop_path = required(args, "stop-file");
   if (quiet && std::filesystem::exists(stop_path)) {
      throw std::runtime_error{"mDNS quiet stop file must not predate readiness"};
   }
   const auto staged = args.contains("lifecycle-control-file");
   if (staged != args.contains("lifecycle-receipt-file")) {
      throw std::runtime_error{"mDNS lifecycle control and receipt paths must be supplied together"};
   }
   if (staged) {
      const auto& control = required(args, "lifecycle-control-file");
      const auto& output = required(args, "lifecycle-receipt-file");
      auto paths = std::vector<std::filesystem::path>{};
      for (const auto& path : {control, output, output + ".tmp", ready_path, result_path, stop_path}) {
         const auto normalized = std::filesystem::absolute(path).lexically_normal();
         for (const auto& prior : paths) {
            if (normalized == prior) {
               throw std::runtime_error{"mDNS lifecycle paths must be distinct"};
            }
         }
         paths.push_back(normalized);
      }
   }
   if ((transport == "tcp-pnet") != args.contains("pnet-key-file")) {
      throw std::runtime_error{"mDNS private mode requires exactly tcp-pnet and a key file"};
   }
   auto options = helpers.make_options(args);
   options.capabilities = capability_set{};
   if (transport == "quic") {
      options.capabilities.add(capabilities::direct_quic);
   }
   options.limits.topology.operating_mode = topology::mode::managed;
   options.limits.topology.peers = {.low = 1, .target = 1, .high = 1};
   options.limits.topology.max_parallel_dials = 1;
   options.limits.topology.dht_enabled = false;
   options.limits.topology.rendezvous_enabled = false;
   options.limits.topology.peer_exchange_enabled = false;
   options.reachability_policy.client_v1_enabled = false;
   options.reachability_policy.client_v2_enabled = false;
   options.reachability_policy.service_v1_enabled = false;
   options.reachability_policy.service_v2_enabled = false;
   options.reachability_policy.ping_enabled = false;
   options.mdns.enabled = true;
   options.mdns.ipv4_enabled = address.is_v4();
   options.mdns.ipv6_enabled = address.is_v6();
   options.lifecycle.listen = {endpoint{.transport = {
       .host_type = address.is_v4() ? endpoint::host_kind::ip4 : endpoint::host_kind::ip6,
       .protocol = transport == "quic" ? endpoint::protocol_kind::quic_v1 : endpoint::protocol_kind::tcp,
       .host = bind, .port = 0}}};
   auto result = receipt{.role = listener ? "listener" : "dialer", .status = "ready",
       .service_name = "_p2p._udp.local"};
   if (transport == "tcp-pnet") {
      if (!options.private_network || !options.private_network->protector) {
         throw std::runtime_error{"mDNS private key was not loaded"};
      }
      result.service_name = "_p2p-" + forge::codec::hex::encode(
          options.private_network->protector->network_fingerprint().bytes) + "._udp.local";
   }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   // No ownership cycle: the node owns the observer, whose pointer remains valid
   // until async_stop joins all gate calls; it is never accessed after shutdown.
   auto owner = std::unique_ptr<node>{};
   auto observer = std::make_shared<connection_observer>(listener, [&owner] {
      return owner->diagnostics({.max_peers = 0, .max_sessions = 0, .max_dht_profiles = 0})
          .topology.mdns_observations;
   });
   options.connection_gater = observer;
   owner = std::make_unique<node>(runtime, std::move(options));
   result.local_peer_id = owner->local_peer().value;
   auto echoed = std::make_shared<std::promise<receipt>>();
   auto echo_result = echoed->get_future();
   auto entered = std::make_shared<std::atomic_bool>(false);
   auto quiet_streams = std::atomic<std::uint64_t>{0};
   if (quiet) {
      owner->register_protocol_handler(protocol_id{.value = "/forge/interop/relay-echo/1"},
          [&](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
             ++quiet_streams;
             co_await incoming.stream.async_close();
          });
   } else if (listener) {
      owner->register_protocol_handler(protocol_id{.value = "/forge/interop/relay-echo/1"},
          [&, echoed, entered](node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
             if (entered->exchange(true)) {
                throw std::runtime_error{"mDNS fixture received a second echo stream"};
             }
             try {
                const auto session = co_await identified(*owner, true);
                if (incoming.session.remote_peer != session.remote_peer || incoming.session.path != path::kind::direct) {
                   throw std::runtime_error{"mDNS echo is not on the authenticated direct connection"};
                }
                const auto received = co_await helpers.read(incoming.stream, 4096);
                if (received != payload) {
                   throw std::runtime_error{"mDNS echo challenge mismatch"};
                }
                auto evidence = exchanged(*owner, session, *observer, result, incoming.stream, received);
                co_await incoming.stream.async_write(helpers.frame(received));
                co_await incoming.stream.async_close();
                echoed->set_value(std::move(evidence));
             } catch (...) {
                echoed->set_exception(std::current_exception());
             }
          });
   }
   auto quiet_result = quiet_receipt{.role = result.role, .status = "ready",
       .local_peer_id = result.local_peer_id, .service_name = result.service_name};
   auto check_quiet = [&] {
      const auto snapshot = owner->diagnostics({.max_peers = 0, .max_sessions = 0, .max_dht_profiles = 0});
      const auto gates = observer->snapshot();
      quiet_result.mdns_observations_snapshot = snapshot.topology.mdns_observations;
      quiet_result.peer_dial_gate_calls = gates.peer_dial_gate_calls;
      quiet_result.inbound_gate_calls = gates.inbound_gate_calls;
      quiet_result.direct_attempts = snapshot.metrics.path_direct_attempts;
      quiet_result.authenticated_connections = snapshot.metrics.sessions_opened;
      quiet_result.handshakes_completed = snapshot.metrics.handshakes_completed;
      quiet_result.echo_streams = quiet_streams.load();
      if (quiet_result.mdns_observations_snapshot || quiet_result.peer_dial_gate_calls ||
          quiet_result.inbound_gate_calls || quiet_result.direct_attempts || quiet_result.authenticated_connections ||
          quiet_result.handshakes_completed || quiet_result.echo_streams || gates.upgraded || gates.invalid) {
         throw std::runtime_error{"mDNS quiet observation contained discovery or connection activity"};
      }
   };
   auto echo_completed = std::atomic_bool{false};
   const auto quiet_deadline = std::chrono::steady_clock::now() + 45s;
   auto operation = [&]() -> boost::asio::awaitable<void> {
      static_cast<void>(co_await owner->async_start());
      if (quiet) {
         const auto began = std::chrono::steady_clock::now();
         quiet_result.ready_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
         quiet_result.capture_phase = "ready";
         check_quiet();
         save(ready_path, quiet_result);
         if (staged) {
            co_await observe_lifecycle(*owner, required(args, "lifecycle-control-file"),
                required(args, "lifecycle-receipt-file"), stop_path, "quiet_before_stop", check_quiet);
         } else {
            while (!std::filesystem::exists(stop_path)) {
               check_quiet();
               co_await pause();
            }
         }
         if (!std::filesystem::is_regular_file(stop_path)) {
            throw std::runtime_error{"mDNS quiet stop must be a regular file"};
         }
         if (std::chrono::steady_clock::now() >= quiet_deadline) {
            throw std::runtime_error{"mDNS quiet deadline expired before stop"};
         }
         check_quiet();
         quiet_result.stop_reason = "stop_file";
         quiet_result.stop_requested_at_unix_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch()).count();
         quiet_result.observed_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - began).count();
         quiet_result.capture_phase = "before_stop";
         co_return;
      }
      save(ready_path, result);
      if (listener) {
         while (echo_result.wait_for(0s) != std::future_status::ready) {
            co_await pause();
         }
         save(result_path, echo_result.get());
         echo_completed = true;
         while (!staged && !std::filesystem::exists(stop_path)) {
            co_await pause();
         }
      } else {
         const auto session = co_await identified(*owner, false);
         if (observer->snapshot().observations_before_dial == 0) {
            throw std::runtime_error{"mDNS evidence was not observed before the outbound connection"};
         }
         auto application = co_await owner->async_open_protocol_stream(session.remote_peer,
             protocol_id{.value = "/forge/interop/relay-echo/1"},
             node::open_options{.allow_relay = false, .allow_hole_punch = false});
         co_await application.async_write(helpers.frame(payload));
         const auto received = co_await helpers.read(application, 4096);
         if (received != payload) {
            throw std::runtime_error{"mDNS echo challenge mismatch"};
         }
         auto evidence = exchanged(*owner, session, *observer, result, application, received);
         co_await application.async_close();
         save(result_path, evidence);
         echo_completed = true;
      }
      if (staged) {
         co_await observe_lifecycle(*owner, required(args, "lifecycle-control-file"),
             required(args, "lifecycle-receipt-file"), stop_path);
      }
   };
   auto staged_operation = [&]() -> boost::asio::awaitable<void> {
      using namespace boost::asio::experimental::awaitable_operators;
      auto operation_failure = std::exception_ptr{};
      auto run_operation = [&]() -> boost::asio::awaitable<void> {
         try {
            co_await operation();
         } catch (...) {
            operation_failure = std::current_exception();
         }
      };
      auto until_stop = [&]() -> boost::asio::awaitable<void> {
         // After echo, observe_lifecycle owns normal stop completion. Do not
         // cancel it halfway through publishing the final requested snapshot.
         while (echo_completed.load() || !std::filesystem::exists(stop_path)) {
            co_await pause();
         }
      };
      co_await (run_operation() || until_stop());
      if (operation_failure) {
         std::rethrow_exception(operation_failure);
      }
      if (!echo_completed.load()) {
         throw std::runtime_error{"mDNS lifecycle stopped before authenticated echo"};
      }
   };
   auto failure = std::exception_ptr{};
   try {
      forge::asio::blocking::run(runtime, bounded(staged && !quiet ? staged_operation() : operation(),
                                                 staged && !quiet ? 120s : 45s));
   } catch (...) {
      failure = std::current_exception();
   }
   // Snapshot before shutdown withdraws transient discovery and closes sessions.
   // Kept locally unless a failure receipt is needed, including shutdown failure.
   auto before_stop = failure_diagnostics{};
   try {
      const auto observed = observer->snapshot();
      before_stop.peer_dial_gate_calls = observed.peer_dial_gate_calls;
      before_stop.observations_before_dial = observed.observations_before_dial;
      before_stop.upgraded = observed.upgraded;
      before_stop.observer_invalid = observed.invalid;
      const auto snapshot = owner->diagnostics({.max_peers = 0, .max_sessions = 0, .max_dht_profiles = 0});
      before_stop.topology_phase = snapshot.topology.phase;
      before_stop.mdns_observations = snapshot.topology.mdns_observations;
      before_stop.active_operations = snapshot.topology.active_operations;
      before_stop.completed_refreshes = snapshot.topology.completed_refreshes;
      before_stop.failed_refreshes = snapshot.topology.failed_refreshes;
      before_stop.direct_attempts = snapshot.metrics.path_direct_attempts;
      before_stop.direct_failures = snapshot.metrics.direct_failures;
      before_stop.sessions_opened = snapshot.metrics.sessions_opened;
      before_stop.sessions_closed = snapshot.metrics.sessions_closed;
   } catch (const std::exception& error) {
      before_stop.capture_error = error.what();
   } catch (...) {
      before_stop.capture_error = "non-standard exception during diagnostic capture";
   }
   try {
      forge::asio::blocking::run(runtime, bounded(owner->async_stop(), 5s));
      if (quiet) {
         // Capture lifetime counters again after joins, so shutdown races cannot
         // turn a pre-stop zero snapshot into a false quiet success.
         check_quiet();
         quiet_result.cleanup_complete = true;
      }
   } catch (...) {
      failure = std::current_exception();
   }
   // Join workers while every object referenced by handlers is still alive,
   // including the failure path where the shutdown deadline was exceeded.
   runtime.stop();
   if (quiet) {
      quiet_result.capture_phase = "after_stop";
      quiet_result.status = failure ? "error" : "ok";
      if (failure) {
         quiet_result.failure = before_stop;
      }
      save(result_path, quiet_result);
   }
   if (failure) {
      result.status = "error";
      result.failure = std::move(before_stop);
      if (!quiet) {
         save(result_path, result);
      }
      std::rethrow_exception(failure);
   }
   return 0;
}

} // namespace forge::test::libp2p_interop::forge_mdns_fixture
