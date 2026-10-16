#include <boost/asio/awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

import forge.app.application_builder;
import forge.app.application_shell;
import forge.api.core.registry;
import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.config.core.document;
import forge.config.core.value;
import forge.db.blob.ref;
import forge.db.blob.snapshot;
import forge.db.blob.types;
import forge.db.core.exceptions;
import forge.plugins.db.store.api;
import forge.plugins.db.store.exceptions;
import forge.plugins.db.store.plugin;

namespace {
namespace db = forge::plugins::db::store;

struct directory_fence_gate {
   std::uint64_t device = 0;
   std::uint64_t inode = 0;
   std::atomic<unsigned> calls{0};
};
std::atomic<directory_fence_gate*> active_directory_fence{nullptr};
} // namespace

// Test executable only. Delegate every other real fsync unchanged. Fail exactly
// the second parent fence: pre-rename succeeds, post-rename returns EIO once.
extern "C" int fsync(int descriptor) {
   const auto saved_error = errno;
   using function = int (*)(int);
   static const auto actual = reinterpret_cast<function>(::dlsym(RTLD_NEXT, "fsync"));
   if (!actual) {
      errno = ENOSYS;
      return -1;
   }
   const auto gate = active_directory_fence.load(std::memory_order_acquire);
   struct stat value{};
   if (gate && ::fstat(descriptor, &value) == 0 && static_cast<std::uint64_t>(value.st_dev) == gate->device &&
       static_cast<std::uint64_t>(value.st_ino) == gate->inode &&
       gate->calls.fetch_add(1, std::memory_order_relaxed) == 1U) {
      errno = EIO;
      return -1;
   }
   errno = saved_error;
   return actual(descriptor);
}

namespace {

struct activation_fixture {
   std::filesystem::path parent;
   std::filesystem::path stage;
   std::unique_ptr<forge::app::application_shell> app;
   db::store_handle store;
   bool cleanup_failed = false;

   explicit activation_fixture(std::string durability = "durable-sync", bool with_compute = true,
                               std::string driver = "mdbx") {
      auto pattern = std::string{"/tmp/forge-store-activation-XXXXXX"};
      BOOST_REQUIRE(::mkdtemp(pattern.data()) != nullptr);
      parent = std::filesystem::canonical(pattern);
      stage = parent / "stage";
      try {
         BOOST_REQUIRE(::mkdir(stage.c_str(), 0700) == 0);
         auto builder = forge::app::application_builder{};
         builder.name("store-activation-test")
             .runtime({.worker_threads = 2, .thread_name = "store-activation-test"})
             .plugin(db::plugin::descriptor());
         if (with_compute)
            builder.compute({.worker_threads = 2, .max_pending_tasks = 8, .max_waiting_submissions = 8});
         app = std::move(builder).build();
         using value = forge::config::core::value;
         auto settings = value::object_type{{"name", "state"},
                                            {"driver", driver},
                                            {"path", stage.string()},
                                            {"object", value::object_type{}},
                                            {"blob", value::object_type{}},
                                            {"create-if-missing", true},
                                            {"create-missing-column-families", true}};
         if (driver == "mdbx")
            settings.emplace(
                "mdbx",
                value::object_type{{"durability", std::move(durability)},
                                   {"map", value::object_type{{"upper-size", std::uint64_t{64U * 1024U * 1024U}}}}});
         forge::config::core::document document;
         document.set("plugins.db.store.stores", value::array_type{std::move(settings)});
         app->configure(std::move(document));
         forge::asio::blocking::run(app->runtime(), app->startup());
         store = forge::asio::blocking::run(app->runtime(), app->apis().get<db::api>(db::api::ref())->store("state"));
      } catch (...) {
         const auto failure = std::current_exception();
         try {
            stop();
         } catch (...) {
            std::terminate(); // BEFORE constructor unwinding destroys borrowed owners.
         }
         std::error_code ignored;
         std::filesystem::remove_all(parent, ignored);
         std::rethrow_exception(failure);
      }
   }

   void stop() {
      if (!app)
         return;
      if (cleanup_failed)
         std::terminate(); // No hidden retry after an explicit cleanup failure.
      try {
         forge::asio::blocking::run(app->runtime(), app->shutdown());
      } catch (...) {
         cleanup_failed = true;
         throw;
      }
      store = {};
      app.reset();
   }

