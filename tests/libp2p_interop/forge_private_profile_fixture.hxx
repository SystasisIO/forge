#pragma once

#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Test-only declarations, included after the public runtime/node/stream imports.
namespace forge::test::libp2p_interop {

struct private_profile_support {
   std::function<forge::net::p2p::node::options(const std::map<std::string, std::string>&)> make_options;
   std::function<void(forge::net::p2p::node&)> register_echo;
   std::function<std::vector<std::uint8_t>(const forge::net::p2p::node&)> signed_record;
};

int run_private_profile_fixture(const std::map<std::string, std::string>&, const private_profile_support&);
int private_profile_self_test();

} // namespace forge::test::libp2p_interop
