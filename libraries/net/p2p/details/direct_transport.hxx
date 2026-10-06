#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>

#include "session_teardown.hxx"

namespace forge::net::p2p {

class cancellation_latch;
struct libp2p_identity_material;
enum class upgrade_role : std::uint8_t;

} // namespace forge::net::p2p

namespace forge::net::p2p::detail {
class connection_gate;
class coordinated_dial;
}

namespace forge::net::p2p::direct {

using authenticated_admission_handler = std::function<void(const peer_id&)>;
using tcp_transport_progress_handler = std::function<void()>;
// Called once after source selection, before opening a dial socket. Borrowed
// QUIC uses zero new descriptors; TCP (including port reuse) still uses one.
using native_socket_admission_handler = std::function<void(std::size_t)>;

// TCP progress is advisory scheduler input. It must never alter transport
// ownership or cause an already-connected socket to skip terminal cleanup.
void notify_tcp_transport_progress(const tcp_transport_progress_handler& handler) noexcept;

[[nodiscard]] bool owns_coordinated_source(const endpoint& listener, const endpoint& source);
[[nodiscard]] endpoint select_coordinated_source(std::span<const endpoint> listeners, const endpoint& remote,
                                                 const std::optional<endpoint>& requested);
[[nodiscard]] std::optional<endpoint> select_dial_source(boost::asio::io_context& context,
                                                         resource_manager& resources,
                                                         std::span<const endpoint> listeners, const endpoint& remote);

[[nodiscard]] resource_manager::dial_reservation reserve_coordinated_dial(resource_manager& resources,
                                                                          const peer_id& peer);
[[nodiscard]] resource_manager::session_reservation
reserve_coordinated_session(resource_manager& resources, resource_manager::session_direction direction);
void establish_coordinated_session(resource_manager::session_reservation& admission, const peer_id& expected,
                                   const peer_id& actual, resource_manager::session_direction direction,
                                   const authenticated_admission_handler& authenticated);

struct coordinated_tcp_upgrade {
   peer_id expected_peer;
   endpoint local_source;
   upgrade_role role;
   std::chrono::steady_clock::time_point expires;
   std::shared_ptr<cancellation_latch> cancellation;
   std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner;
};

// Shared with the native worker; admission cannot be released before socket
// cleanup merely because the caller unwinds during security negotiation.
struct native_connection_lifetime {
   std::shared_ptr<void> source;
   std::shared_ptr<void> source_admission;
   std::shared_ptr<resource_manager::session_reservation> admission;
   std::optional<resource_manager::file_descriptor_reservation> descriptor;
   std::shared_ptr<void> parent;
};

struct connection {
   peer_id peer;
   forge::net::transport::session session;
   std::optional<forge::net::p2p::endpoint> local_endpoint;
   std::optional<forge::net::p2p::endpoint> remote_endpoint;
   std::optional<resource_manager::session_reservation> admission;
   std::shared_ptr<void> native_lifetime;
   peer_authentication authentication = peer_authentication::unverified;
   protocol_id muxer;
   bool used_early_muxer_negotiation = false;
   std::optional<upgrade_role> role;
   decltype(node::session_info::security_role) security_role;
   decltype(node::session_info::yamux_role) yamux_role;
   // Native profile receipt; never inferred from a session to the same peer.
   std::shared_ptr<forge::net::p2p::detail::coordinated_dial> coordinated_owner;
   bool coordinated_inbound_worker = false;
};

// An accepted connection is not yet visible to node session registries. Its
// admission and native lifetime remain owned until the lower close completes.
boost::asio::awaitable<void> async_discard_unpublished(connection& value);

struct profile {
   std::function<bool(const forge::net::p2p::endpoint&)> supports;
   std::function<bool()> listening;
   std::function<std::vector<forge::net::p2p::endpoint>()> local_endpoints;
   std::function<forge::net::p2p::endpoint(forge::net::p2p::endpoint)> listen;
   std::function<void()> stop;
   std::function<boost::asio::awaitable<void>()> async_stop;
   std::function<boost::asio::awaitable<connection>(
       forge::net::p2p::endpoint, const node::connect_options&, std::shared_ptr<forge::net::p2p::cancellation_latch>,
       std::shared_ptr<void>, authenticated_admission_handler, tcp_transport_progress_handler)>
       async_connect;
   std::function<boost::asio::awaitable<connection>(
       endpoint, const node::connect_options&, std::shared_ptr<cancellation_latch>, std::shared_ptr<void>,
       authenticated_admission_handler, tcp_transport_progress_handler, native_socket_admission_handler)>
       async_connect_admitted;
   std::function<boost::asio::awaitable<connection>(endpoint, peer_id, upgrade_role, std::chrono::milliseconds,
                                                    std::shared_ptr<cancellation_latch>,
                                                    authenticated_admission_handler, std::optional<endpoint>)>
       async_connect_coordinated;
   std::function<boost::asio::awaitable<connection>(forge::net::p2p::endpoint)> async_accept;
   std::function<void(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>&)> prepare_coordinated;
   std::function<void(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>&)> release_coordinated;
   std::function<boost::asio::awaitable<connection>(std::shared_ptr<forge::net::p2p::detail::coordinated_dial>)>
       async_connect_coordinated_owned;
};

class registry {
 public:
   registry(forge::asio::runtime& runtime, const node::options& options, const libp2p_identity_material& identity,
            resource_manager resources, std::shared_ptr<forge::net::p2p::detail::connection_gate> gate = {});
   ~registry();

