#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>

// Test-only composition after the public node/stream imports. No native bypass.
namespace forge::test::libp2p_interop {

struct coordinated_support {
   class dial_gate;
   struct exchange {
      std::vector<std::uint8_t> payload;
      std::vector<std::uint8_t> request_frame;
      std::vector<std::uint8_t> response_frame;
   };
   std::function<forge::net::p2p::node::options(const std::map<std::string, std::string>&)> make_options;
   std::function<void(forge::net::p2p::node&, forge::net::p2p::node::protocol_handler)> register_echo;
   std::function<boost::asio::awaitable<std::vector<std::uint8_t>>(forge::net::p2p::stream&,
                                                                   std::vector<std::uint8_t>*)>
       read_payload;
   std::function<boost::asio::awaitable<void>(forge::net::p2p::stream&, std::span<const std::uint8_t>,
                                              std::vector<std::uint8_t>*)>
       write_payload;
};

// The production gater denies all subsequent dials before socket admission.
// A probe can only use the already authenticated winning node session.
class coordinated_support::dial_gate final : public forge::net::p2p::connection_gater {
 public:
   void seal() noexcept;
   [[nodiscard]] bool intercept_peer_dial(const forge::net::p2p::peer_id&) noexcept override;
   [[nodiscard]] bool intercept_address_dial(const forge::net::p2p::peer_id&,
                                             const forge::net::p2p::endpoint&) noexcept override;

 private:
   std::atomic_bool _sealed{false};
};

int run_coordinated_fixture(const std::map<std::string, std::string>&, const coordinated_support&);
void coordinated_fixture_self_test();

} // namespace forge::test::libp2p_interop
