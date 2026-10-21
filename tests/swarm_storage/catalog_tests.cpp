#include <boost/test/unit_test.hpp>
#include <limits>
#include <array>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <chrono>
#include <thread>
#include "support.hpp"
#include "memory_driver.hpp"

import forge.swarm.storage.catalog;
import forge.db.object.exceptions;
import forge.db.object.hooks;
import forge.asio.notification;

#if defined(FORGE_TEST_HAS_MDBX)
import forge.asio.affine;
import forge.db.mdbx.driver;
#endif
#if defined(FORGE_TEST_HAS_ROCKSDB)
import forge.db.rocksdb.driver;
#endif

using namespace swarm_test;
namespace meta = forge::swarm::db;
namespace dbo = forge::db::object;

namespace {

class catalog_access_probe final : public storage::catalog::access {
 public:
   catalog_access_probe(dbo::store objects, std::shared_ptr<dbc::driver> driver)
      : objects(std::move(objects)), driver(std::move(driver)) {}

   dbo::store::options runtime_options() const override { check(); return objects.runtime_options(); }
   boost::asio::awaitable<dbo::snapshot> begin_read() override {
      check();
      const auto epoch = released.epoch();
      entered.notify();
      if (hold_reads && epoch == 0U) {
         if (co_await released.async_wait_until(epoch, std::chrono::steady_clock::now() + std::chrono::seconds{5}) == epoch) {
            throw std::runtime_error{"catalog read barrier timed out"};
         }
      }
      check();
      co_return co_await objects.begin_read();
   }
   boost::asio::awaitable<dbo::transaction> begin_transaction() override {
      check();
      co_return co_await objects.begin_transaction();
   }
   boost::asio::awaitable<void> flush() override {
      check();
      ++flushes;
      if (fail_flush) { throw std::runtime_error{"access flush failed"}; }
      co_await driver->async_flush(true);
   }

   dbo::store objects;
   std::shared_ptr<dbc::driver> driver;
   std::atomic<bool> available{true};
   std::atomic<unsigned> flushes{0};
   bool hold_reads = false;
   bool fail_flush = false;
   forge::asio::notification entered;
   forge::asio::notification released;

 private:
   void check() const {
      if (!available.load()) {
         FORGE_THROW_EXCEPTION(core::exceptions::storage_closed, "guarded test access is closed");
      }
   }
};

// Same constrained registration surface as a named object_handle; no store is
// borrowed by register_objects after this synchronous call returns.
struct registration_view {
   dbo::store& objects;
   const bool& setup;
   template <dbo::application_object_model Object> void register_object() const {
      if (!setup) { throw std::logic_error{"registration is outside setup"}; }
      objects.register_object<Object>();
   }
};

struct catalog_fixture {
   fixture files;
   boost::asio::thread_pool database{1};
   std::shared_ptr<memory_driver> driver = std::make_shared<memory_driver>();
   dbo::store objects = run(dbo::store::open(driver));
   std::shared_ptr<storage::catalog> catalog;

