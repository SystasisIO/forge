#pragma once

#include "interface_state.hxx"
#include "mdns_codec.hxx"
#include <boost/asio/ip/udp.hpp>
#include <optional>
#include <span>

import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.mdns_policy;
import forge.net.transport.datagram_io;

namespace forge::net::p2p::detail::mdns_wire {

[[nodiscard]] bool usable(const interface_state::address& address) noexcept;
[[nodiscard]] bool usable(const interface_state::interface& interface) noexcept;
[[nodiscard]] bool same_name(const mdns_codec::name& a, const mdns_codec::name& b);
[[nodiscard]] boost::asio::ip::udp::endpoint group(bool ipv6, std::uint32_t index);
[[nodiscard]] bool admit(const interface_state::interface& interface, bool ipv6,
                         const forge::net::transport::datagram_io::received& route);
[[nodiscard]] bool on_link(const interface_state::interface& interface, boost::asio::ip::address remote);
[[nodiscard]] mdns_codec::limits bounds(const mdns_policy& policy);
[[nodiscard]] mdns_codec::message query(const mdns_codec::name& service);
// Uses only usable addresses of this local interface; all receiver-local zones
// are removed from DNS bytes. Invalid/unrepresentable advertisements fail closed.
[[nodiscard]] mdns_codec::message advertisement(const mdns_policy& policy, const mdns_codec::name& service,
                                               std::string_view instance, const peer_id& local,
                                               const interface_state::interface& interface,
                                               std::span<const endpoint> listeners);
// Responses never cause responses. Known answers suppress matching records at
// >= half our TTL. Legacy replies echo id/questions, omit flush, and cap TTL at 10.
[[nodiscard]] std::optional<mdns_codec::message> answer(const mdns_codec::message& query,
                                                       const mdns_codec::message& advertisement, bool legacy);

} // namespace forge::net::p2p::detail::mdns_wire
