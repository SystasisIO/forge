#pragma once

namespace forge::swarm::storage::detail {

class catalog_store_access final : public catalog::access {
 public:
   explicit catalog_store_access(forge::db::object::store);
   forge::db::object::store::options runtime_options() const override;
   boost::asio::awaitable<forge::db::object::snapshot> begin_read() override;
   boost::asio::awaitable<forge::db::object::transaction> begin_transaction() override;
   boost::asio::awaitable<void> flush() override;
 private:
   forge::db::object::store _objects;
};

} // namespace forge::swarm::storage::detail