   catalog_fixture() {
      storage::catalog::register_objects(objects);
      catalog = run(storage::catalog::async_open(objects, {files.cpu.get_executor(), database.get_executor()}));
   }
   ~catalog_fixture() { catalog.reset(); run(driver->async_close()); }
   core::descriptor manifest(std::size_t i = 0) {
      auto value = describe({data(17)});
      value.files[0].path = {"file-" + std::to_string(i)};
      return value;
   }
   meta::content create(std::size_t i = 0, storage::catalog::job_settings settings = {}) {
      return run(catalog->async_create(manifest(i), "locator-" + std::to_string(i), settings));
   }
};

core::resume_state complete(const core::descriptor& manifest, std::uint64_t generation) {
   return {1U, core::calculate_swarm_id(manifest), generation, false, {1U}, {}};
}

boost::asio::awaitable<std::optional<meta::content>> find_deferred(
   std::shared_ptr<storage::catalog> catalog, core::swarm_id id) {
   co_return co_await catalog->async_find(id);
}

boost::asio::awaitable<bool> close_preserving_caller_cancellation(
   std::shared_ptr<storage::catalog> catalog, boost::asio::cancellation_signal& signal, bool precancel) {
   auto filters = std::make_shared<std::array<unsigned, 2>>();
   co_await boost::asio::this_coro::reset_cancellation_state(
      [filters](boost::asio::cancellation_type_t type) {
         ++(*filters)[0];
         return type & boost::asio::cancellation_type::partial;
      },
      [filters](boost::asio::cancellation_type_t type) {
         ++(*filters)[1];
         return type & boost::asio::cancellation_type::partial;
      });
   // A caller with pending cancellation explicitly permits cleanup awaits.
   const auto throw_policy = !precancel;
   co_await boost::asio::this_coro::throw_if_cancelled(throw_policy);
   const auto executor = co_await boost::asio::this_coro::executor;
   auto interrupt_timer = [&signal, executor]() -> boost::asio::awaitable<bool> {
      auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{500}};
      boost::asio::post(executor, [&signal] {
         signal.emit(boost::asio::cancellation_type::terminal);
         signal.emit(boost::asio::cancellation_type::partial);
      });
      auto error = boost::system::error_code{};
      co_await timer.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      co_return error == boost::asio::error::operation_aborted;
   };
   if (precancel && !co_await interrupt_timer()) co_return false;
   const auto before = (co_await boost::asio::this_coro::cancellation_state).cancelled();
   if (precancel && before != boost::asio::cancellation_type::partial) co_return false;
   co_await catalog->async_close();
   const auto preserved = (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy &&
      (co_await boost::asio::this_coro::cancellation_state).cancelled() == before;
   const auto interrupted = co_await interrupt_timer();
   co_return preserved && interrupted &&
      (co_await boost::asio::this_coro::throw_if_cancelled()) == throw_policy &&
      (co_await boost::asio::this_coro::cancellation_state).cancelled() == boost::asio::cancellation_type::partial &&
      (*filters)[0] == (precancel ? 4U : 2U) && (*filters)[1] == (precancel ? 4U : 2U);
}

boost::asio::awaitable<void> parity(std::shared_ptr<dbc::driver> driver, fixture& files) {
   auto database = boost::asio::thread_pool{1};
   auto objects = co_await dbo::store::open(driver);
   auto setup = true;
   const auto registration = registration_view{objects, setup};
   storage::catalog::register_objects(registration);
   setup = false;
   BOOST_CHECK_THROW(storage::catalog::register_objects(registration), std::logic_error);
   auto access = std::make_shared<catalog_access_probe>(objects, driver);
   auto catalog = co_await storage::catalog::async_open(access, {files.cpu.get_executor(), database.get_executor()});
   const auto bytes = data(5U * core::block_size + 3U);
   const auto manifest = describe({bytes});
   const auto id = core::calculate_swarm_id(manifest);
   const auto locator = (files.root / "result").string();
   const auto row = co_await catalog->async_create(manifest, locator,
      {.state = meta::job_state::downloading, .priority = 7, .expires_at = 100});
   BOOST_CHECK_THROW(co_await catalog->async_create(manifest, "duplicate", {}), storage::exceptions::conflict);
   auto other_manifest = manifest;
   other_manifest.files[0].path = {"other"};
   BOOST_CHECK_THROW(co_await catalog->async_create(other_manifest, locator, {}), storage::exceptions::conflict);
   const auto other = co_await catalog->async_create(other_manifest, "other-locator",
      {.state = meta::job_state::paused, .priority = 4, .expires_at = 10});
   auto selected = co_await catalog->async_expiring_before(11, {.limit = 1});
   BOOST_REQUIRE_EQUAL(selected.items.size(), 1U);
   BOOST_CHECK(selected.items[0].id == other.id);
   auto old_handle = co_await catalog->async_checkpoint_store(other.swarm, other.locator);
   auto previous = complete(other_manifest, 9);
   co_await old_handle->async_save(previous, {});
   co_await old_handle->async_save(previous, {});
   --previous.generation;
   BOOST_CHECK_THROW(co_await old_handle->async_save(previous, {}), storage::exceptions::stale_checkpoint);
   co_await catalog->async_erase(other.swarm);
   const auto replacement = co_await catalog->async_create(other_manifest, "other-locator", {});
   BOOST_CHECK(replacement.id != other.id);
   BOOST_CHECK_THROW(co_await old_handle->async_load(other.swarm, {}), storage::exceptions::missing_metadata);
   auto adapter = co_await catalog->async_checkpoint_store(id, locator);
   auto config = files.options(manifest);
   config.checkpoint = adapter;
   auto store = co_await storage::file_store::async_open(config);
   {
      auto writer = store->begin({0, 0});
      co_await writer->async_write(0, {bytes.begin(), bytes.begin() + core::block_size});
   }
   const auto partial = co_await store->async_checkpoint();
   BOOST_CHECK_EQUAL(partial.unfinished.size(), 1U);
   co_await store->async_close();
   store.reset();
   config.mode = storage::open_mode::resume;
   store = co_await storage::file_store::async_open(config);
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   BOOST_REQUIRE_EQUAL(store->snapshot().unfinished.size(), 1U);
   // Reuse the retained first block; finish that piece and the remainder.
   for (std::uint32_t p = 0; p < core::piece_count(bytes.size(), manifest.piece_size); ++p) {
      auto writer = store->begin({0, p});
      const auto size = core::piece_length(manifest, {0, p});
      const auto start = std::size_t{p} * manifest.piece_size;
      for (auto offset = (p == 0U ? core::block_size : 0U); offset < size; offset += core::block_size) {
         const auto end = std::min<std::size_t>(offset + core::block_size, size);
         co_await writer->async_write(offset, {bytes.begin() + start + offset, bytes.begin() + start + end});
      }
      co_await writer->async_commit();
   }
   co_await catalog->async_update(id, {.state = meta::job_state::seeding, .priority = 11});
   co_await store->async_publish();
   co_await store->async_close();
   store.reset();
   const auto saved = co_await catalog->async_find(id);
   BOOST_REQUIRE(saved.has_value());
   BOOST_CHECK(saved->id == row.id);
   BOOST_CHECK(saved->state == meta::job_state::seeding);
   BOOST_CHECK_EQUAL(saved->priority, 11);
   BOOST_REQUIRE(saved->checkpoint.has_value());
   BOOST_CHECK(saved->checkpoint->published);
   BOOST_CHECK(read_file(files.root / "result") == bytes);
   for (const auto& item : std::filesystem::directory_iterator(files.root)) {
      BOOST_CHECK(item.path().extension() != ".resume");
   }
   config.mode = storage::open_mode::resume;
   store = co_await storage::file_store::async_open(config);
   BOOST_CHECK((co_await store->async_read({0, 1}, 0, 17)) ==
      std::vector<std::uint8_t>(bytes.begin() + manifest.piece_size, bytes.begin() + manifest.piece_size + 17));
   co_await store->async_close();
   co_await catalog->async_close();
   BOOST_CHECK_THROW(static_cast<void>(adapter->async_load(id, {})), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(adapter->async_save(*saved->checkpoint, {})), core::exceptions::storage_closed);
   BOOST_CHECK(access->flushes.load() > 0U);
   BOOST_CHECK((co_await objects.get(row.id)).id == row.id);
   co_await driver->async_flush(true);
}

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_catalog_tests)

