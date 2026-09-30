#pragma once

namespace forge::plugins::net::p2p::node {

class object_peer_state_adapter final : public forge::net::p2p::peer_store::persistence {
 public:
   [[nodiscard]] static boost::asio::awaitable<std::shared_ptr<object_peer_state_adapter>>
   async_open(forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
              forge::net::p2p::peer_store::options limits, bool reset_incompatible_cache = false);

   boost::asio::awaitable<forge::net::p2p::peer_store::hydration_page>
   async_hydrate(forge::net::p2p::peer_store::hydration_request request) override;
   boost::asio::awaitable<forge::net::p2p::peer_store::apply_result>
   async_apply(forge::net::p2p::peer_store::mutation_batch batch) override;
   boost::asio::awaitable<forge::net::p2p::peer_store::prune_result>
   async_prune_expired(std::chrono::system_clock::time_point now, std::size_t limit) override;
   boost::asio::awaitable<void> async_flush() override;
   boost::asio::awaitable<void> async_close() override;

 private:
   object_peer_state_adapter(forge::plugins::db::store::api* db, forge::plugins::db::store::store_handle store,
                             forge::net::p2p::peer_store::options limits);

   void ensure_open() const;

   forge::plugins::db::store::api* db_ = nullptr;
   forge::plugins::db::store::store_handle store_;
   forge::net::p2p::peer_store::options limits_;
   std::atomic_bool closed_{false};
   std::atomic_uint8_t next_prune_kind_{0};
};

} // namespace forge::plugins::net::p2p::node
