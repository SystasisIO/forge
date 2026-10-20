#include <boost/asio/thread_pool.hpp>
#include <cstddef>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include "support.hpp"

import forge.swarm.storage.catalog;
import forge.swarm.storage.file_store;
import forge.db.core.driver;
import forge.db.object.hooks;
import forge.db.object.store;
import forge.raw.raw;

#if defined(FORGE_TEST_HAS_MDBX)
import forge.asio.affine;
import forge.db.mdbx.driver;
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
import forge.db.rocksdb.driver;
#endif

namespace {

using namespace swarm_test;
namespace meta = forge::swarm::db;
namespace dbo = forge::db::object;

class crash_observer final : public dbo::observer {
 public:
   crash_observer(const meta::content& row, bool published)
      : _row(row.id), _swarm(row.swarm), _published(published) {}

   boost::asio::awaitable<void> after_commit(const dbo::change_set& changes) override {
      for (const auto& mutation : changes.mutations) {
         if (mutation.id.space != 81U || mutation.id.type != 2U || !mutation.after) continue;
         auto encoded = std::vector<std::uint8_t>{};
         encoded.reserve(mutation.after->size());
         for (const auto byte : *mutation.after) encoded.push_back(std::to_integer<std::uint8_t>(byte));
         const auto row = forge::raw::unpack_exact<meta::content>(encoded);
         if (row.id != _row || row.swarm != _swarm || mutation.id.instance != row.id.instance) continue;
         if (row.checkpoint && row.checkpoint->published == _published) {
            // The real Core commit completed. catalog's subsequent durable flush has NOT run.
            // Deliberately skip MDBX close / RocksDB teardown: process-crash, not power-loss proof.
            ::_exit(77);
         }
      }
      co_return;
   }

 private:
   const meta::content::id_t _row;
   const core::swarm_id _swarm;
   const bool _published;
};

[[noreturn]] void failure(int code, const char* message) {
   std::fputs(message, stderr);
   std::fflush(stderr);
   ::_exit(code);
}

} // namespace

int main(int argc, char** argv) {
   try {
      if (argc != 4) throw std::invalid_argument{"expected root, backend and crash point"};
      const auto root = std::filesystem::path{argv[1]};
      const auto backend = std::string{argv[2]};
      const auto point = std::string_view{argv[3]};
      if (root.empty() || (backend != "mdbx" && backend != "rocksdb") ||
          (point != "staged" && point != "published")) {
         throw std::invalid_argument{"invalid crash helper arguments"};
      }
      std::filesystem::create_directories(root);
      auto disk = forge::asio::compute::pool{{
         .worker_threads = 2, .max_pending_tasks = 16, .max_waiting_submissions = 16,
         .thread_name = "swarm-crash-disk"}};
      auto cpu = forge::asio::compute::pool{{
         .worker_threads = 2, .max_pending_tasks = 16, .max_waiting_submissions = 16,
         .thread_name = "swarm-crash-cpu"}};
      auto database = boost::asio::thread_pool{1};
      auto driver = std::shared_ptr<forge::db::core::driver>{};
#if defined(FORGE_TEST_HAS_MDBX)
      if (backend == "mdbx") {
         driver = run(forge::db::mdbx::driver::open(
            {.path = (root / backend).string(), .families = {"objectdb"}},
            forge::asio::affine::lane::options{.thread_name = "swarm-crash-mdbx"}));
      }
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
      if (backend == "rocksdb") {
         driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
            .path = (root / backend).string(), .families = {"objectdb"}});
      }
#endif
      if (!driver) throw std::runtime_error{"requested crash backend is unavailable"};
      auto objects = run(dbo::store::open(driver));
      storage::catalog::register_objects(objects);
      auto catalog = run(storage::catalog::async_open(objects, {cpu.get_executor(), database.get_executor()}));
      const auto bytes = data(17);
      const auto manifest = describe({bytes});
      const auto destination = root / "result";
      const auto row = run(catalog->async_create(manifest, destination.string(), {}));
      auto checkpoint = run(catalog->async_checkpoint_store(row.swarm, row.locator));
      objects.add_observer(std::make_shared<crash_observer>(row, point == "published"));

      auto options = storage::file_store::options{
         .manifest = manifest, .destination = destination, .mode = storage::open_mode::create,
         .disk = disk.get_executor(), .cpu = cpu.get_executor(), .checkpoint = std::move(checkpoint)};
      auto files = run(storage::file_store::async_open(std::move(options)));
      fill(*files, manifest, {bytes});
      run(files->async_publish());
      failure(78, "metadata crash point was not reached\n");
   } catch (...) {
      failure(79, "metadata crash helper setup failed\n");
   }
}