BOOST_AUTO_TEST_CASE(metadata_v1_has_frozen_raw_layout_without_a_second_codec) {
   const auto unhex = [](const std::string& hex) {
      auto bytes = std::vector<std::uint8_t>{};
      for (auto i = std::size_t{}; i < hex.size(); i += 2) {
         bytes.push_back(static_cast<std::uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
      }
      return bytes;
   };
   const auto bytes = std::array<std::uint8_t, 1>{'a'};
   auto row = meta::content{};
   row.id = meta::content::id_t{3};
   row.manifest = {core::descriptor_version::v1, core::block_size,
      {{{"a"}, 1U, core::hash_file(bytes, core::block_size).root, {}}}};
   row.swarm = core::calculate_swarm_id(row.manifest);
   row.locator = "file";
   row.state = meta::job_state::failed;
   row.priority = -2;
   row.expires_at = 42;
   const auto fixture = unhex(
      "0300000000000000"
      "fce83635f30c1c132dc75136561c023f62db154a65bfc68e655668edd64069ab"
      "0100000000400000010101610100000000000000"
      "ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb00"
      "0466696c65"
      "0300000000000000"
      "feffffff"
      "2a00000000000000"
      "00");
   BOOST_CHECK(forge::raw::pack(row) == fixture);
   BOOST_CHECK(forge::raw::unpack<meta::content>(fixture) == row);
   BOOST_CHECK(forge::raw::pack(meta::schema_state{}) == unhex("000000000000000001000000"));
}

BOOST_AUTO_TEST_CASE(identity_locator_uniqueness_and_explicit_schema_registration) {
   auto env = catalog_fixture{};
   const auto first = env.create();
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(), "other", {})), storage::exceptions::conflict);
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(1), first.locator, {})), storage::exceptions::conflict);
   BOOST_CHECK(run(env.catalog->async_find(first.swarm)).has_value());
   auto unregistered = run(dbo::store::open(env.driver, {.family = dbc::family{"other"}}));
   BOOST_CHECK_THROW(run(storage::catalog::async_open(unregistered, {env.files.cpu.get_executor(), env.database.get_executor()})), dbo::exceptions::unregistered_object);
}

BOOST_AUTO_TEST_CASE(correct_schema_cannot_hide_missing_content_model_in_either_open_overload) {
   for (const auto guarded_access : {false, true}) {
      BOOST_TEST_CONTEXT("guarded_access=" << guarded_access) {
         auto files = fixture{};
         auto database = boost::asio::thread_pool{1};
         auto driver = std::make_shared<memory_driver>();
         auto objects = run(dbo::store::open(driver));
         objects.register_object<meta::schema_index>();
         run(objects.insert(meta::schema_state{}));
         BOOST_CHECK_EQUAL(run(objects.get(meta::schema_id)).version, meta::schema_version);
         {
            auto tx = run(objects.begin_transaction());
            // No query is awaited: constructing the view must validate registration.
            BOOST_CHECK_THROW(static_cast<void>((tx.index<meta::content_index, meta::by_id>())),
               dbo::exceptions::unregistered_object);
            run(tx.rollback());
         }
         const auto options = storage::catalog::options{files.cpu.get_executor(), database.get_executor()};
         if (guarded_access) {
            auto access = std::make_shared<catalog_access_probe>(objects, driver);
            BOOST_CHECK_THROW(run(storage::catalog::async_open(access, options)), dbo::exceptions::unregistered_object);
         } else {
            BOOST_CHECK_THROW(run(storage::catalog::async_open(objects, options)), dbo::exceptions::unregistered_object);
         }
         run(driver->async_close());
      }
   }
}

