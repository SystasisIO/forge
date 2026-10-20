#include <boost/test/unit_test.hpp>
#include "support.hpp"

using namespace swarm_test;

BOOST_AUTO_TEST_SUITE(swarm_files)

BOOST_AUTO_TEST_CASE(single_file_random_writes_readback_and_atomic_publication) {
   auto env = fixture{};
   const auto bytes = data(5U * core::block_size + 7U);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   BOOST_CHECK(!std::filesystem::exists(env.root / "result"));
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::unverified_piece);
   fill(*store, manifest, {bytes});
   const auto range = run(store->async_read({0, 1}, 7, 27));
   BOOST_CHECK(range == std::vector<std::uint8_t>(bytes.begin() + manifest.piece_size + 7U, bytes.begin() + manifest.piece_size + 34U));
   const auto checkpoint = run(store->async_checkpoint());
   BOOST_TEST(checkpoint.generation > 0U);
   BOOST_CHECK(!checkpoint.published);
   BOOST_CHECK(core::decode_checkpoint(read_file(env.sidecar()), manifest) == checkpoint);
   run(store->async_publish());
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(read_file(env.root / "result") == bytes);
   BOOST_CHECK_THROW((void)store->begin({0, 0}), core::exceptions::storage_busy);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(multifile_tree_and_empty_files_publish_as_one_root) {
   auto env = fixture{};
   const auto files = std::vector<std::vector<std::uint8_t>>{data(17), {}, data(3U * core::block_size + 1U, 7)};
   const auto manifest = describe(files);
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, files);
   BOOST_CHECK(!std::filesystem::exists(env.root / "result"));
   run(store->async_publish());
   for (auto i = 0U; i < files.size(); ++i) {
      BOOST_CHECK(read_file(env.root / "result" / ("dir" + std::to_string(i)) / "file") == files[i]);
   }
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(all_empty_files_need_no_fake_piece) {
   auto env = fixture{};
   const auto manifest = describe({{}, {}});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   BOOST_CHECK(store->snapshot().verified.empty());
   run(store->async_publish());
   BOOST_CHECK(std::filesystem::is_regular_file(env.root / "result/dir0/file"));
   BOOST_CHECK(std::filesystem::file_size(env.root / "result/dir1/file") == 0U);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(partial_blocks_survive_checkpoint_reopen_without_becoming_available) {
   auto env = fixture{};
   const auto bytes = data(3U * core::block_size + 1U);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto writer = store->begin({0, 0});
   run(writer->async_write(core::block_size, {bytes.begin() + core::block_size, bytes.begin() + 2U * core::block_size}));
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::invalid_write);
   const auto checkpoint = run(store->async_checkpoint());
   BOOST_REQUIRE_EQUAL(checkpoint.unfinished.size(), 1U);
   BOOST_CHECK(!core::bitmap_test(checkpoint.verified, 0));
   writer.reset();
   run(store->async_close());
   store.reset();
   store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume)));
   const auto restored = store->snapshot();
   BOOST_REQUIRE_EQUAL(restored.unfinished.size(), 1U);
   BOOST_CHECK(core::bitmap_test(restored.unfinished[0].blocks, 1));
   BOOST_CHECK(!core::bitmap_test(restored.verified, 0));
   writer = store->begin({0, 0});
   run(writer->async_write(0, {bytes.begin(), bytes.begin() + core::block_size}));
   run(writer->async_commit());
   BOOST_CHECK(core::bitmap_test(store->snapshot().verified, 0));
   writer.reset();
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(reopen_rehashes_payload_even_when_size_and_timestamp_are_unchanged) {
   auto env = fixture{};
   const auto bytes = data(4U * core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {bytes});
   run(store->async_close());
   store.reset();
   const auto path = env.stage();
   const auto time = std::filesystem::last_write_time(path);
   auto corrupt = bytes;
   corrupt[1] ^= 1U;
   write_file(path, corrupt);
   std::filesystem::last_write_time(path, time);
   store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume)));
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   BOOST_CHECK(core::bitmap_test(store->snapshot().verified, 1));
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 8)), core::exceptions::unverified_piece);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(seed_uses_the_existing_file_and_verified_reads_detect_later_corruption) {
   auto env = fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   write_file(env.root / "result", bytes);
   auto store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::seed)));
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(run(store->async_read({0, 0}, 3, 5)) == std::vector<std::uint8_t>(bytes.begin() + 3, bytes.begin() + 8));
   auto corrupt = bytes;
   corrupt[0] ^= 1U;
   write_file(env.root / "result", corrupt);
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::integrity_failure);
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   // A corrupted published seed cannot produce a successful durable complete checkpoint.
   BOOST_CHECK_THROW(run(store->async_close()), core::exceptions::invalid_checkpoint);
}

