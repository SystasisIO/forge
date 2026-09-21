#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>

// This test-only boundary is included after the public P2P module imports.
namespace forge::test::libp2p_interop {

using fixture_arguments = std::map<std::string, std::string>;

class autonat_connection_observation {
 public:
   virtual ~autonat_connection_observation() noexcept;

   [[nodiscard]] virtual bool complete() const noexcept = 0;
   [[nodiscard]] virtual std::string failure_reason() const = 0;
   [[nodiscard]] virtual std::string json() const = 0;
};

[[nodiscard]] std::pair<std::shared_ptr<forge::net::p2p::connection_gater>,
                        std::shared_ptr<autonat_connection_observation>>
make_autonat_connection_observer(std::shared_ptr<forge::net::p2p::connection_gater> existing);

struct autonat_fixture_node {
   std::unique_ptr<forge::net::p2p::node> value;
   std::shared_ptr<autonat_connection_observation> connection_observation;
};

struct autonat_fixture_support {
   std::function<autonat_fixture_node(forge::asio::runtime&, const fixture_arguments&, bool service)> make_node;
};

int run_forge_autonat_fixture(const fixture_arguments& args, const autonat_fixture_support& support);

} // namespace forge::test::libp2p_interop
