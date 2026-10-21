#pragma once

namespace forge::plugins::swarm::node {

class peer_capabilities;

struct plugin::impl {
   impl(forge::app::plugin_lifecycle, plugin_options);
   std::shared_ptr<forge::swarm::runtime::engine> require_engine() const;
   std::shared_ptr<forge::swarm::storage::catalog> require_catalog() const;
   std::shared_ptr<peer_capabilities> require_capabilities() const;
   void request_stop() noexcept;
   static boost::asio::awaitable<void> start(std::shared_ptr<impl>);
   static boost::asio::awaitable<void> close(std::shared_ptr<impl>);

   const forge::app::plugin_lifecycle lifecycle;
   const plugin_options options;
   mutable std::mutex mutex;
   config settings;
   std::stop_source stop;
   forge::asio::task::scheduler* scheduler = nullptr;
   forge::asio::compute::executor cpu;
   std::shared_ptr<forge::plugins::net::p2p::node::api> p2p;
   std::shared_ptr<forge::plugins::net::p2p::node::dht_api> dht;
   std::shared_ptr<forge::plugins::db::store::api> database;
   forge::plugins::db::store::object_handle objects;
   std::shared_ptr<peer_capabilities> capabilities;
   std::shared_ptr<forge::swarm::storage::catalog> catalog;
   std::shared_ptr<forge::swarm::runtime::engine> engine;
   std::shared_ptr<forge::api::core::registry> exports;
   std::shared_ptr<forge::api::p2p::publication> publication;
   std::unique_ptr<std::stop_callback<std::function<void()>>> publication_stop;
};

} // namespace forge::plugins::swarm::node
