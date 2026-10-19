#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/thread_pool.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <unistd.h>
#include "../swarm_storage/memory_driver.hpp"

import forge.swarm.storage.file_store;
import forge.swarm.storage.catalog;

namespace core = forge::swarm::core;
namespace storage = forge::swarm::storage;

boost::asio::awaitable<void> exercise(std::filesystem::path destination,
   forge::asio::compute::executor disk, forge::asio::compute::executor cpu) {
   auto database = boost::asio::thread_pool{1};
   auto driver = std::make_shared<swarm_test::memory_driver>();
   auto objects = co_await forge::db::object::store::open(driver);
   storage::catalog::register_objects(objects);
   auto catalog = co_await storage::catalog::async_open(objects, {cpu, database.get_executor()});
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   auto manifest = core::descriptor{core::descriptor_version::v1, core::block_size,
      {{{"a"}, 1U, core::hash_file(bytes, core::block_size).root, {}}}};
   const auto row = co_await catalog->async_create(manifest, destination.string(), {});
   auto checkpoint = co_await catalog->async_checkpoint_store(row.swarm, row.locator);
   auto store = co_await storage::file_store::async_open(
      {.manifest = manifest, .destination = destination, .disk = disk, .cpu = cpu, .checkpoint = checkpoint});
   auto writer = store->begin({0, 0});
   co_await writer->async_write(0, {'a'});
   co_await writer->async_commit();
   co_await store->async_publish();
   co_await store->async_close();
   const auto saved = co_await catalog->async_find(row.swarm);
   if (!saved || !saved->checkpoint || !saved->checkpoint->published) { throw std::runtime_error{"checkpoint missing"}; }
   co_await driver->async_close();
}

int main() {
   auto name = (std::filesystem::temp_directory_path() / "forge-package-swarm-XXXXXX").string();
   if (!::mkdtemp(name.data())) { return 1; }
   const auto root = std::filesystem::canonical(name);
   auto disk = forge::asio::compute::pool{{.worker_threads = 1}};
   auto cpu = forge::asio::compute::pool{{.worker_threads = 1}};
   auto io = boost::asio::io_context{};
   auto done = boost::asio::co_spawn(io, exercise(root / "file", disk.get_executor(), cpu.get_executor()), boost::asio::use_future);
   io.run();
   try { done.get(); } catch (...) { std::filesystem::remove_all(root); throw; }
   const auto valid = std::filesystem::file_size(root / "file") == 1U;
   std::filesystem::remove_all(root);
   return valid ? 0 : 2;
}