BOOST_AUTO_TEST_CASE(bounded_writers_invalid_ranges_and_integrity_failure_never_publish) {
   auto env = fixture{};
   const auto bytes = data(5U * core::block_size);
   const auto manifest = describe({bytes});
   auto options = env.options(manifest);
   options.max_unfinished = 1;
   auto store = run(storage::file_store::async_open(std::move(options)));
   auto writer = store->begin({0, 0});
   BOOST_CHECK_THROW((void)store->begin({0, 0}), core::exceptions::storage_busy);
   BOOST_CHECK_THROW((void)store->begin({0, 1}), core::exceptions::storage_busy);
   BOOST_CHECK_THROW(run(writer->async_write(1, data(core::block_size))), core::exceptions::invalid_write);
   BOOST_CHECK_THROW(run(writer->async_write(0, data(1))), core::exceptions::invalid_write);
   BOOST_CHECK_THROW(run(store->async_publish()), core::exceptions::unverified_piece);
   run(writer->async_write(0, data(2U * core::block_size, 99)));
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::integrity_failure);
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   run(writer->async_discard());
   writer.reset();
   BOOST_CHECK(store->snapshot().unfinished.empty());
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(existing_destination_and_second_process_owner_are_not_overwritten) {
   auto env = fixture{};
   const auto manifest = describe({data(1)});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume))), storage::exceptions::conflict);
   const auto foreign = data(7, 77);
   write_file(env.root / "result", foreign);
   fill(*store, manifest, {data(1)});
   BOOST_CHECK_THROW(run(store->async_publish()), storage::exceptions::conflict);
   BOOST_CHECK(read_file(env.root / "result") == foreign);
   run(store->async_close());
   store.reset();
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest))), storage::exceptions::conflict);
}

BOOST_AUTO_TEST_CASE(symlink_replacement_cannot_redirect_range_writes) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes, bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   const auto stage = env.stage();
   std::filesystem::create_directory(env.root / "outside");
   const auto foreign = data(8, 19);
   write_file(env.root / "outside/file", foreign);
   std::filesystem::rename(stage / "dir0", stage / "old-dir0");
   std::filesystem::create_directory_symlink(env.root / "outside", stage / "dir0");
   auto writer = store->begin({0, 0});
   BOOST_CHECK_THROW(run(writer->async_write(0, bytes)), storage::exceptions::unsafe_path);
   BOOST_CHECK(read_file(env.root / "outside/file") == foreign);
   writer.reset();
   store.reset();
}

BOOST_AUTO_TEST_CASE(corrupt_or_wrong_identity_checkpoint_and_missing_files_fail_closed) {
   auto env = fixture{};
   const auto manifest = describe({data(17)});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {data(17)});
   run(store->async_close());
   store.reset();
   const auto path = env.sidecar();
   const auto valid = read_file(path);
   auto corrupt = valid;
   corrupt.push_back(0);
   write_file(path, corrupt);
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume))), core::exceptions::invalid_checkpoint);
   auto state = core::decode_checkpoint(valid, manifest);
   state.id.value = core::sha256::hash(std::string{"wrong content"});
   write_file(path, forge::raw::pack(state));
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume))), core::exceptions::invalid_checkpoint);
   write_file(path, valid);
   std::filesystem::remove(env.stage());
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume))), storage::exceptions::missing_data);
}

