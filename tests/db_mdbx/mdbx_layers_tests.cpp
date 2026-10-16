#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/db/object/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

import forge.asio.affine;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.db.blob.ref;
import forge.db.blob.store;
import forge.db.blob.types;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.mdbx.driver;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.store;
import forge.db.revision.store;
import forge.db.revision.types;

namespace mdbx_layer_tests {

struct by_id;
struct by_name;
struct by_state;
struct by_bytes;
struct file_by_id;

struct account : forge::db::object::object<account, 71, 1> {
   std::string name;

   bool operator==(const account&) const = default;
};

BOOST_DESCRIBE_STRUCT(account,
                      (forge::db::object::object<account, 71, 1>),
                      (name))

using account_index = forge::db::object::object_index<
   account,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<
         by_name, forge::db::object::member<&account::name>>>>;

struct usage : forge::db::object::object<usage, 71, 2> {
   std::uint32_t state = 0;
   std::uint64_t bytes = 0;

   bool operator==(const usage&) const = default;
};

BOOST_DESCRIBE_STRUCT(usage,
                      (forge::db::object::object<usage, 71, 2>),
                      (state, bytes))

using usage_index = forge::db::object::object_index<
   usage,
   forge::db::object::indexed_by<
      forge::db::object::ranked_primary_unique<
         by_id,
         forge::db::object::ranked_schema<1>,
         forge::db::object::sum<
            by_bytes, forge::db::object::member<&usage::bytes>>>,
      forge::db::object::ranked_non_unique<
         by_state,
         forge::db::object::member<&usage::state>,
         forge::db::object::ranked_schema<1>,
         forge::db::object::sum<
            by_bytes, forge::db::object::member<&usage::bytes>>>>>;

struct file : forge::db::object::object<file, 71, 3> {
   forge::db::blob::ref<> content;

   bool operator==(const file&) const = default;
};

BOOST_DESCRIBE_STRUCT(file,
                      (forge::db::object::object<file, 71, 3>),
                      (content))

using file_index = forge::db::object::object_index<
   file,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<file_by_id>>>;

} // namespace mdbx_layer_tests

FORGE_DB_OBJECT(mdbx_layer_tests::account_index)
FORGE_DB_OBJECT(mdbx_layer_tests::usage_index)
FORGE_DB_OBJECT(mdbx_layer_tests::file_index)

namespace {

std::filesystem::path make_layer_root(std::string name) {
   static auto sequence = std::atomic_uint64_t{0};
   const auto nonce = sequence.fetch_add(1, std::memory_order_relaxed);
   auto root = std::filesystem::temp_directory_path() /
               (std::move(name) + "_" + std::to_string(nonce));
   std::filesystem::remove_all(root);
   return root;
}

std::vector<std::byte> layer_bytes(std::string value) {
   return {
      reinterpret_cast<const std::byte*>(value.data()),
      reinterpret_cast<const std::byte*>(value.data() + value.size()),
   };
}

forge::db::core::record_key layer_key(std::string value) {
   return forge::db::core::record_key{layer_bytes(std::move(value))};
}

forge::db::mdbx::config layer_config(const std::filesystem::path& path) {
   return forge::db::mdbx::config{
      .path = path.string(),
      .families = {"objectdb", "blobdb.data", "blobdb.refs", "records"},
   };
}

boost::asio::awaitable<std::shared_ptr<forge::db::mdbx::driver>>
open_layer_driver(const std::filesystem::path& path,
                  const forge::asio::affine::lane& lane) {
   co_return co_await forge::db::mdbx::driver::open(
      layer_config(path), lane.get_executor());
}

} // namespace

BOOST_AUTO_TEST_SUITE(db_mdbx_layer_test_suite)

