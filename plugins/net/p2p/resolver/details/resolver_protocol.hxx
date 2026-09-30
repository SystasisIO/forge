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
