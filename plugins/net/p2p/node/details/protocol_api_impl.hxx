#pragma once

namespace forge::plugins::net::p2p::node {
class plugin::protocol_api_impl final : public protocol_api {
 public:
   explicit protocol_api_impl(std::shared_ptr<plugin::impl> owner);
   protocol_registration register_protocol(forge::net::p2p::protocol_id protocol,
       forge::net::p2p::node::protocol_handler handler) override;
   protocol_registration replace_protocol(const protocol_registration& registration,
       forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) override;
   boost::asio::awaitable<forge::net::p2p::stream> open(forge::net::p2p::peer_id peer,
       forge::net::p2p::protocol_id protocol, forge::net::p2p::node::open_options options) override;
 private:
   static boost::asio::awaitable<forge::net::p2p::stream> open_owned(std::shared_ptr<plugin::impl> owner,
       forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol,
       forge::net::p2p::node::open_options options);
   std::shared_ptr<plugin::impl> owner_;
};
}
