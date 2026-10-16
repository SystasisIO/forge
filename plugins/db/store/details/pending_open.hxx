#pragma once

namespace forge::plugins::db::store {

struct pending_open {
   std::string name;
   std::optional<store_config> config;
   store_options options;
   bool initialize_layers = true;
   bool owns_driver = false;
   std::shared_ptr<forge::db::core::driver> driver;
   std::shared_ptr<forge::db::object::store> objects;
   std::shared_ptr<forge::db::blob::store> blobs;
   std::shared_ptr<forge::db::revision::store> revisions;
};

} // namespace forge::plugins::db::store
