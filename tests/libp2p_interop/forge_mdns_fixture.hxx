#pragma once

#include <boost/describe.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Included after the runtime and P2P node/stream/gater module imports.
namespace forge::test::libp2p_interop::forge_mdns_fixture {

using arguments = std::map<std::string, std::string>;

struct support {
   std::function<forge::net::p2p::node::options(const arguments&)> make_options;
   std::function<std::vector<std::uint8_t>(std::span<const std::uint8_t>)> frame;
   std::function<boost::asio::awaitable<std::vector<std::uint8_t>>(forge::net::p2p::stream&, std::size_t)> read;
};

struct discovery_receipt {
   std::string source = "mdns";
   std::string basis = "runtime_observation_count";
   std::size_t runtime_observation_count = 0;
   std::size_t observations_before_outbound_dial = 0;
};
BOOST_DESCRIBE_STRUCT(discovery_receipt, (),
                      (source, basis, runtime_observation_count, observations_before_outbound_dial))

struct connection_receipt {
   std::string id, local_peer_id, remote_peer_id, direction, local_address, remote_address, transport;
   std::optional<std::string> security, muxer;
   std::string binding = "single_lifetime_session_and_stream_peer";
};
BOOST_DESCRIBE_STRUCT(connection_receipt, (),
                      (id, local_peer_id, remote_peer_id, direction, local_address, remote_address,
                       transport, security, muxer, binding))

struct echo_receipt {
   std::string protocol = "/forge/interop/relay-echo/1";
   std::size_t bytes = 0;
   std::string sha256, connection_id, remote_peer_id;
   std::int64_t stream_id = 0;
};
BOOST_DESCRIBE_STRUCT(echo_receipt, (), (protocol, bytes, sha256, connection_id, remote_peer_id, stream_id))

struct failure_diagnostics {
   std::string capture_phase = "before_stop";
   std::string capture_error;
   std::string service_errors = "unavailable_via_public_api";
   std::string topology_phase;
   std::size_t mdns_observations = 0;
   std::size_t active_operations = 0;
   std::uint64_t completed_refreshes = 0, failed_refreshes = 0;
   std::uint64_t direct_attempts = 0, direct_failures = 0;
   std::uint64_t sessions_opened = 0, sessions_closed = 0;
   std::size_t peer_dial_gate_calls = 0, observations_before_dial = 0, upgraded = 0;
   bool observer_invalid = false;
};
BOOST_DESCRIBE_STRUCT(failure_diagnostics, (),
                      (capture_phase, capture_error, service_errors, topology_phase, mdns_observations,
                       active_operations, completed_refreshes, failed_refreshes, direct_attempts, direct_failures,
                       sessions_opened, sessions_closed, peer_dial_gate_calls, observations_before_dial,
                       upgraded, observer_invalid))

struct receipt {
   std::string schema = "forge.mdns.interop.v1";
   std::string implementation = "forge";
   std::string role, status, local_peer_id, service_name;
   std::optional<discovery_receipt> discovery;
   std::optional<connection_receipt> connection;
   std::optional<echo_receipt> echo;
   std::optional<failure_diagnostics> failure;
};
BOOST_DESCRIBE_STRUCT(receipt, (),
                      (schema, implementation, role, status, local_peer_id, service_name, discovery, connection, echo, failure))

struct quiet_receipt {
   std::string schema = "forge.mdns.interop.v1", implementation = "forge", outcome = "quiet";
   std::string role, status, local_peer_id, service_name, stop_reason;
   std::string basis = "runtime_metrics_and_gater_with_discovery_snapshot";
   std::string capture_phase = "after_stop";
   std::string service_errors = "unavailable_via_public_api";
   std::int64_t ready_at_unix_ms = 0, stop_requested_at_unix_ms = 0, observed_milliseconds = 0;
   std::size_t mdns_observations_snapshot = 0, peer_dial_gate_calls = 0, inbound_gate_calls = 0;
   std::uint64_t direct_attempts = 0, authenticated_connections = 0, handshakes_completed = 0;
   std::uint64_t echo_streams = 0, application_bytes_sent = 0, application_bytes_received = 0;
   bool cleanup_complete = false;
   std::optional<failure_diagnostics> failure;
};
BOOST_DESCRIBE_STRUCT(quiet_receipt, (),
    (schema, implementation, outcome, role, status, local_peer_id, service_name, stop_reason, basis,
     capture_phase, service_errors, ready_at_unix_ms, stop_requested_at_unix_ms, observed_milliseconds,
     mdns_observations_snapshot, peer_dial_gate_calls, inbound_gate_calls, direct_attempts,
     authenticated_connections, handshakes_completed, echo_streams, application_bytes_sent,
     application_bytes_received, cleanup_complete, failure))

// Opt-in harness protocol: atomically publish "<positive epoch> snapshot\n".
// Each new epoch captures once; repeat with a higher epoch to sample again.
struct lifecycle_receipt {
   std::string schema = "forge.mdns.lifecycle.v1";
   std::uint64_t epoch = 0;
   std::int64_t pid = 0;
   std::string request = "snapshot", capture_phase = "after_echo_before_stop";
   std::string local_peer_id, topology_phase, capture_error;
   std::string service_errors = "unavailable_via_public_api";
   std::size_t mdns_observations = 0, active_operations = 0;
   std::size_t memory = 0, file_descriptors = 0, active_dials = 0;
   std::size_t inbound_connections = 0, outbound_connections = 0;
   std::size_t inbound_streams = 0, outbound_streams = 0;
   std::uint64_t failed_refreshes = 0, sessions_opened = 0, sessions_closed = 0;
};
BOOST_DESCRIBE_STRUCT(lifecycle_receipt, (),
    (schema, epoch, pid, request, capture_phase, local_peer_id, topology_phase, capture_error,
     service_errors, mdns_observations, active_operations, memory, file_descriptors, active_dials,
     inbound_connections, outbound_connections, inbound_streams, outbound_streams,
     failed_refreshes, sessions_opened, sessions_closed))

class connection_observer final : public forge::net::p2p::connection_gater {
 public:
   connection_observer(bool listener, std::function<std::size_t()> observations);
   bool intercept_peer_dial(const forge::net::p2p::peer_id&) noexcept override;
   bool intercept_accept(const forge::net::p2p::connection_endpoints&) noexcept override;
   bool intercept_upgraded(forge::net::p2p::connection_direction, const forge::net::p2p::peer_id&,
                           const forge::net::p2p::connection_endpoints&) noexcept override;
   struct evidence {
      std::optional<forge::net::p2p::peer_id> peer;
      std::optional<forge::net::p2p::connection_endpoints> endpoints;
      std::size_t observations_before_dial = 0;
      std::size_t peer_dial_gate_calls = 0;
      std::size_t inbound_gate_calls = 0;
      std::size_t upgraded = 0;
      bool invalid = false;
   };
   [[nodiscard]] evidence snapshot() const;

 private:
   bool listener_;
   std::function<std::size_t()> observations_;
   mutable std::mutex mutex_;
   evidence evidence_;
};

int run(const arguments&, const support&);

} // namespace forge::test::libp2p_interop::forge_mdns_fixture
