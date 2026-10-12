#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace forge::plugins::net::p2p::node::detail {

[[nodiscard]] std::chrono::milliseconds to_ms(std::uint64_t value);
void patch_size(std::size_t& target, const std::optional<std::uint64_t>& value);
void patch_time(std::chrono::milliseconds& target, const std::optional<std::uint64_t>& value);
[[nodiscard]] std::optional<forge::net::p2p::pubsub::scoring_params> pubsub_scoring_for(
    const std::optional<pubsub_config::scoring_config>& settings);
void validate_pubsub_config(const pubsub_config& value);
[[nodiscard]] forge::net::p2p::pubsub::options pubsub_options_for(const pubsub_config& settings);

} // namespace forge::plugins::net::p2p::node::detail
