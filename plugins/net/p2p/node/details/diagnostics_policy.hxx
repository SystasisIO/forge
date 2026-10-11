#pragma once

namespace forge::plugins::net::p2p::node::detail {

[[nodiscard]] forge::net::p2p::diagnostics::options configured_options(const diagnostics_config& value);
[[nodiscard]] std::vector<forge::net::p2p::diagnostics::peer>
filter_peers(const forge::net::p2p::diagnostics::snapshot& snapshot, const peer_filter& value);

} // namespace forge::plugins::net::p2p::node::detail