BOOST_AUTO_TEST_CASE(seed_close_reopen_keeps_payload_and_checkpoint_generation) {
   auto env = fixture{};
   const auto bytes = data(19);
   const auto manifest = describe({bytes});
   write_file(env.root / "result", bytes);
   auto store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::seed)));
   run(store->async_close());
   store.reset();
   const auto checkpoint = core::decode_checkpoint(read_file(env.sidecar()), manifest);
   store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume)));
   BOOST_CHECK(run(store->async_read({0, 0}, 0, 19)) == bytes);
   const auto next = run(store->async_checkpoint());
   BOOST_TEST(next.generation > checkpoint.generation);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(unexpected_seed_symlink_and_case_aliases_are_rejected) {
   auto env = fixture{};
   const auto bytes = data(17);
   auto manifest = describe({bytes});
   write_file(env.root / "outside", bytes);
   std::filesystem::create_symlink(env.root / "outside", env.root / "result");
   BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest, storage::open_mode::seed))), storage::exceptions::unsafe_path);
   std::filesystem::remove(env.root / "result");
   // Case-sensitive filesystems preserve both names; case-folding ones must reject aliases.
   manifest = describe({bytes, bytes});
   manifest.files[0].path = {"A", "x"};
   manifest.files[1].path = {"a", "y"};
   auto probe = std::ofstream{env.root / "CaseProbe"};
   probe.close();
   const auto folding = std::filesystem::exists(env.root / "caseprobe");
   if (folding) {
      BOOST_CHECK_THROW(run(storage::file_store::async_open(env.options(manifest))), storage::exceptions::unsafe_path);
   } else {
      auto store = run(storage::file_store::async_open(env.options(manifest)));
      fill(*store, manifest, {bytes, bytes});
      run(store->async_publish());
      run(store->async_close());
   }
}

BOOST_AUTO_TEST_CASE(external_checkpoint_is_the_only_authority_and_failed_commit_is_not_acknowledged) {
   class backend final : public core::checkpoint_store {
    public:
      std::optional<core::resume_state> value;
      bool fail = false;
      boost::asio::awaitable<std::optional<core::resume_state>> async_load(core::swarm_id, std::stop_token) override { co_return value; }
      boost::asio::awaitable<void> async_save(core::resume_state state, std::stop_token) override {
         if (fail) { throw std::runtime_error{"checkpoint commit failed"}; }
         value = std::move(state);
         co_return;
      }
   };
   auto env = fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   auto db = std::make_shared<backend>();
   auto options = env.options(manifest);
   options.checkpoint = db;
   auto store = run(storage::file_store::async_open(options));
   fill(*store, manifest, {bytes});
   db->fail = true;
   BOOST_CHECK_THROW(run(store->async_checkpoint()), std::runtime_error);
   BOOST_TEST(store->snapshot().durable_generation == 0U);
   db->fail = false;
   const auto checkpoint = run(store->async_checkpoint());
   BOOST_TEST(checkpoint.generation == 2U);
   BOOST_CHECK(db->value == checkpoint);
   for (const auto& entry : std::filesystem::directory_iterator(env.root)) {
      BOOST_CHECK(entry.path().extension() != ".resume");
   }
   run(store->async_publish());
   run(store->async_close());
   store.reset();
   options.mode = storage::open_mode::resume;
   store = run(storage::file_store::async_open(options));
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(run(store->async_read({0, 0}, 0, 17)) == bytes);
   run(store->async_close());
   store.reset();
   options.mode = storage::open_mode::create;
   BOOST_CHECK_THROW(run(storage::file_store::async_open(options)), storage::exceptions::conflict);
}

