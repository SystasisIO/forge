#pragma once

namespace forge::plugins::net::p2p::node {

class object_dht_record_store_adapter final : public forge::net::p2p::dht::record_store::persistence {
 public:
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<object_dht_record_store_adapter>>
   async_open(forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
              forge::net::p2p::protocol_id profile, forge::net::p2p::dht::record_store::options limits);

   boost::asio::awaitable<forge::net::p2p::dht::record_store::hydration_page>
   async_hydrate(forge::net::p2p::dht::record_store::hydration_request request) override;
   boost::asio::awaitable<forge::net::p2p::dht::record_store::apply_result>
   async_apply(forge::net::p2p::dht::record_store::mutation_batch batch) override;
   boost::asio::awaitable<forge::net::p2p::dht::record_store::prune_result>
   async_prune_expired(std::chrono::system_clock::time_point now, std::size_t limit) override;
   boost::asio::awaitable<void> async_flush() override;
   boost::asio::awaitable<void> async_close() override;

 private:
   object_dht_record_store_adapter(forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
                                   forge::net::p2p::protocol_id profile, std::size_t max_record_bytes);

   void ensure_open() const;

   forge::plugins::db::store::api* db_ = nullptr;
   forge::plugins::db::store::store_handle store_;
   forge::net::p2p::protocol_id profile_;
   std::size_t max_record_bytes_ = 0;
   std::atomic_bool closed_{false};
};

} // namespace forge::plugins::net::p2p::node
