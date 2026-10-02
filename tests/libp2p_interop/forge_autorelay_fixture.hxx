#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

// Test-only composition; production node remains the lifecycle/relay owner.
namespace forge::test::libp2p_interop {

struct forge_autorelay_fixture {
   struct support {
      std::function<forge::net::p2p::node::options(const std::map<std::string, std::string>&)> make_options;
      std::function<forge::net::p2p::endpoint(std::string_view)> listen_endpoint;
      std::function<void(forge::net::p2p::node&)> register_echo;
   };

   static int run(const std::map<std::string, std::string>& args, const support& composition);
};

} // namespace forge::test::libp2p_interop