   ~activation_fixture() {
      if (cleanup_failed)
         std::terminate(); // BEFORE destroying any unproven borrowed owner.
      try {
         stop();
      } catch (...) {
         std::terminate();
      }
      std::error_code ignored;
      std::filesystem::remove_all(parent, ignored); // Exact mkdtemp-owned tree only, after joins.
   }
};

std::uint64_t inode(const std::filesystem::path& path) {
   struct stat value{};
   BOOST_REQUIRE(::lstat(path.c_str(), &value) == 0);
   return static_cast<std::uint64_t>(value.st_ino);
}

std::vector<std::byte> payload() {
   return {std::byte{1}, std::byte{7}, std::byte{19}};
}

void require_reopened_blob(const std::filesystem::path& root, forge::db::blob::ref<forge::db::blob::digest> ref) {
   auto builder = forge::app::application_builder{};
   builder.plugin(db::plugin::descriptor());
   auto other = std::move(builder).build();
   using value = forge::config::core::value;
   forge::config::core::document document;
   document.set("plugins.db.store.stores", value::array_type{value::object_type{{"name", "state"},
                                                                                {"driver", "mdbx"},
                                                                                {"path", root.string()},
                                                                                {"object", value::object_type{}},
                                                                                {"blob", value::object_type{}},
                                                                                {"create-if-missing", false}}});
   const auto stop_other = boost::scope::scope_exit{[&] {
      try {
         forge::asio::blocking::run(other->runtime(), other->shutdown());
      } catch (...) {
         std::terminate();
      }
   }};
   other->configure(std::move(document));
   forge::asio::blocking::run(other->runtime(), other->startup());
   auto reopened =
       forge::asio::blocking::run(other->runtime(), other->apis().get<db::api>(db::api::ref())->store("state"));
   BOOST_TEST(forge::asio::blocking::run(other->runtime(), reopened.blobs().get(ref)) == payload());
}

} // namespace

BOOST_AUTO_TEST_SUITE(store_activation)

#if FORGE_HAS_MDBX
BOOST_AUTO_TEST_CASE(strict_busy_keeps_same_owner_and_close_only_retry_releases_backend) {
   static_assert(!std::is_default_constructible_v<db::closed_store>);
   static_assert(!std::is_copy_constructible_v<db::closed_store>);
   auto fixture = activation_fixture{};
   auto& runtime = fixture.app->runtime();
   const auto ref = forge::asio::blocking::run(runtime, fixture.store.blobs().put(payload()));
   auto read = forge::asio::blocking::run(runtime, fixture.store.begin_read());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.store.close_for_activation()),
                     forge::db::core::exceptions::driver_busy);
   BOOST_TEST(forge::asio::blocking::run(runtime, read.blobs().get(ref)) == payload());
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.store.begin_transaction()), db::exceptions::stopped);
   // Unlike ordinary shutdown's supported busy-release policy, this explicit
   // strict claim must retain the SAME owner and report incomplete shutdown.
   fixture.cleanup_failed = true;
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.app->shutdown()),
                     forge::db::core::exceptions::driver_busy);
   read = {};
   auto closed = forge::asio::blocking::run(runtime, fixture.store.close_for_activation());
   // Explicit successful SAME-owner close permits a later explicit shutdown
   // retry. Any throw before this point takes the fixture fail-fast backstop.
   fixture.cleanup_failed = false;
   BOOST_TEST(!closed.state().published);
   BOOST_TEST(closed.state().inode == inode(fixture.stage));
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.store.close_for_activation()),
                     db::exceptions::activation_conflict);
   // Physical close, not merely Store ownership release: another real App can
   // reopen and read the SAME MDBX while the original App/capability still live.
   require_reopened_blob(fixture.stage, ref);
}

BOOST_AUTO_TEST_CASE(durable_stage_is_not_published_and_native_no_replace_preserves_existing_names) {
   auto fixture = activation_fixture{};
   auto& runtime = fixture.app->runtime();
   const auto ref = forge::asio::blocking::run(runtime, fixture.store.blobs().put(payload()));
   auto closed = forge::asio::blocking::run(runtime, fixture.store.close_for_activation());
   const auto original = closed.state();
   forge::asio::blocking::run(runtime, closed.confirm_durability());
   BOOST_CHECK(closed.state().phase == db::activation_phase::durable);
   BOOST_TEST(!closed.state().published);
   const auto destination = fixture.parent / "target";
   BOOST_TEST(!std::filesystem::exists(destination));
   BOOST_REQUIRE(::mkdir(destination.c_str(), 0700) == 0);
   const auto occupied = inode(destination);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(destination)),
                     db::exceptions::activation_conflict);
   BOOST_TEST(inode(destination) == occupied);
   BOOST_TEST(inode(fixture.stage) == original.inode);
   BOOST_REQUIRE(std::filesystem::remove(destination));
   BOOST_REQUIRE(::symlink("stage", destination.c_str()) == 0);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(destination)),
                     db::exceptions::activation_conflict);
   BOOST_REQUIRE(std::filesystem::remove(destination));
   forge::asio::blocking::run(runtime, closed.activate(destination));
   const auto published = closed.state();
   BOOST_TEST(published.published);
   BOOST_CHECK(published.phase == db::activation_phase::durable);
   BOOST_TEST(published.root == destination);
   BOOST_TEST(published.inode == original.inode);
   BOOST_TEST(!std::filesystem::exists(fixture.stage));
   BOOST_TEST(inode(destination) == original.inode);
   require_reopened_blob(destination, ref);
}

