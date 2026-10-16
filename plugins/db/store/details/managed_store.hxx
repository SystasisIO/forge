#pragma once

namespace forge::plugins::db::store {
class activation_state;

struct managed_store {
   std::string name;
   std::string driver_name;
   std::optional<std::string> durability;
   std::string path;
   std::vector<std::string> families;
   store_options options;
   bool initialize_layers = true;
   bool owns_driver = false;
   std::shared_ptr<forge::db::core::driver> driver;
   std::shared_ptr<forge::db::object::store> objects;
   std::shared_ptr<forge::db::blob::store> blobs;
   std::shared_ptr<forge::db::revision::store> revisions;
   bool opened = false;
   bool started = false;
   std::shared_ptr<activation_state> activation;
   bool strict_running = false;
   bool strict_closed = false;
   bool activation_claimed = false;
   bool activation_io = false;
   std::exception_ptr strict_failure;
};

} // namespace forge::plugins::db::store
