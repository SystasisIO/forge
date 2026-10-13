#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

import forge.net.p2p.dht;

namespace forge::test::libp2p_interop {

struct dht_value_fixture {
   forge::net::p2p::dht::key key;
   std::vector<std::uint8_t> value;

   [[nodiscard]] static bool is_scenario(std::string_view scenario) noexcept;
   [[nodiscard]] static dht_value_fixture for_scenario(std::string_view scenario);
};

} // namespace forge::test::libp2p_interop
