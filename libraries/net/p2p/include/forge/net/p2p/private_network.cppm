module;

#include <memory>

export module forge.net.p2p.private_network;

export import forge.net.p2p.exceptions;
export import forge.net.pnet.protector;

export namespace forge::net::p2p::private_network {

enum class internet_egress_policy {
   deny_external,
   allow_internet,
};

struct options {
   std::shared_ptr<const forge::net::pnet::protector> protector;
   internet_egress_policy internet_egress = internet_egress_policy::deny_external;
};

void validate(const options& value);

} // namespace forge::net::p2p::private_network