BOOST_AUTO_TEST_CASE(state_priority_and_expiry_queries_are_bounded_index_pages) {
   auto env = catalog_fixture{};
   for (auto i = 0U; i < 300U; ++i) {
      env.create(i, {.state = i % 2 ? meta::job_state::downloading : meta::job_state::paused,
         .priority = static_cast<std::int32_t>(i % 9), .expires_at = i % 3 ? INT64_MAX : 50});
   }
   auto state = env.driver->state;
   state->gets = state->scans = state->scanned = 0;
   auto page = run(env.catalog->async_by_state(meta::job_state::downloading, {.limit = 5}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 5U);
   BOOST_REQUIRE(page.next.has_value());
   BOOST_CHECK(state->scanned <= 6U); // One bounded schema singleton plus five matching rows.
   BOOST_CHECK(state->gets <= 12U);
   for (const auto& item : page.items) { BOOST_CHECK(item.state == meta::job_state::downloading); }
   BOOST_CHECK(std::is_sorted(page.items.begin(), page.items.end(), [](const auto& a, const auto& b) {
      return a.priority > b.priority || (a.priority == b.priority && a.swarm.value < b.swarm.value);
   }));
   const auto first = page.items.front();
   const auto second = run(env.catalog->async_by_state(meta::job_state::downloading, {.after = page.next, .limit = 5}));
   BOOST_REQUIRE_EQUAL(second.items.size(), 5U);
   for (const auto& item : second.items) { BOOST_CHECK(item.id != first.id); }
   auto expired = run(env.catalog->async_expiring_before(51, {.limit = 7}));
   BOOST_REQUIRE_EQUAL(expired.items.size(), 7U);
   for (const auto& item : expired.items) { BOOST_CHECK_EQUAL(item.expires_at, 50); }
   BOOST_CHECK(run(env.catalog->async_expiring_before(50)).items.empty());
   BOOST_CHECK_THROW(run(env.catalog->async_by_state(meta::job_state::paused, {.limit = 33})), storage::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(checkpoint_generations_are_monotonic_and_equal_retry_is_exact) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   BOOST_CHECK(!run(adapter->async_load(row.swarm, {})));
   auto checkpoint = complete(row.manifest, 7);
   run(adapter->async_save(checkpoint, {}));
   const auto before = env.driver->state->flushes;
   run(adapter->async_save(checkpoint, {}));
   BOOST_CHECK(env.driver->state->flushes > before);
   auto stale = checkpoint;
   stale.generation = 6;
   BOOST_CHECK_THROW(run(adapter->async_save(stale, {})), storage::exceptions::stale_checkpoint);
   stale = checkpoint;
   stale.verified = {0};
   BOOST_CHECK_THROW(run(adapter->async_save(stale, {})), storage::exceptions::stale_checkpoint);
   stale = checkpoint;
   stale.generation = UINT64_MAX;
   BOOST_CHECK_THROW(run(adapter->async_save(stale, {})), core::exceptions::invalid_checkpoint);
   BOOST_CHECK(run(adapter->async_load(row.swarm, {})) == checkpoint);
}

BOOST_AUTO_TEST_CASE(erased_and_recreated_content_cannot_be_revived_by_an_old_binding) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   run(env.catalog->async_erase(row.swarm));
   const auto replacement = env.create();
   BOOST_CHECK(row.id != replacement.id);
   BOOST_CHECK_THROW(run(adapter->async_save(complete(row.manifest, 1), {})), storage::exceptions::missing_metadata);
   BOOST_CHECK_THROW(run(adapter->async_load(row.swarm, {})), storage::exceptions::missing_metadata);
   BOOST_CHECK(!run(env.catalog->async_find(row.swarm))->checkpoint);
}

BOOST_AUTO_TEST_CASE(commit_and_write_failures_rollback_rows_and_indexes) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   for (bool commit : {false, true}) {
      if (commit) { env.driver->state->fail_commit = true; }
      else { env.driver->state->fail_put_after = 1U; }
      BOOST_CHECK_THROW(run(env.catalog->async_update(row.swarm,
         {.state = meta::job_state::seeding, .priority = 19, .expires_at = 10})), std::runtime_error);
      env.driver->state->fail_commit = false;
      env.driver->state->fail_put_after.reset();
      const auto read = run(env.catalog->async_find(row.swarm));
      BOOST_REQUIRE(read.has_value());
      BOOST_CHECK(read->state == meta::job_state::paused);
      BOOST_CHECK_EQUAL(read->priority, 0);
      BOOST_CHECK(run(env.catalog->async_expiring_before(11)).items.empty());
      BOOST_CHECK(run(env.catalog->async_by_state(meta::job_state::seeding)).items.empty());
   }
}

BOOST_AUTO_TEST_CASE(flush_failure_is_not_an_acknowledgement_and_load_restores_the_fence) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   auto checkpoint = complete(row.manifest, 2);
   env.driver->state->fail_flush = true;
   BOOST_CHECK_THROW(run(adapter->async_save(checkpoint, {})), std::runtime_error);
   BOOST_CHECK_THROW(run(adapter->async_load(row.swarm, {})), std::runtime_error);
   BOOST_CHECK(run(env.catalog->async_find(row.swarm))->checkpoint == checkpoint);
   auto older = checkpoint;
   older.generation = 1;
   BOOST_CHECK_THROW(run(adapter->async_save(older, {})), storage::exceptions::stale_checkpoint);
   env.driver->state->fail_flush = false;
   BOOST_CHECK(run(adapter->async_load(row.swarm, {})) == checkpoint);
   BOOST_CHECK(env.driver->state->live == env.driver->state->durable);
}

