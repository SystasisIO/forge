#pragma once

namespace forge::plugins::net::p2p::resolver::detail {

class resolver_protocol
    : public forge::api::core::contract<resolver_protocol,
                                        forge::api::core::surface::local |
                                           forge::api::core::surface::remote> {
 public:
   explicit resolver_protocol(std::weak_ptr<plugin::impl> owner);
   virtual ~resolver_protocol();

   virtual boost::asio::awaitable<response> query(::forge::plugins::net::p2p::resolver::query request);

 protected:
   resolver_protocol() = default;

 private:
   std::weak_ptr<plugin::impl> owner_;
};

} // namespace forge::plugins::net::p2p::resolver::detail

namespace forge::plugins::net::p2p::resolver {

[[nodiscard]] forge::api::core::binding_plan make_resolver_protocol_plan(forge::api::core::registry& registry,
                                                                         std::weak_ptr<plugin::impl> owner);

[[nodiscard]] boost::asio::awaitable<std::vector<entry>>
query_resolver_peer(forge::plugins::net::p2p::node::api& p2p, forge::net::p2p::peer_id peer,
                    forge::net::p2p::protocol_id protocol, forge::plugins::net::p2p::node::remote_options options);

} // namespace forge::plugins::net::p2p::resolver
