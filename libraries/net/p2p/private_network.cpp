module;

#include <forge/exceptions/macros.hpp>

module forge.net.p2p.private_network;

namespace forge::net::p2p::private_network {

void validate(const options& value) {
   if (!value.protector) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P private-network profile requires a pnet protector");
   }
   if (value.internet_egress != internet_egress_policy::deny_external &&
       value.internet_egress != internet_egress_policy::allow_internet) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P private-network Internet-egress policy is invalid");
   }
}

} // namespace forge::net::p2p::private_network