BOOST_AUTO_TEST_CASE(access_flush_is_the_checkpoint_fence_and_guard_failures_propagate) {
   auto env = catalog_fixture{};
   auto access = std::make_shared<catalog_access_probe>(env.objects, env.driver);
   env.catalog = run(storage::catalog::async_open(access,
      {env.files.cpu.get_executor(), env.database.get_executor()}));
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   const auto checkpoint = complete(row.manifest, 2);
   const auto before = access->flushes.load();
   access->fail_flush = true;
   BOOST_CHECK_THROW(run(adapter->async_save(checkpoint, {})), std::runtime_error);
   BOOST_CHECK_THROW(run(adapter->async_load(row.swarm, {})), std::runtime_error);
   BOOST_CHECK_EQUAL(access->flushes.load(), before + 2U);
   BOOST_CHECK(run(env.catalog->async_find(row.swarm))->checkpoint == checkpoint);
   access->fail_flush = false;
   BOOST_CHECK(run(adapter->async_load(row.swarm, {})) == checkpoint);
   BOOST_CHECK(env.driver->state->live == env.driver->state->durable);
   access->available = false;
   BOOST_CHECK_THROW(run(env.catalog->async_find(row.swarm)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(run(adapter->async_save(checkpoint, {})), core::exceptions::storage_closed);
   access->available = true;
   run(env.catalog->async_close());
}

BOOST_AUTO_TEST_CASE(explicit_close_rejects_all_admission_releases_access_and_keeps_db_open) {
   auto env = catalog_fixture{};
   auto access = std::make_shared<catalog_access_probe>(env.objects, env.driver);
   auto weak = std::weak_ptr<catalog_access_probe>{access};
   env.catalog = run(storage::catalog::async_open(access,
      {env.files.cpu.get_executor(), env.database.get_executor()}));
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   access.reset();
   auto closing = env.catalog->async_close();
   // Admission closes when the awaitable is constructed, not at initial resume.
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_find(row.swarm)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_create(env.manifest(1), "later", {})), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_update(row.swarm, {})), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_erase(row.swarm)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_by_state(meta::job_state::paused)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_expiring_before(100)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_checkpoint_store(row.swarm, row.locator)), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(adapter->async_load(row.swarm, {})), core::exceptions::storage_closed);
   BOOST_CHECK_THROW(static_cast<void>(adapter->async_save(complete(row.manifest, 1), {})), core::exceptions::storage_closed);
   run(std::move(closing));
   BOOST_CHECK(weak.expired());
   run(env.catalog->async_close());
   BOOST_CHECK(run(env.objects.get(row.id)).swarm == row.swarm);
   run(env.driver->async_flush(true));
   auto reopened = run(storage::catalog::async_open(env.objects,
      {env.files.cpu.get_executor(), env.database.get_executor()}));
   BOOST_CHECK(run(reopened->async_find(row.swarm)).has_value());
   run(reopened->async_close());
}

BOOST_AUTO_TEST_CASE(close_joins_accepted_read_and_concurrent_closers) {
   using namespace std::chrono_literals;
   auto env = catalog_fixture{};
   auto access = std::make_shared<catalog_access_probe>(env.objects, env.driver);
   env.catalog = run(storage::catalog::async_open(access,
      {env.files.cpu.get_executor(), env.database.get_executor()}));
   const auto row = env.create();
   access->hold_reads = true;
   auto io = boost::asio::io_context{};
   auto work = boost::asio::make_work_guard(io);
   const auto epoch = access->entered.epoch();
   auto entered = boost::asio::co_spawn(io,
      access->entered.async_wait_until(epoch, std::chrono::steady_clock::now() + 3s), boost::asio::use_future);
   auto reading = boost::asio::co_spawn(io, env.catalog->async_find(row.swarm), boost::asio::use_future);
   auto thread = std::jthread{[&] { io.run(); }};
   const auto reached = entered.wait_for(4s) == std::future_status::ready && entered.get() != epoch;
   auto first = boost::asio::co_spawn(io, env.catalog->async_close(), boost::asio::use_future);
   auto second = boost::asio::co_spawn(io, env.catalog->async_close(), boost::asio::use_future);
   const auto first_pending = first.wait_for(20ms) == std::future_status::timeout;
   const auto second_pending = second.wait_for(20ms) == std::future_status::timeout;
   access->released.notify();
   work.reset();
   thread.join();
   BOOST_REQUIRE_MESSAGE(reached, "accepted read never entered guarded access");
   BOOST_CHECK(first_pending);
   BOOST_CHECK(second_pending);
   BOOST_CHECK(reading.get().has_value());
   BOOST_CHECK_NO_THROW(first.get());
   BOOST_CHECK_NO_THROW(second.get());
   BOOST_CHECK(run(env.objects.get(row.id)).swarm == row.swarm);
}

BOOST_AUTO_TEST_CASE(close_counts_unstarted_work_and_survives_facade_destruction) {
   using namespace std::chrono_literals;
   auto env = catalog_fixture{};
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   auto accepted = std::optional{env.catalog->async_find(row.swarm)};
   auto closing = env.catalog->async_close();
   env.catalog.reset();
   auto io = boost::asio::io_context{};
   auto finished = boost::asio::co_spawn(io, std::move(closing), boost::asio::use_future);
   auto thread = std::jthread{[&] { io.run(); }};
   const auto pending = finished.wait_for(20ms) == std::future_status::timeout;
   accepted.reset();
   thread.join();
   BOOST_CHECK(pending);
   BOOST_CHECK_NO_THROW(finished.get());
   BOOST_CHECK_THROW(static_cast<void>(adapter->async_load(row.swarm, {})), core::exceptions::storage_closed);
   BOOST_CHECK(run(env.objects.get(row.id)).swarm == row.swarm);
}

