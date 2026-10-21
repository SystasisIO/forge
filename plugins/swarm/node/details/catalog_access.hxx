#pragma once

namespace forge::plugins::swarm::node {

class catalog_access final : public forge::swarm::storage::catalog::access {
 public:
   catalog_access(forge::plugins::db::store::object_handle, std::shared_ptr<forge::plugins::db::store::api>);
   forge::db::object::store::options runtime_options() const override;
   boost::asio::awaitable<forge::db::object::snapshot> begin_read() override;
   boost::asio::awaitable<forge::db::object::transaction> begin_transaction() override;
   boost::asio::awaitable<void> flush() override;
 private:
   forge::plugins::db::store::object_handle _objects;
   std::shared_ptr<forge::plugins::db::store::api> _database;
};

} // namespace forge::plugins::swarm::node
