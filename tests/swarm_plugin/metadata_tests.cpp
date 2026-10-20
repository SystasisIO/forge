#include <boost/test/unit_test.hpp>

#if FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB

#include <boost/asio/awaitable.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "metadata_fixture.hxx"
#include "../swarm_storage/support.hpp"

import forge.db.object.transaction;
import forge.db.object.exceptions;
import forge.plugins.db.store.exceptions;
import forge.plugins.swarm.node.exceptions;
import forge.swarm.core.exceptions;
import forge.swarm.db.content;
import forge.swarm.db.schema;
import forge.swarm.storage.exceptions;
import forge.swarm.storage.file_store;

namespace {
namespace fixture = swarm_plugin_metadata_test;
namespace core = forge::swarm::core;
namespace meta = forge::swarm::db;
namespace storage = forge::swarm::storage;

boost::asio::awaitable<void> guarded_update(fixture::db_plugin::object_handle objects, meta::content::id_t id) {
   auto tx = co_await objects.begin_transaction();
   co_await tx.modify(id, [](meta::content& row) { row.priority = 23; });
   co_await tx.commit();
}

void indexed_metadata(fixture::metadata_fixture& env, const core::descriptor& manifest, const std::string& locator,
   meta::content& original) {
   const auto api = env.api();
   original = env.run(api->async_create(manifest, locator,
      {.state = meta::job_state::downloading, .priority = 7, .expires_at = 100}));
   auto other_manifest = manifest;
   other_manifest.files[0].path.back() = "other";
   const auto other = env.run(api->async_create(other_manifest, locator + "-other",
      {.state = meta::job_state::downloading, .priority = 15, .expires_at = 10}));
   auto paused_manifest = manifest;
   paused_manifest.files[0].path.back() = "paused";
   const auto paused = env.run(api->async_create(paused_manifest, locator + "-paused",
      {.state = meta::job_state::paused, .priority = 3, .expires_at = 200}));
   const auto found = env.run(api->async_find(original.swarm));
   BOOST_REQUIRE(found);
   BOOST_CHECK(*found == original);
   auto page = env.run(api->async_by_state(meta::job_state::downloading, {.limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == other.id);
   BOOST_REQUIRE(page.next);
   page = env.run(api->async_by_state(meta::job_state::downloading, {.after = page.next, .limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == original.id);
   BOOST_CHECK(!page.next);
   env.run(api->async_update(original.swarm, {.state = meta::job_state::seeding, .priority = 21, .expires_at = 30}));
   page = env.run(api->async_by_state(meta::job_state::downloading, {.limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == other.id);
   BOOST_CHECK(!page.next);
   page = env.run(api->async_by_state(meta::job_state::seeding, {.limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == original.id);
   BOOST_CHECK_EQUAL(page.items.front().priority, 21);
   BOOST_CHECK_EQUAL(page.items.front().expires_at, 30);
   BOOST_CHECK(env.run(api->async_expiring_before(10, {.limit = 1})).items.empty());
   page = env.run(api->async_expiring_before(31, {.limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == other.id);
   BOOST_REQUIRE(page.next);
   page = env.run(api->async_expiring_before(31, {.after = page.next, .limit = 1}));
   BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
   BOOST_CHECK(page.items.front().id == original.id);
   BOOST_CHECK(!page.next);
   BOOST_CHECK_THROW(env.run(api->async_by_state(meta::job_state::paused, {.limit = 0})), storage::exceptions::invalid_options);
   BOOST_CHECK_THROW(env.run(api->async_expiring_before(300, {.limit = 33})), storage::exceptions::invalid_options);
   env.run(api->async_erase(paused.swarm));
   BOOST_CHECK(!env.run(api->async_find(paused.swarm)));
}

void metadata_restart_acceptance(const std::string& backend) {
   auto env = fixture::metadata_fixture{backend};
   auto files = swarm_test::fixture{};
   const auto bytes = swarm_test::data(3U * core::block_size + 17U);
   // Two entries intentionally select file_store's atomic directory layout.
   const auto manifest = swarm_test::describe({bytes, {}});
   const auto id = core::calculate_swarm_id(manifest);
   const auto locator = (files.root / "result").string();
   env.start();
   BOOST_CHECK(env.shell->has_compute());
   BOOST_REQUIRE(!env.node()->local_endpoints().empty());
   const auto database = env.database();
   const auto status = env.run(database->status());
   BOOST_REQUIRE_EQUAL(status.stores.size(), 2U);
   for (const auto& store : status.stores) {
      BOOST_CHECK_EQUAL(store.driver, backend);
      BOOST_CHECK(store.started);
      BOOST_CHECK(store.object);
   }
   auto original = meta::content{};
   indexed_metadata(env, manifest, locator, original);
   const auto peer_objects = env.run(database->store(fixture::metadata_fixture::peer_name)).objects();
   BOOST_CHECK_EQUAL(peer_objects.name(), fixture::metadata_fixture::peer_name);
   BOOST_CHECK_THROW(env.run(peer_objects.get(original.id)), forge::db::object::exceptions::unregistered_object);
   auto checkpoint = env.run(env.api()->async_checkpoint_store(id, locator));
   auto options = files.options(manifest);
   options.checkpoint = checkpoint;
   auto payload = env.run(storage::file_store::async_open(options));
   {
      auto writer = payload->begin({0, 0});
      env.run(writer->async_write(0, {bytes.begin(), bytes.begin() + core::block_size}));
   }
   const auto partial = env.run(payload->async_checkpoint());
   BOOST_REQUIRE_EQUAL(partial.unfinished.size(), 1U);
   BOOST_CHECK(core::bitmap_test(partial.unfinished.front().blocks, 0));
   BOOST_CHECK(!core::bitmap_test(partial.verified, 0));
   env.run(payload->async_close());
   payload.reset();
   // Close can advance the checkpoint generation, so retain its acknowledged
   // terminal value rather than assuming the earlier snapshot is the last save.
   const auto acknowledged = env.run(checkpoint->async_load(id, {}));
   BOOST_REQUIRE(acknowledged);
   BOOST_CHECK(acknowledged->unfinished == partial.unfinished);
   const auto named = env.run(database->store(fixture::metadata_fixture::metadata_name));
   auto objects = named.objects();
   BOOST_CHECK_EQUAL(objects.name(), fixture::metadata_fixture::metadata_name);
   const auto old_api = env.api();
   env.stop_swarm();
   BOOST_CHECK_THROW((void)checkpoint->async_load(id, {}), core::exceptions::storage_closed);
   BOOST_CHECK_THROW((void)checkpoint->async_save(*acknowledged, {}), core::exceptions::storage_closed);
   BOOST_CHECK_THROW((void)old_api->async_find(id), fixture::sn::exceptions::not_running);
   const auto still_started = env.run(database->status());
   for (const auto& store : still_started.stores) BOOST_CHECK(store.started);
   const auto retained = env.run(objects.get(original.id));
   BOOST_REQUIRE(retained.checkpoint);
   BOOST_CHECK(*retained.checkpoint == *acknowledged);
   // The SAME guarded named handle remains a usable DB capability after Swarm
   // has drained; no raw driver/store escapes through the Swarm production API.
   env.run_database(guarded_update(objects, original.id));
   env.run_database(database->flush(fixture::metadata_fixture::metadata_name, true));
   BOOST_CHECK_EQUAL(env.run(objects.get(original.id)).priority, 23);
   BOOST_CHECK_EQUAL(env.run(env.shell->compute().execute({.name = "metadata-after-swarm-stop"}, [] { return 7; })), 7);
   env.run(env.database_executor_probe());
   env.close();
   BOOST_CHECK_THROW(env.run(objects.get(original.id)), fixture::db_plugin::exceptions::stopped);
   env.reopen();
   env.start();
   const auto restored = env.run(env.api()->async_find(id));
   BOOST_REQUIRE(restored);
   BOOST_CHECK(restored->id == original.id);
   BOOST_CHECK(restored->manifest == manifest);
   BOOST_CHECK_EQUAL(restored->locator, locator);
   BOOST_CHECK(restored->state == meta::job_state::seeding);
   BOOST_CHECK_EQUAL(restored->priority, 23);
   BOOST_CHECK_EQUAL(restored->expires_at, 30);
   BOOST_REQUIRE(restored->checkpoint);
   BOOST_CHECK(*restored->checkpoint == *acknowledged);
   // An escaped old-generation checkpoint never revives when a new owner opens.
   BOOST_CHECK_THROW((void)checkpoint->async_load(id, {}), core::exceptions::storage_closed);
   auto resumed_checkpoint = env.run(env.api()->async_checkpoint_store(id, locator));
   BOOST_CHECK(env.run(resumed_checkpoint->async_load(id, {})) == acknowledged);
   options.mode = storage::open_mode::resume;
   options.checkpoint = resumed_checkpoint;
   payload = env.run(storage::file_store::async_open(options));
   BOOST_REQUIRE_EQUAL(payload->snapshot().unfinished.size(), 1U);
   BOOST_CHECK(!core::bitmap_test(payload->snapshot().verified, 0));
   // Complete the actual partially stored file. Only file_store's verification
   // can promote these blocks; catalog metadata itself is not availability.
   for (auto piece = 0U; piece < core::piece_count(bytes.size(), manifest.piece_size); ++piece) {
      auto writer = payload->begin({0, piece});
      const auto length = core::piece_length(manifest, {0, piece});
      const auto start = std::size_t{piece} * manifest.piece_size;
      for (auto offset = piece == 0U ? core::block_size : 0U; offset < length; offset += core::block_size) {
         const auto end = std::min<std::uint32_t>(offset + core::block_size, length);
         env.run(writer->async_write(offset, {bytes.begin() + start + offset, bytes.begin() + start + end}));
      }
      env.run(writer->async_commit());
   }
   env.run(payload->async_publish());
   env.run(payload->async_close());
   payload.reset();
   const auto completed = env.run(resumed_checkpoint->async_load(id, {}));
   BOOST_REQUIRE(completed);
   BOOST_CHECK(completed->published);
   BOOST_CHECK(completed->unfinished.empty());
   BOOST_CHECK(std::filesystem::is_directory(files.root / "result"));
   BOOST_CHECK(swarm_test::read_file(files.root / "result" / "dir0" / "file") == bytes);
   BOOST_CHECK(std::filesystem::is_regular_file(files.root / "result" / "dir1" / "file"));
   BOOST_CHECK_EQUAL(std::filesystem::file_size(files.root / "result" / "dir1" / "file"), 0U);
   env.reopen();
   env.start();
   const auto durable = env.run(env.api()->async_find(id));
   BOOST_REQUIRE(durable);
   BOOST_REQUIRE(durable->checkpoint);
   BOOST_CHECK(*durable->checkpoint == *completed);
   BOOST_CHECK_THROW((void)resumed_checkpoint->async_load(id, {}), core::exceptions::storage_closed);
}

void unknown_metadata_name_rejected(const std::string& backend) {
   auto env = fixture::metadata_fixture{backend, true, "missing-swarm-store"};
   BOOST_CHECK_THROW(env.start(), fixture::db_plugin::exceptions::unknown_store);
}

void incompatible_metadata_schema_rejected(const std::string& backend) {
   auto env = fixture::metadata_fixture{backend, false};
   env.run(env.shell->initialize());
   const auto named = env.run(env.database()->store(fixture::metadata_fixture::metadata_name));
   auto objects = named.objects();
   objects.register_object<meta::schema_index>();
   env.start();
   auto incompatible = meta::schema_state{};
   incompatible.version = 999U;
   env.run_database(objects.insert(incompatible));
   env.run_database(env.database()->flush(fixture::metadata_fixture::metadata_name, true));
   // Fresh DB owners will register the two catalog models normally, avoiding
   // duplicate registration or a second catalog implementation in the fixture.
   env.reopen();
   BOOST_CHECK_THROW(env.start(), storage::exceptions::schema_mismatch);
}
} // namespace

#if FORGE_HAS_MDBX
BOOST_AUTO_TEST_SUITE(swarm_plugin_metadata_mdbx)
BOOST_AUTO_TEST_CASE(indexed_catalog_checkpoint_reopen_and_owner_order) { metadata_restart_acceptance("mdbx"); }
BOOST_AUTO_TEST_CASE(unknown_named_metadata_store_is_rejected) { unknown_metadata_name_rejected("mdbx"); }
BOOST_AUTO_TEST_CASE(incompatible_persisted_schema_is_rejected) { incompatible_metadata_schema_rejected("mdbx"); }
BOOST_AUTO_TEST_SUITE_END()
#endif

#if FORGE_HAS_ROCKSDB
BOOST_AUTO_TEST_SUITE(swarm_plugin_metadata_rocksdb)
BOOST_AUTO_TEST_CASE(indexed_catalog_checkpoint_reopen_and_owner_order) { metadata_restart_acceptance("rocksdb"); }
BOOST_AUTO_TEST_CASE(unknown_named_metadata_store_is_rejected) { unknown_metadata_name_rejected("rocksdb"); }
BOOST_AUTO_TEST_CASE(incompatible_persisted_schema_is_rejected) { incompatible_metadata_schema_rejected("rocksdb"); }
BOOST_AUTO_TEST_SUITE_END()
#endif

#endif