BOOST_AUTO_TEST_CASE(close_preserves_caller_filters_pending_cancellation_and_next_timer) {
   for (const auto precancel : {false, true}) {
      auto env = catalog_fixture{};
      auto signal = boost::asio::cancellation_signal{};
      auto io = boost::asio::io_context{};
      auto result = boost::asio::co_spawn(io, close_preserving_caller_cancellation(env.catalog, signal, precancel),
         boost::asio::bind_cancellation_slot(signal.slot(), boost::asio::use_future));
      BOOST_CHECK(drain_with_diagnostics(io, result, [] {}, std::source_location::current()));
   }
}

BOOST_AUTO_TEST_CASE(schema_mismatch_and_unversioned_populated_catalog_fail_closed) {
   for (const bool header : {false, true}) {
      auto files = fixture{};
      auto database = boost::asio::thread_pool{1};
      auto driver = std::make_shared<memory_driver>();
      auto objects = run(dbo::store::open(driver));
      storage::catalog::register_objects(objects);
      if (header) {
         auto value = meta::schema_state{};
         value.version = 999;
         run(objects.insert(value));
      } else {
         auto value = meta::content{};
         value.manifest = describe({data(17)});
         value.swarm = core::calculate_swarm_id(value.manifest);
         value.locator = "unversioned";
         run(objects.insert(value));
      }
      BOOST_CHECK_THROW(run(storage::catalog::async_open(objects, {files.cpu.get_executor(), database.get_executor()})), storage::exceptions::schema_mismatch);
      run(driver->async_close());
   }
}

BOOST_AUTO_TEST_CASE(invalid_metadata_and_pre_cancel_do_not_mutate_the_catalog) {
   auto env = catalog_fixture{};
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(), "", {})), storage::exceptions::invalid_metadata);
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(), "locator", {.state = static_cast<meta::job_state>(99)})), storage::exceptions::invalid_metadata);
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(), "locator", {.expires_at = -1})), storage::exceptions::invalid_metadata);
   auto stop = std::stop_source{};
   stop.request_stop();
   BOOST_CHECK_THROW(run(env.catalog->async_create(env.manifest(), "locator", {}, stop.get_token())), std::exception);
   BOOST_CHECK(run(env.catalog->async_by_state(meta::job_state::paused)).items.empty());
}

BOOST_AUTO_TEST_CASE(after_commit_observer_failure_is_not_misreported_as_rollback) {
   class observer final : public dbo::observer {
    public:
      bool fail = false;
      boost::asio::awaitable<void> after_commit(const dbo::change_set&) override {
         if (fail) { throw std::runtime_error{"injected after-commit failure"}; }
         co_return;
      }
   };
   auto env = catalog_fixture{};
   auto hook = std::make_shared<observer>();
   env.objects.add_observer(hook);
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   const auto checkpoint = complete(row.manifest, 1);
   hook->fail = true;
   BOOST_CHECK_THROW(run(adapter->async_save(checkpoint, {})), std::runtime_error);
   hook->fail = false;
   BOOST_CHECK(run(env.catalog->async_find(row.swarm))->checkpoint == checkpoint);
   run(adapter->async_save(checkpoint, {}));
   BOOST_CHECK(run(adapter->async_load(row.swarm, {})) == checkpoint);
}

BOOST_AUTO_TEST_CASE(rollback_failure_does_not_keep_the_writer_lane_or_partial_indexes) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   env.driver->state->fail_put_after = 1;
   env.driver->state->fail_rollback = true;
   BOOST_CHECK_THROW(run(env.catalog->async_update(row.swarm, {.priority = 99})), std::exception);
   env.driver->state->fail_rollback = false;
   env.driver->state->fail_put_after.reset();
   BOOST_CHECK_EQUAL(run(env.catalog->async_find(row.swarm))->priority, 0);
   run(env.catalog->async_update(row.swarm, {.priority = 5}));
   BOOST_CHECK_EQUAL(run(env.catalog->async_find(row.swarm))->priority, 5);
}

BOOST_AUTO_TEST_CASE(persisted_metadata_is_validated_instead_of_trusted_as_file_availability) {
   auto env = catalog_fixture{};
   const auto row = env.create();
   run(env.objects.modify(row.id, [](meta::content& value) { value.manifest.files[0].path = {"changed"}; }));
   BOOST_CHECK_THROW(run(env.catalog->async_find(row.swarm)), storage::exceptions::invalid_metadata);
   BOOST_CHECK_THROW(run(env.catalog->async_checkpoint_store(row.swarm, row.locator)), storage::exceptions::invalid_metadata);
}