BOOST_AUTO_TEST_CASE(postrename_parent_fence_failure_retains_same_target_and_only_explicit_confirmation) {
   auto fixture = activation_fixture{};
   auto& runtime = fixture.app->runtime();
   auto closed = forge::asio::blocking::run(runtime, fixture.store.close_for_activation());
   const auto original = closed.state();
   struct stat parent{};
   BOOST_REQUIRE(::stat(fixture.parent.c_str(), &parent) == 0);
   auto gate = directory_fence_gate{.device = static_cast<std::uint64_t>(parent.st_dev),
                                    .inode = static_cast<std::uint64_t>(parent.st_ino)};
   const auto clear =
       boost::scope::scope_exit{[] { active_directory_fence.store(nullptr, std::memory_order_release); }};
   active_directory_fence.store(&gate, std::memory_order_release);
   const auto target = fixture.parent / "target";
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(target)),
                     db::exceptions::activation_incomplete);
   active_directory_fence.store(nullptr, std::memory_order_release); // Work/captures already retired.
   BOOST_TEST(gate.calls.load(std::memory_order_acquire) == 2U);
   const auto uncertain = closed.state();
   BOOST_TEST(uncertain.published);
   BOOST_CHECK(uncertain.phase == db::activation_phase::published_unconfirmed);
   BOOST_TEST(uncertain.root == target);
   BOOST_TEST(uncertain.inode == original.inode);
   BOOST_TEST(inode(target) == original.inode);
   BOOST_TEST(!std::filesystem::exists(fixture.stage));
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(fixture.parent / "second")),
                     db::exceptions::activation_conflict);
   forge::asio::blocking::run(runtime, closed.confirm_durability());
   BOOST_CHECK(closed.state().phase == db::activation_phase::durable);
   BOOST_TEST(closed.state().published);
   BOOST_TEST(inode(target) == original.inode);
   BOOST_TEST(!std::filesystem::exists(fixture.parent / "second"));
}

BOOST_AUTO_TEST_CASE(changed_root_identity_and_other_parent_never_publish) {
   auto fixture = activation_fixture{};
   auto& runtime = fixture.app->runtime();
   auto closed = forge::asio::blocking::run(runtime, fixture.store.close_for_activation());
   const auto original = closed.state();
   BOOST_REQUIRE(::mkdir((fixture.parent / "other").c_str(), 0700) == 0);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(fixture.parent / "other" / "target")),
                     db::exceptions::invalid_argument);
   const auto saved = fixture.parent / "saved";
   std::filesystem::rename(fixture.stage, saved);
   BOOST_REQUIRE(::mkdir(fixture.stage.c_str(), 0700) == 0);
   BOOST_TEST(inode(saved) == original.inode);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, closed.activate(fixture.parent / "target")),
                     db::exceptions::activation_incomplete);
   BOOST_TEST(!closed.state().published);
   BOOST_TEST(!std::filesystem::exists(fixture.parent / "target"));
   BOOST_TEST(inode(saved) == original.inode);
}

BOOST_AUTO_TEST_CASE(unsupported_durability_or_missing_compute_does_not_close_normal_store) {
   for (unsigned variant = 0; variant < 2; ++variant) {
      auto fixture = activation_fixture{variant == 0 ? "safe-nosync" : "durable-sync", variant == 0};
      auto& runtime = fixture.app->runtime();
      BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.store.close_for_activation()),
                        db::exceptions::unsupported_activation);
      const auto ref = forge::asio::blocking::run(runtime, fixture.store.blobs().put(payload()));
      BOOST_TEST(forge::asio::blocking::run(runtime, fixture.store.blobs().get(ref)) == payload());
   }
}
#endif

#if FORGE_HAS_ROCKSDB
BOOST_AUTO_TEST_CASE(unsupported_rocksdb_stays_available_for_ordinary_shutdown) {
   auto fixture = activation_fixture{"durable-sync", true, "rocksdb"};
   auto& runtime = fixture.app->runtime();
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, fixture.store.close_for_activation()),
                     db::exceptions::unsupported_activation);
   const auto ref = forge::asio::blocking::run(runtime, fixture.store.blobs().put(payload()));
   BOOST_TEST(forge::asio::blocking::run(runtime, fixture.store.blobs().get(ref)) == payload());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
