#pragma once

namespace forge::plugins::net::p2p::pubsub {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
[[nodiscard]] config decode_config(const forge::config::core::component_view& view);
void validate_config(const config& value);
[[nodiscard]] forge::net::p2p::pubsub::options core_options_for(const config& settings);

} // namespace forge::plugins::net::p2p::pubsub
