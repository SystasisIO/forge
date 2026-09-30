#pragma once

namespace forge::plugins::net::p2p::resolver {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
void validate_config(const config& value);
void validate_transport_options(const forge::api::transport::options& value);

} // namespace forge::plugins::net::p2p::resolver