BOOST_AUTO_TEST_CASE(db_mdbx_object_ranked_state_persists_and_reopens) {
   const auto root = make_layer_root("forge_db_mdbx_object");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_layer_driver(root / "store", lane);
      {
         auto objects = co_await forge::db::object::store::open(
            driver,
            forge::db::object::store::options{
               .writes = forge::db::object::write_policy::backend});
         objects.register_object<mdbx_layer_tests::account_index>();
         objects.register_object<mdbx_layer_tests::usage_index>();

         auto transaction = co_await objects.begin_transaction();
         auto alice = mdbx_layer_tests::account{};
         alice.id = mdbx_layer_tests::account::id_t{1};
         alice.name = "alice";
         co_await transaction.insert(alice);

         auto first = mdbx_layer_tests::usage{};
         first.id = mdbx_layer_tests::usage::id_t{1};
         first.state = 3;
         first.bytes = 4096;
         co_await transaction.insert(first);
         const auto point = co_await transaction.db_transaction().create_savepoint();
         auto discarded = mdbx_layer_tests::usage{};
         discarded.id = mdbx_layer_tests::usage::id_t{2};
         discarded.state = 3;
         discarded.bytes = 2048;
         co_await transaction.insert(discarded);
         co_await transaction.db_transaction().rollback_to_savepoint(point);
         co_await transaction.commit();

         auto ranked = objects.index<mdbx_layer_tests::usage_index,
                                     mdbx_layer_tests::by_state>();
         BOOST_CHECK_EQUAL(co_await ranked.equal_range(3U).count(), 1U);
         BOOST_CHECK_EQUAL(
            co_await ranked.equal_range(3U).sum<mdbx_layer_tests::by_bytes>(),
            4096U);

         auto snapshot = co_await objects.begin_read();
         auto changed = alice;
         changed.name = "alice-new";
         co_await objects.replace(changed);
         BOOST_CHECK_EQUAL((co_await snapshot.get(alice.id)).name, "alice");
         BOOST_CHECK_EQUAL((co_await objects.get(alice.id)).name, "alice-new");
      }

      co_await driver->async_flush(true);
      co_await driver->async_close();
      driver.reset();

      auto reopened = co_await open_layer_driver(root / "store", lane);
      {
         auto objects = co_await forge::db::object::store::open(
            reopened,
            forge::db::object::store::options{
               .writes = forge::db::object::write_policy::backend});
         objects.register_object<mdbx_layer_tests::account_index>();
         objects.register_object<mdbx_layer_tests::usage_index>();
         BOOST_CHECK_EQUAL(
            (co_await objects.get(mdbx_layer_tests::account::id_t{1})).name,
            "alice-new");
         auto ranked = objects.index<mdbx_layer_tests::usage_index,
                                     mdbx_layer_tests::by_state>();
         BOOST_CHECK_EQUAL(co_await ranked.count(), 1U);
         BOOST_CHECK_EQUAL(co_await ranked.sum<mdbx_layer_tests::by_bytes>(),
                           4096U);
         BOOST_REQUIRE((co_await ranked.nth(0)).has_value());
      }
      co_await reopened->async_close();
      reopened.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_mdbx_shared_snapshot_keeps_object_and_blob_visible) {
   const auto root = make_layer_root("forge_db_mdbx_shared_snapshot");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_layer_driver(root / "store", lane);
      auto objects = co_await forge::db::object::store::open(driver);
      objects.register_object<mdbx_layer_tests::file_index>();
      auto blobs = forge::db::blob::store{driver};
      const auto owner = forge::db::blob::owner_ref{"file:1"};
      const auto payload = layer_bytes("snapshot payload");

      auto seed = co_await driver->begin_transaction();
      auto object_tx = co_await objects.join(seed);
      auto blob_tx = blobs.join(seed);
      const auto content = co_await blob_tx.put(payload);
      co_await blob_tx.retain(content, owner);
      auto record = mdbx_layer_tests::file{};
      record.id = mdbx_layer_tests::file::id_t{1};
      record.content = content;
      co_await object_tx.insert(record);
      co_await seed.commit();

      auto shared = co_await driver->begin_read();
      auto old_objects = objects.join(shared);
      auto old_blobs = blobs.join(shared);

      auto erase = co_await driver->begin_transaction();
      auto erase_objects = co_await objects.join(erase);
      auto erase_blobs = blobs.join(erase);
      co_await erase_objects.erase(record.id);
      co_await erase_blobs.release(content, owner);
      const auto collected = co_await erase_blobs.collect_unreferenced({.limit = 10});
      BOOST_CHECK_EQUAL(collected.removed, 1U);
      co_await erase.commit();

      BOOST_CHECK(!(co_await objects.find(record.id)).has_value());
      BOOST_CHECK(!(co_await blobs.has(content)));
      BOOST_CHECK_EQUAL((co_await old_objects.get(record.id)).content.size,
                        content.size);
      BOOST_CHECK_EQUAL(co_await old_blobs.ref_count(content), 1U);
      BOOST_CHECK(co_await old_blobs.get(content) == payload);

      old_objects = {};
      old_blobs = {};
      shared = {};
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_mdbx_transactional_allocator_trace_revision_revert_and_reopen) {
   const auto root = make_layer_root("forge_db_mdbx_allocator_v4");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_layer_driver(root / "store", lane);
      {
         auto objects = co_await forge::db::object::store::open(
             driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
         objects.register_object<mdbx_layer_tests::account_index>();
         auto revisions = co_await forge::db::revision::store::open(driver, objects);
         {
            auto active = co_await driver->begin_transaction();
            auto tx = co_await objects.join(active);
            const auto revision = co_await revisions.join(tx);
            const auto first = co_await tx.create<mdbx_layer_tests::account>([](auto& row) { row.name = "first"; });
            BOOST_CHECK_EQUAL(first.id.instance, 0U);
            BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
            BOOST_CHECK_EQUAL(revision.id(), 1U);
            co_await active.commit();
         }
         forge::db::revision::revision_id_t changed = 0;
         {
            auto active = co_await driver->begin_transaction();
            auto tx = co_await objects.join(active);
            const auto revision = co_await revisions.join(tx);
            changed = revision.id();
            co_await tx.erase(mdbx_layer_tests::account::id_t{0});
            const auto next = co_await tx.create<mdbx_layer_tests::account>([](auto& row) { row.name = "next"; });
            BOOST_CHECK_EQUAL(next.id.instance, 1U);
            co_await tx.erase(next.id);
            BOOST_CHECK_EQUAL(co_await tx.allocation_next<mdbx_layer_tests::account_index>(), 2U);
            co_await active.commit();
         }
         {
            auto active = co_await driver->begin_transaction();
            co_await revisions.revert(active, changed);
            co_await active.commit();
         }
         auto view = co_await objects.begin_read();
         BOOST_CHECK_EQUAL(co_await view.allocation_next<mdbx_layer_tests::account_index>(), 1U);
         BOOST_CHECK_EQUAL((co_await view.get(mdbx_layer_tests::account::id_t{0})).name, "first");
      }
      co_await driver->async_flush(true);
      co_await driver->async_close();
      driver.reset();
      auto reopened = co_await open_layer_driver(root / "store", lane);
      {
         auto objects = co_await forge::db::object::store::open(
             reopened, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
         objects.register_object<mdbx_layer_tests::account_index>();
         auto tx = co_await objects.begin_transaction();
         BOOST_CHECK_EQUAL(co_await tx.allocation_next<mdbx_layer_tests::account_index>(), 1U);
         const auto next = co_await tx.create<mdbx_layer_tests::account>([](auto& row) { row.name = "after-reopen"; });
         BOOST_CHECK_EQUAL(next.id.instance, 1U);
         co_await tx.commit();
      }
      co_await reopened->async_close();
      reopened.reset();
      co_await lane.shutdown();
   }());
   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_mdbx_checkpoint_is_a_durable_point_in_time_copy) {
   const auto root = make_layer_root("forge_db_mdbx_checkpoint");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_layer_driver(root / "store", lane);
      {
         auto active = co_await driver->begin_transaction();
         co_await active.put(forge::db::core::family{"records"}, layer_key("state"), layer_bytes("before"));
         co_await active.commit();
      }
      {
         auto view = co_await driver->begin_read();
         const auto stored = co_await view.get(forge::db::core::family{"records"}, layer_key("state"));
         BOOST_REQUIRE(stored.has_value());
         BOOST_CHECK(*stored == layer_bytes("before"));
      }

      const auto checkpoint_path = root / "checkpoints" / "state.mdbx";
      co_await driver->create_checkpoint(checkpoint_path);

      {
         auto active = co_await driver->begin_transaction();
         co_await active.put(forge::db::core::family{"records"}, layer_key("state"), layer_bytes("after"));
         co_await active.commit();
      }

      auto checkpoint_config = layer_config(checkpoint_path);
      checkpoint_config.create_if_missing = false;
      checkpoint_config.create_missing_families = false;
      auto checkpoint = co_await forge::db::mdbx::driver::open(checkpoint_config, lane.get_executor());
      auto view = co_await checkpoint->begin_read();
      const auto stored = co_await view.get(forge::db::core::family{"records"}, layer_key("state"));
      BOOST_REQUIRE(stored.has_value());
      BOOST_CHECK(*stored == layer_bytes("before"));

      view = {};
      co_await checkpoint->async_close();
      checkpoint.reset();
      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_mdbx_revision_revert_prune_and_blob_barrier_are_atomic) {
   const auto root = make_layer_root("forge_db_mdbx_revision");
   auto runtime = forge::asio::runtime{};
   auto lane = forge::asio::affine::lane{};

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto driver = co_await open_layer_driver(root / "store", lane);
      auto objects = co_await forge::db::object::store::open(
         driver,
         forge::db::object::store::options{
            .writes = forge::db::object::write_policy::backend});
      objects.register_object<mdbx_layer_tests::account_index>();
      auto revisions = co_await forge::db::revision::store::open(driver, objects);
      auto blobs = forge::db::blob::store{driver};
      const auto owner = forge::db::blob::owner_ref{"account:1"};
      const auto payload = layer_bytes("retained payload");
      const auto content = co_await blobs.put(payload);
      co_await blobs.retain(content, owner);

      auto active = co_await driver->begin_transaction();
      auto object_tx = co_await objects.join(active);
      const auto revision = co_await revisions.join(object_tx);
      auto blob_tx = blobs.join(active);
      auto account = mdbx_layer_tests::account{};
      account.id = mdbx_layer_tests::account::id_t{1};
      account.name = "revision-created";
      co_await object_tx.insert(account);
      co_await blob_tx.release(content, owner);
      co_await active.commit();
      BOOST_CHECK_EQUAL(revision.id(), 1U);

      BOOST_CHECK_EQUAL(co_await blobs.ref_count(content), 0U);
      BOOST_CHECK_EQUAL(
         (co_await blobs.collect_unreferenced({.limit = 10})).removed, 0U);
      BOOST_CHECK(co_await blobs.has(content));

      auto revert = co_await driver->begin_transaction();
      co_await revisions.revert(revert, revision.id());
      co_await revert.commit();
      BOOST_CHECK(!(co_await objects.find(account.id)).has_value());
      BOOST_CHECK_EQUAL(co_await blobs.ref_count(content), 1U);

      for (const auto* value : {"v1", "v2"}) {
         auto next = co_await revisions.begin_transaction();
         co_await next.db_transaction().put(
            forge::db::core::family{"records"}, layer_key("state"),
            layer_bytes(value));
         co_await next.commit();
      }
      auto prune = co_await driver->begin_transaction();
      const auto result = co_await revisions.prune_through(
         prune, 2U, {.max_revisions = 1U, .max_deltas = 10U});
      BOOST_CHECK_EQUAL(result.revisions_pruned, 1U);
      BOOST_CHECK(result.complete);
      co_await prune.commit();

      co_await driver->async_close();
      driver.reset();
      co_await lane.shutdown();
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_SUITE_END()
