#pragma once

namespace forge::plugins::net::p2p::node {

class plugin::dht_api_impl final : public dht_api {
 public:
   explicit dht_api_impl(std::shared_ptr<plugin::impl> impl);

   boost::asio::awaitable<forge::net::p2p::dht::query_result>
   find_peer(forge::net::p2p::protocol_id profile, forge::net::p2p::peer_id peer,
             forge::net::p2p::dht::query_options options) override;
   boost::asio::awaitable<forge::net::p2p::provider_registration>
   provide(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
           forge::net::p2p::dht::query_options options) override;
   boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>>
   find_providers(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
                  forge::net::p2p::dht::query_options options) override;
   boost::asio::awaitable<forge::net::p2p::dht::value_put_result>
   put_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::record value,
             forge::net::p2p::dht::query_options options) override;
   boost::asio::awaitable<forge::net::p2p::dht::value_get_result>
   get_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
             forge::net::p2p::dht::query_options options) override;
   [[nodiscard]] forge::net::p2p::ipns::record
   create_ipns_record(std::span<const std::uint8_t> value, std::uint64_t sequence,
                       forge::chrono::timestamp eol, std::chrono::nanoseconds ttl,
                       forge::net::p2p::ipns::create_options options) const override;

 private:
   static boost::asio::awaitable<forge::net::p2p::dht::query_result>
   find_peer_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                    forge::net::p2p::peer_id peer, forge::net::p2p::dht::query_options options);
   static boost::asio::awaitable<forge::net::p2p::provider_registration>
   provide_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                  forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options);
   static boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>>
   find_providers_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                         forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options);
   static boost::asio::awaitable<forge::net::p2p::dht::value_put_result>
   put_value_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                    forge::net::p2p::dht::record value, forge::net::p2p::dht::query_options options);
   static boost::asio::awaitable<forge::net::p2p::dht::value_get_result>
   get_value_owned(std::shared_ptr<plugin::impl> state, forge::net::p2p::protocol_id profile,
                    forge::net::p2p::dht::key key, forge::net::p2p::dht::query_options options);

   std::shared_ptr<plugin::impl> impl_;
};

} // namespace forge::plugins::net::p2p::node