BOOST_AUTO_TEST_CASE(external_checkpoint_rejects_corrupted_published_state_before_calling_the_backend) {
   class backend final : public core::checkpoint_store {
    public:
      std::optional<core::resume_state> value;
      std::size_t saves = 0;
      boost::asio::awaitable<std::optional<core::resume_state>> async_load(core::swarm_id, std::stop_token) override { co_return value; }
      boost::asio::awaitable<void> async_save(core::resume_state state, std::stop_token) override {
         ++saves;
         value = std::move(state);
         co_return;
      }
   };
   for (const bool seed : {false, true}) {
      auto env = fixture{};
      const auto bytes = data(17);
      const auto manifest = describe({bytes});
      if (seed) { write_file(env.root / "result", bytes); }
      auto db = std::make_shared<backend>();
      auto options = env.options(manifest, seed ? storage::open_mode::seed : storage::open_mode::create);
      options.checkpoint = db;
      auto store = run(storage::file_store::async_open(options));
      if (!seed) {
         fill(*store, manifest, {bytes});
         run(store->async_publish());
      } else {
         static_cast<void>(run(store->async_checkpoint()));
      }
      const auto previous = db->value;
      const auto saves = db->saves;
      const auto durable = store->snapshot().durable_generation;
      BOOST_REQUIRE(previous.has_value());
      auto corrupt = bytes;
      corrupt[0] ^= 1U;
      write_file(env.root / "result", corrupt);
      BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::integrity_failure);
      BOOST_CHECK_THROW(run(store->async_checkpoint()), core::exceptions::invalid_checkpoint);
      BOOST_CHECK_THROW(run(store->async_close()), core::exceptions::invalid_checkpoint);
      BOOST_CHECK_EQUAL(db->saves, saves);
      BOOST_CHECK(db->value == previous);
      BOOST_CHECK_EQUAL(store->snapshot().durable_generation, durable);
      core::validate_checkpoint(*db->value, manifest);
      store.reset();
      write_file(env.root / "result", bytes);
      options.mode = storage::open_mode::resume;
      store = run(storage::file_store::async_open(options));
      BOOST_CHECK(run(store->async_read({0, 0}, 0, 17)) == bytes);
      run(store->async_close());
   }
}

BOOST_AUTO_TEST_CASE(invalid_pool_and_resource_limits_fail_before_touching_payload) {
   auto env = fixture{};
   auto options = env.options(describe({data(1)}));
   options.max_unfinished = core::max_unfinished_pieces + 1U;
   BOOST_CHECK_THROW(run(storage::file_store::async_open(options)), storage::exceptions::invalid_options);
   options.max_unfinished = 1;
   options.disk = {};
   BOOST_CHECK_THROW(run(storage::file_store::async_open(options)), storage::exceptions::invalid_options);
   BOOST_CHECK(std::filesystem::is_empty(env.root));
}

BOOST_AUTO_TEST_CASE(repeated_orphan_checkpoint_recovery_uses_one_private_slot) {
   auto env = fixture{};
   const auto manifest = describe({data(17)});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   const auto first = run(store->async_checkpoint());
   const auto temporary = std::filesystem::path{env.sidecar().string() + ".tmp-0"};
   for (auto attempt = 0U; attempt < 16U; ++attempt) {
      write_file(temporary, data(7));
      std::filesystem::permissions(temporary, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
      const auto next = run(store->async_checkpoint());
      BOOST_TEST(next.generation > first.generation);
      BOOST_CHECK(!std::filesystem::exists(temporary));
      auto sidecars = 0U;
      for (const auto& entry : std::filesystem::directory_iterator(env.root)) {
         sidecars += entry.path().extension() == ".resume";
         BOOST_CHECK(entry.path().filename().string().find(".tmp-") == std::string::npos);
      }
      BOOST_TEST(sidecars == 1U);
   }
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(untrusted_temporary_symlink_and_hardlink_never_delete_their_target_or_checkpoint) {
   auto env = fixture{};
   const auto manifest = describe({data(17)});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   static_cast<void>(run(store->async_checkpoint()));
   const auto checkpoint = read_file(env.sidecar());
   const auto temporary = std::filesystem::path{env.sidecar().string() + ".tmp-0"};
   const auto outside = env.root / "foreign";
   const auto foreign = data(11, 97);
   write_file(outside, foreign);
   std::filesystem::create_symlink(outside, temporary);
   BOOST_CHECK_THROW(run(store->async_checkpoint()), storage::exceptions::unsafe_path);
   BOOST_CHECK(std::filesystem::is_symlink(temporary));
   BOOST_CHECK(read_file(outside) == foreign);
   BOOST_CHECK(read_file(env.sidecar()) == checkpoint);
   std::filesystem::remove(temporary);
   std::filesystem::create_hard_link(outside, temporary);
   BOOST_CHECK_THROW(run(store->async_checkpoint()), storage::exceptions::unsafe_path);
   BOOST_CHECK(std::filesystem::exists(temporary));
   BOOST_CHECK(read_file(outside) == foreign);
   BOOST_CHECK(read_file(env.sidecar()) == checkpoint);
   std::filesystem::remove(temporary);
   run(store->async_close());
}

BOOST_AUTO_TEST_SUITE_END()