   registry(const registry&) = delete;
   registry& operator=(const registry&) = delete;

   [[nodiscard]] bool listening() const noexcept;
   [[nodiscard]] std::optional<forge::net::p2p::endpoint> local_endpoint() const;
   [[nodiscard]] std::vector<forge::net::p2p::endpoint> local_endpoints() const;

   void add(profile value);
   [[nodiscard]] forge::net::p2p::endpoint listen(forge::net::p2p::endpoint endpoint);
   void stop() noexcept;
   [[nodiscard]] forge::net::p2p::detail::session_teardown::operation teardown_operation() const;

   boost::asio::awaitable<connection>
   async_connect(forge::net::p2p::endpoint endpoint, const node::connect_options& options,
                 std::shared_ptr<forge::net::p2p::cancellation_latch> cancellation = {},
                 std::shared_ptr<void> native_lifetime = {}, authenticated_admission_handler authenticated = {},
                 tcp_transport_progress_handler tcp_transport_progress = {},
                 native_socket_admission_handler socket_admission = {});
   // Owns dial/session admission. Reuse is mandatory; a wildcard listener
   // requires the caller's concrete local_source, never an ephemeral fallback.
   // QUIC responders use the existing node accept loop to deliver an inbound
   // authenticated peer; they punch only, and never initiate a QUIC handshake.
   boost::asio::awaitable<connection> async_connect_coordinated(endpoint remote, peer_id expected_peer,
                                                                upgrade_role role, std::chrono::milliseconds budget,
                                                                std::shared_ptr<cancellation_latch> cancellation = {},
                                                                authenticated_admission_handler authenticated = {},
                                                                std::optional<endpoint> local_source = {});
   boost::asio::awaitable<connection> async_accept(forge::net::p2p::endpoint endpoint);
   void prepare_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner);
   void release_coordinated(const std::shared_ptr<forge::net::p2p::detail::coordinated_dial>& owner) noexcept;
   boost::asio::awaitable<connection> async_connect_coordinated(
       std::shared_ptr<forge::net::p2p::detail::coordinated_dial> owner);

 private:
   struct state;
   std::unique_ptr<state> state_;
};

struct registry::state {
   std::vector<profile> profiles;
};

void register_quic_profile(registry& value, forge::asio::runtime& runtime, const node::options& options,
                           resource_manager resources, std::shared_ptr<forge::net::p2p::detail::connection_gate> gate);
void register_tcp_profile(registry& value, forge::asio::runtime& runtime, const node::options& options,
                          const libp2p_identity_material& identity, resource_manager resources,
                          std::shared_ptr<forge::net::p2p::detail::connection_gate> gate);

} // namespace forge::net::p2p::direct
