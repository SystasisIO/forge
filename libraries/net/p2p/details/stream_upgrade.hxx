#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p {

struct libp2p_identity_material;
class cancellation_latch;

// Security negotiation, TLS/Noise and Yamux role, not TCP connect/accept direction.
enum class upgrade_role : std::uint8_t {
   initiator,
   responder,
};

struct upgraded_session {
   peer_id peer;
   std::shared_ptr<forge::net::yamux::session> session;
   peer_authentication authentication = peer_authentication::unverified;
   protocol_id muxer;
   bool used_early_muxer_negotiation = false;
   std::optional<upgrade_role> role;
   decltype(node::session_info::security_role) security_role;
   decltype(node::session_info::yamux_role) yamux_role;
   std::optional<endpoint> circuit_endpoint;
   std::optional<std::uint64_t> carrier_session_id;
};

struct upgrade_callbacks {
   std::function<void(const peer_id&)> secured;
   std::function<void(const peer_id&)> established;
   std::function<void(const peer_id&)> upgraded;
};

namespace detail {

boost::asio::awaitable<protocol_id> select_private_stream_security_protocol(forge::net::p2p::stream& stream);

boost::asio::awaitable<protocol_id> accept_private_stream_security_protocol(forge::net::p2p::stream& stream);

} // namespace detail

boost::asio::awaitable<upgraded_session> upgrade_outbound_stream(forge::net::p2p::stream stream,
                                                                 const node::options& options,
                                                                 const libp2p_identity_material& identity,
                                                                 std::optional<peer_id> expected_peer,
                                                                 upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session> upgrade_inbound_stream(forge::net::p2p::stream stream,
                                                                const node::options& options,
                                                                const libp2p_identity_material& identity,
                                                                std::optional<peer_id> expected_peer,
                                                                upgrade_callbacks callbacks = {});

struct tcp_upgrade_deadline {
   boost::asio::io_context* context = nullptr;
   std::chrono::milliseconds timeout{0};
   std::shared_ptr<cancellation_latch> cancel_current;
};

boost::asio::awaitable<upgraded_session>
upgrade_tcp(forge::net::tcp::connection connection, const node::options& options,
             const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
             upgrade_role role, tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session> upgrade_outbound_tcp(forge::net::tcp::connection connection,
                                                              const node::options& options,
                                                              const libp2p_identity_material& identity,
                                                              std::optional<peer_id> expected_peer);

boost::asio::awaitable<upgraded_session> upgrade_inbound_tcp(forge::net::tcp::connection connection,
                                                             const node::options& options,
                                                             const libp2p_identity_material& identity,
                                                             std::optional<peer_id> expected_peer);

boost::asio::awaitable<upgraded_session>
upgrade_outbound_tcp(forge::net::tcp::connection connection, const node::options& options,
                     const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                     tcp_upgrade_deadline deadline, upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session>
upgrade_inbound_tcp(forge::net::tcp::connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline, upgrade_callbacks callbacks = {});

namespace detail {

struct cancel_cleanup {
   tcp_upgrade_deadline* deadline = nullptr;

   ~cancel_cleanup();
};

void set_cancel(tcp_upgrade_deadline& deadline, std::function<void()> cancel);
void clear_cancel(tcp_upgrade_deadline& deadline) noexcept;
[[nodiscard]] bool has_timeout(const tcp_upgrade_deadline& deadline) noexcept;

boost::asio::awaitable<upgraded_session>
finish_tls_outbound(forge::net::tcp::connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session>
finish_tls_inbound(forge::net::tcp::connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session>
finish_tls_outbound(forge::net::transport::stream_connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {});

boost::asio::awaitable<upgraded_session>
finish_tls_inbound(forge::net::transport::stream_connection connection, const node::options& options,
                    const libp2p_identity_material& identity, std::optional<peer_id> expected_peer,
                    tcp_upgrade_deadline deadline = {}, upgrade_callbacks callbacks = {});

} // namespace detail

} // namespace forge::net::p2p
