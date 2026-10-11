#pragma once

namespace forge::plugins::net::p2p::node::detail {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
void validate_pubsub_config(const pubsub_config& value);
[[nodiscard]] forge::net::p2p::pubsub::options pubsub_options_for(const pubsub_config& settings);

} // namespace forge::plugins::net::p2p::node::detail