BOOST_AUTO_TEST_CASE(outstanding_admission_is_bounded_and_commit_flush_is_joined_on_cancellation) {
   using namespace std::chrono_literals;
   auto env = catalog_fixture{};
   env.catalog = run(storage::catalog::async_open(env.objects,
      {env.files.cpu.get_executor(), env.database.get_executor(), 1}));
   const auto row = env.create();
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   auto entered = std::promise<void>{};
   auto entered_future = entered.get_future();
   auto release = std::promise<void>{};
   auto release_future = release.get_future().share();
   env.driver->state->before_commit = [&] {
      entered.set_value();
      if (release_future.wait_for(5s) != std::future_status::ready) { throw std::runtime_error{"commit barrier timed out"}; }
   };
   auto io = boost::asio::io_context{};
   auto cancel = boost::asio::cancellation_signal{};
   auto stop = std::stop_source{};
   auto saved = boost::asio::co_spawn(io, adapter->async_save(complete(row.manifest, 1), stop.get_token()),
      boost::asio::bind_cancellation_slot(cancel.slot(), boost::asio::use_future));
   auto thread = std::jthread{[&] { io.run(); }};
   const auto reached = entered_future.wait_for(5s) == std::future_status::ready;
   if (!reached) {
      release.set_value();
      thread.join();
      BOOST_FAIL("commit barrier not reached");
   }
   auto rejected = boost::asio::co_spawn(io, find_deferred(env.catalog, row.swarm), boost::asio::use_future);
   const auto bounded = rejected.wait_for(2s) == std::future_status::ready;
   stop.request_stop();
   boost::asio::post(io, [&] { cancel.emit(boost::asio::cancellation_type::all); });
   BOOST_CHECK(saved.wait_for(20ms) == std::future_status::timeout);
   release.set_value();
   thread.join();
   BOOST_REQUIRE_MESSAGE(bounded, "admission did not reject before dispatching to a blocked DB executor");
   BOOST_CHECK_THROW(static_cast<void>(rejected.get()), core::exceptions::storage_busy);
   BOOST_CHECK_THROW(saved.get(), std::exception);
   env.driver->state->before_commit = {};
   const auto stored = run(env.catalog->async_find(row.swarm));
   BOOST_REQUIRE(stored->checkpoint.has_value());
   BOOST_CHECK_EQUAL(stored->checkpoint->generation, 1U);
   BOOST_CHECK(env.driver->state->live == env.driver->state->durable);
}

BOOST_AUTO_TEST_CASE(memory_file_store_checkpoint_parity) {
   auto files = fixture{};
   auto driver = std::make_shared<memory_driver>();
   run(parity(driver, files));
   run(driver->async_close());
}

BOOST_AUTO_TEST_CASE(unstarted_operations_release_admission_and_handles_outlive_the_facade) {
   auto env = catalog_fixture{};
   env.catalog = run(storage::catalog::async_open(env.objects,
      {env.files.cpu.get_executor(), env.database.get_executor(), 1}));
   const auto row = env.create();
   {
      auto unstarted = env.catalog->async_find(row.swarm);
      BOOST_CHECK_THROW(static_cast<void>(env.catalog->async_find(row.swarm)), core::exceptions::storage_busy);
   }
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   auto unstarted = adapter->async_save(complete(row.manifest, 1), {});
   adapter.reset();
   env.catalog.reset();
   run(std::move(unstarted));
   auto reopened = run(storage::catalog::async_open(env.objects,
      {env.files.cpu.get_executor(), env.database.get_executor()}));
   const auto read = run(reopened->async_find(row.swarm));
   BOOST_REQUIRE(read->checkpoint.has_value());
   BOOST_CHECK_EQUAL(read->checkpoint->generation, 1U);
}

BOOST_AUTO_TEST_CASE(a_complete_db_row_cannot_make_missing_or_corrupted_payload_available) {
   auto env = catalog_fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   const auto path = (env.files.root / "result").string();
   const auto row = run(env.catalog->async_create(manifest, path, {}));
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, path));
   auto config = env.files.options(manifest);
   config.checkpoint = adapter;
   auto store = run(storage::file_store::async_open(config));
   fill(*store, manifest, {bytes});
   run(store->async_publish());
   run(store->async_close());
   store.reset();
   const auto saved = run(adapter->async_load(row.swarm, {}));
   BOOST_REQUIRE(saved->published);
   config.mode = storage::open_mode::resume;
   store = run(storage::file_store::async_open(config));
   auto corrupt = bytes;
   corrupt[0] ^= 1U;
   write_file(path, corrupt);
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::integrity_failure);
   BOOST_CHECK_THROW(run(store->async_close()), core::exceptions::invalid_checkpoint);
   store.reset();
   BOOST_CHECK(run(adapter->async_load(row.swarm, {})) == saved);
   BOOST_CHECK_THROW(run(storage::file_store::async_open(config)), core::exceptions::integrity_failure);
   BOOST_REQUIRE(std::filesystem::remove(path));
   BOOST_CHECK_THROW(run(storage::file_store::async_open(config)), storage::exceptions::missing_data);
}

BOOST_AUTO_TEST_CASE(sidecar_and_database_checkpoint_authorities_are_not_silently_combined) {
   auto env = catalog_fixture{};
   const auto manifest = describe({data(17)});
   auto config = env.files.options(manifest);
   auto store = run(storage::file_store::async_open(config));
   run(store->async_close());
   store.reset();
   const auto row = run(env.catalog->async_create(manifest, config.destination.string(), {}));
   config.checkpoint = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   config.mode = storage::open_mode::resume;
   BOOST_CHECK_THROW(run(storage::file_store::async_open(config)), storage::exceptions::conflict);
}

