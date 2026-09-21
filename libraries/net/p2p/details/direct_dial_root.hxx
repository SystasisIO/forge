#pragma once

namespace forge::net::p2p::detail {

enum class direct_dial_provenance {
   persistent,
   transient_mdns,
};

struct direct_dial_root {
   forge::multiformats::multiaddr address;
   direct_dial_provenance provenance = direct_dial_provenance::persistent;
};

} // namespace forge::net::p2p::detail
