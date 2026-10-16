#pragma once

#include <map>

namespace forge::db::object {

struct runtime_state {
   runtime_state();

   std::shared_ptr<forge::asio::gate> write_gate;
   std::shared_ptr<forge::asio::gate> allocator_gate;
   std::map<forge::db::ids::object_id, std::uint64_t> next_instances;
};

struct store::impl {
   impl(std::shared_ptr<forge::db::core::driver> driver_value, store::config config_value, store::options options_value);

   boost::asio::awaitable<forge::db::core::transaction> open_write_transaction() const;
   boost::asio::awaitable<forge::db::core::snapshot> open_read_snapshot() const;
   boost::asio::awaitable<forge::db::object::header>
   initialize_header(forge::db::core::transaction& active) const;
   boost::asio::awaitable<forge::db::object::header> read_existing_header(forge::db::core::snapshot& active) const;
   boost::asio::awaitable<forge::db::ids::object_id>
   allocate_id(forge::db::ids::object_id type, forge::db::core::transaction& active, change_set& changes);
   boost::asio::awaitable<void> seal_allocations(transaction::allocation_seal_map seals);

   void register_object_type(forge::db::ids::object_id type, std::type_index model);
   void ensure_registered_type(forge::db::ids::object_id type, std::type_index model) const;

   std::shared_ptr<forge::db::core::driver> driver;
   std::shared_ptr<runtime_state> runtime;
   store::config config;
   store::options settings;
   forge::db::object::header header_value;
   std::map<forge::db::ids::object_id, std::type_index> registered;
   std::vector<std::shared_ptr<interceptor>> interceptors;
   std::vector<std::shared_ptr<observer>> observers;
};

} // namespace forge::db::object