BOOST_AUTO_TEST_CASE(recovery_reconciles_renamed_payload_with_the_last_durable_staged_db_checkpoint) {
   auto env = catalog_fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   const auto row = run(env.catalog->async_create(manifest, (env.files.root / "result").string(), {}));
   auto adapter = run(env.catalog->async_checkpoint_store(row.swarm, row.locator));
   auto config = env.files.options(manifest);
   config.checkpoint = adapter;
   auto store = run(storage::file_store::async_open(config));
   fill(*store, manifest, {bytes});
   auto calls = 0U;
   env.driver->state->before_flush = [&] {
      if (++calls == 2U) { throw std::runtime_error{"lost published-metadata durability fence"}; }
   };
   BOOST_CHECK_THROW(run(store->async_publish()), std::runtime_error);
   BOOST_CHECK_EQUAL(calls, 2U);
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(read_file(row.locator) == bytes);
   store.reset();
   env.driver->state->before_flush = {};
   // A deterministic DB fault model, not a claim of hardware power-loss simulation.
   { const auto lock = std::scoped_lock{env.driver->state->mutex}; env.driver->state->live = env.driver->state->durable; }
   const auto checkpoint = run(adapter->async_load(row.swarm, {}));
   BOOST_REQUIRE(checkpoint.has_value());
   BOOST_CHECK(!checkpoint->published);
   config.mode = storage::open_mode::resume;
   store = run(storage::file_store::async_open(config));
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(run(store->async_read({0, 0}, 0, 17)) == bytes);
   run(store->async_close());
   BOOST_CHECK(run(adapter->async_load(row.swarm, {}))->published);
}

#if defined(FORGE_TEST_HAS_MDBX)
BOOST_AUTO_TEST_CASE(mdbx_four_kib_pages_support_bounded_long_storage_locators) {
   auto files = fixture{};
   auto database = boost::asio::thread_pool{1};
   auto driver = run(forge::db::mdbx::driver::open(
      {.path = (files.root / "long-locator").string(), .families = {"objectdb"}, .map = {.page_size = 4096}},
      forge::asio::affine::lane::options{.thread_name = "swarm-locator"}));
   auto objects = run(dbo::store::open(driver));
   storage::catalog::register_objects(objects);
   auto catalog = run(storage::catalog::async_open(objects, {files.cpu.get_executor(), database.get_executor()}));
   const auto manifest = describe({data(17)});
   const auto locator = std::string(4096, 'a');
   const auto row = run(catalog->async_create(manifest, locator, {}));
   BOOST_CHECK_EQUAL(row.locator, locator);
   BOOST_CHECK_THROW(run(catalog->async_create(manifest, locator, {})), storage::exceptions::conflict);
   run(driver->async_close());
}

BOOST_AUTO_TEST_CASE(mdbx_file_store_checkpoint_parity_and_native_reopen) {
   auto files = fixture{};
   const auto path = files.root / "mdbx";
   auto driver = run(forge::db::mdbx::driver::open({.path = path.string(), .families = {"objectdb"}},
      forge::asio::affine::lane::options{.thread_name = "swarm-pr3-mdbx"}));
   run(parity(driver, files));
   run(driver->async_close());
   driver.reset();
   driver = run(forge::db::mdbx::driver::open({.path = path.string(), .families = {"objectdb"}},
      forge::asio::affine::lane::options{.thread_name = "swarm-pr3-mdbx"}));
   auto objects = run(dbo::store::open(driver));
   auto database = boost::asio::thread_pool{1};
   storage::catalog::register_objects(objects);
   auto catalog = run(storage::catalog::async_open(objects, {files.cpu.get_executor(), database.get_executor()}));
   const auto page = run(catalog->async_by_state(meta::job_state::seeding));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_REQUIRE(page.items[0].checkpoint.has_value());
   BOOST_CHECK(page.items[0].checkpoint->published);
   run(driver->async_close());
}
#endif

#if defined(FORGE_TEST_HAS_ROCKSDB)
BOOST_AUTO_TEST_CASE(rocksdb_file_store_checkpoint_parity_and_native_reopen) {
   auto files = fixture{};
   const auto config = forge::db::rocksdb::config{.path = (files.root / "rocksdb").string(), .families = {"objectdb"}};
   auto driver = std::make_shared<forge::db::rocksdb::driver>(config);
   run(parity(driver, files));
   run(driver->async_close());
   driver.reset();
   driver = std::make_shared<forge::db::rocksdb::driver>(config);
   auto objects = run(dbo::store::open(driver));
   auto database = boost::asio::thread_pool{1};
   storage::catalog::register_objects(objects);
   auto catalog = run(storage::catalog::async_open(objects, {files.cpu.get_executor(), database.get_executor()}));
   const auto page = run(catalog->async_by_state(meta::job_state::seeding));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_REQUIRE(page.items[0].checkpoint.has_value());
   BOOST_CHECK(page.items[0].checkpoint->published);
   run(driver->async_close());
}
#endif

BOOST_AUTO_TEST_SUITE_END()
