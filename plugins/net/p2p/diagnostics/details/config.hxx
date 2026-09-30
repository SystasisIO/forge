#pragma once

namespace forge::plugins::net::p2p::diagnostics {

[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
void validate_config(const config& value);
[[nodiscard]] forge::net::p2p::diagnostics::options configured_options(const config& value);
[[nodiscard]] std::vector<forge::net::p2p::diagnostics::peer>
filter_peers(const forge::net::p2p::diagnostics::snapshot& snapshot, const filter& filter);

} // namespace forge::plugins::net::p2p::diagnostics
