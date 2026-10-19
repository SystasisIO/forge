#include <boost/test/unit_test.hpp>
#include <boost/asio/thread_pool.hpp>
#include <array>
#include <chrono>
#include <csignal>
#include <cerrno>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include "support.hpp"

import forge.swarm.storage.catalog;
import forge.db.core.driver;
#if defined(FORGE_TEST_HAS_MDBX)
import forge.asio.affine;
import forge.db.mdbx.driver;
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
import forge.db.rocksdb.driver;
#endif

extern char** environ;

namespace {

using namespace swarm_test;

void crash_then_resume(const std::string& backend, const std::string& point) {
   auto env = fixture{};
   auto executable = std::string{FORGE_SWARM_METADATA_CRASH_HELPER};
   auto root = env.root.string();
   auto kind = backend;
   auto boundary = point;
   auto args = std::array<char*, 5>{executable.data(), root.data(), kind.data(), boundary.data(), nullptr};
   auto child = pid_t{};
   BOOST_REQUIRE_EQUAL(::posix_spawn(&child, executable.c_str(), nullptr, nullptr, args.data(), environ), 0);
   auto status = 0;
   auto reaped = false;
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{15};
   while (std::chrono::steady_clock::now() < deadline) {
      const auto result = ::waitpid(child, &status, WNOHANG);
      if (result == child) { reaped = true; break; }
      if (result < 0 && errno != EINTR) { break; }
      std::this_thread::sleep_for(std::chrono::milliseconds{5});
   }
   if (!reaped) {
      ::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {}
   }
   BOOST_REQUIRE_MESSAGE(reaped && WIFEXITED(status) && WEXITSTATUS(status) == 77,
      "process did not reach the exact post-commit crash boundary");
   BOOST_CHECK_EQUAL(std::filesystem::exists(env.root / "result"), point == "published");

   std::shared_ptr<forge::db::core::driver> driver;
#if defined(FORGE_TEST_HAS_MDBX)
   if (backend == "mdbx") {
      driver = run(forge::db::mdbx::driver::open(
         {.path = (env.root / backend).string(), .families = {"objectdb"}},
         forge::asio::affine::lane::options{.thread_name = "swarm-crash-reopen"}));
   }
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
   if (backend == "rocksdb") {
      driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
         .path = (env.root / backend).string(), .families = {"objectdb"}});
   }
#endif
   BOOST_REQUIRE(driver);
   auto database = boost::asio::thread_pool{1};
   auto objects = run(forge::db::object::store::open(driver));
   storage::catalog::register_objects(objects);
   auto catalog = run(storage::catalog::async_open(objects, {env.cpu.get_executor(), database.get_executor()}));
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   auto adapter = run(catalog->async_checkpoint_store(id, (env.root / "result").string()));
   const auto checkpoint = run(adapter->async_load(id, {}));
   BOOST_REQUIRE(checkpoint.has_value());
   BOOST_CHECK_EQUAL(checkpoint->published, point == "published");
   auto config = env.options(manifest, storage::open_mode::resume);
   config.checkpoint = adapter;
   auto files = run(storage::file_store::async_open(config));
   BOOST_CHECK(run(files->async_read({0, 0}, 0, 17)) == bytes);
   run(files->async_publish());
   run(files->async_close());
   files.reset();
   BOOST_CHECK(read_file(env.root / "result") == bytes);
   BOOST_CHECK(run(adapter->async_load(id, {}))->published);
   run(driver->async_close());
}

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_metadata_crash_tests)
#if defined(FORGE_TEST_HAS_MDBX)
BOOST_AUTO_TEST_CASE(mdbx_process_exit_after_staged_and_published_checkpoint_commit) {
   crash_then_resume("mdbx", "staged");
   crash_then_resume("mdbx", "published");
}
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
BOOST_AUTO_TEST_CASE(rocksdb_process_exit_after_staged_and_published_checkpoint_commit) {
   crash_then_resume("rocksdb", "staged");
   crash_then_resume("rocksdb", "published");
}
#endif
BOOST_AUTO_TEST_SUITE_END()
