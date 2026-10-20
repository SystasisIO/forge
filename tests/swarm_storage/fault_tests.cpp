#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include "support.hpp"
#include "io_faults.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <stop_token>
#include <sys/wait.h>

using namespace swarm_test;

namespace {

struct counting_checkpoint final : core::checkpoint_store {
   std::atomic<unsigned> saves{0};
   boost::asio::awaitable<std::optional<core::resume_state>> async_load(core::swarm_id, std::stop_token) override {
      co_return std::nullopt;
   }
   boost::asio::awaitable<void> async_save(core::resume_state, std::stop_token) override {
      saves.fetch_add(1U);
      co_return;
   }
};

struct temporary_root {
   std::filesystem::path path;
   temporary_root() {
      auto name = (std::filesystem::temp_directory_path() / "forge-swarm-crash-XXXXXX").string();
      if (!::mkdtemp(name.data())) throw std::runtime_error{"mkdtemp failed"};
      path = std::filesystem::canonical(name);
   }
   ~temporary_root() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};

storage::file_store::options crash_options(const core::descriptor& manifest, const std::filesystem::path& root,
   forge::asio::compute::pool& disk, forge::asio::compute::pool& cpu, storage::open_mode mode) {
   return {.manifest = manifest, .destination = root / "result", .mode = mode,
           .disk = disk.get_executor(), .cpu = cpu.get_executor()};
}

void prepare(core::store& store, const core::descriptor& manifest,
             const std::vector<std::vector<std::uint8_t>>& files, bool complete) {
   for (auto file = 0U; file < files.size(); ++file) {
      const auto count = core::piece_count(files[file].size(), manifest.piece_size);
      for (auto piece = 0U; piece < count; ++piece) {
         const auto id = core::piece_id{file, piece};
         const auto offset = std::size_t{piece} * manifest.piece_size;
         auto length = core::piece_length(manifest, id);
         const bool partial = !complete && file + 1U == files.size() && piece + 1U == count;
         if (partial) length = core::block_size;
         auto writer = store.begin(id);
         run(writer->async_write(0U, {files[file].begin() + offset, files[file].begin() + offset + length}));
         if (!partial) run(writer->async_commit());
      }
   }
}

// Called only while the parent owns no compute pools or I/O threads. Child pools start AFTER fork.
std::uint64_t child_trial(const std::filesystem::path& root, const core::descriptor& manifest,
                         const std::vector<std::vector<std::uint8_t>>& files, bool publish,
                         std::uint64_t crash_at = 0U, faults::crash_edge edge = faults::crash_edge::before) {
   int report[2];
   BOOST_REQUIRE_EQUAL(::pipe(report), 0);
   const auto close_pipe = boost::scope::scope_exit{[&] { ::close(report[0]); ::close(report[1]); }};
   const auto child = ::fork();
   BOOST_REQUIRE_MESSAGE(child >= 0, "fork failed");
   if (child == 0) {
      ::close(report[0]);
      std::signal(SIGALRM, SIG_DFL);
      ::alarm(10);
      try {
         auto observed = std::uint64_t{0};
         {
            auto disk = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 16}};
            auto cpu = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 16}};
            auto store = run(storage::file_store::async_open(crash_options(manifest, root, disk, cpu, storage::open_mode::create)));
            prepare(*store, manifest, files, publish);
            auto injection = faults::scope{{.crash_fsync = crash_at, .edge = edge}};
            if (publish) run(store->async_publish());
            else static_cast<void>(run(store->async_checkpoint()));
            observed = injection.snapshot().syncs;
            injection.disarm();
            run(store->async_close());
         }
         auto done = std::size_t{0};
         while (done < sizeof(observed)) {
            const auto n = ::write(report[1], reinterpret_cast<const char*>(&observed) + done, sizeof(observed) - done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) ::_exit(91);
            done += static_cast<std::size_t>(n);
         }
         ::_exit(crash_at == 0U ? 0 : 92); // An armed crash point that was not hit is a test failure.
      } catch (...) { ::_exit(90); }
   }
   ::close(report[1]); report[1] = -1;
   auto status = int{};
   auto waited = pid_t{};
   do { waited = ::waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
   BOOST_REQUIRE_EQUAL(waited, child);
   BOOST_REQUIRE_MESSAGE(WIFEXITED(status), "child failed or hit its bounded watchdog");
   BOOST_REQUIRE_EQUAL(WEXITSTATUS(status), crash_at == 0U ? 0 : 77);
   if (crash_at != 0U) return 0U;
   auto count = std::uint64_t{0};
   auto done = std::size_t{0};
   while (done < sizeof(count)) {
      const auto n = ::read(report[0], reinterpret_cast<char*>(&count) + done, sizeof(count) - done);
      if (n < 0 && errno == EINTR) continue;
      BOOST_REQUIRE(n > 0);
      done += static_cast<std::size_t>(n);
   }
   BOOST_REQUIRE_MESSAGE(count > 0U, "NOT_PROVEN: production fsync calls did not reach executable interposition");
   BOOST_REQUIRE_LE(count, 64U);
   return count;
}

void verify_recovered(const std::filesystem::path& root, const core::descriptor& manifest,
                      const std::vector<std::vector<std::uint8_t>>& files, bool complete) {
   auto disk = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 16}};
   auto cpu = forge::asio::compute::pool{{.worker_threads = 2, .max_pending_tasks = 16}};
   auto store = run(storage::file_store::async_open(crash_options(manifest, root, disk, cpu, storage::open_mode::resume)));
   const auto state = store->snapshot();
   const bool published = std::filesystem::exists(root / "result");
   BOOST_CHECK_EQUAL(state.published, published);
   auto payload = root / "result";
   if (!published) {
      for (const auto& entry : std::filesystem::directory_iterator(root)) {
         if (entry.path().extension() == ".part") payload = entry.path();
      }
   }
   auto ordinal = 0U;
   for (auto file = 0U; file < files.size(); ++file) {
      auto path = payload;
      for (const auto& component : manifest.files[file].path) path /= component;
      const auto actual = read_file(path);
      BOOST_REQUIRE_EQUAL(actual.size(), files[file].size());
      const auto count = core::piece_count(files[file].size(), manifest.piece_size);
      for (auto piece = 0U; piece < count; ++piece, ++ordinal) {
         const auto id = core::piece_id{file, piece};
         const auto start = std::size_t{piece} * manifest.piece_size;
         const auto length = core::piece_length(manifest, id);
         const bool matches = std::equal(actual.begin() + start, actual.begin() + start + length, files[file].begin() + start);
         const bool available = core::bitmap_test(state.verified, ordinal);
         BOOST_CHECK_EQUAL(available, matches); // Byte oracle, not the stored checkpoint bitmap or timestamps.
         if (complete) BOOST_CHECK(matches);
         if (available) {
            BOOST_CHECK(run(store->async_read(id, 0, length)) ==
                        std::vector<std::uint8_t>(files[file].begin() + start, files[file].begin() + start + length));
         } else {
            BOOST_CHECK_THROW(run(store->async_read(id, 0, 1)), core::exceptions::unverified_piece);
         }
      }
   }
   run(store->async_close());
} // All pools are joined before returning to the next fork.

void crash_matrix(bool publish) {
   auto reset = faults::scope{};
   reset.disarm();
   const auto files = std::vector<std::vector<std::uint8_t>>{data(2U * core::block_size + 7U), data(3U * core::block_size + 7U, 19)};
   auto manifest = describe(files);
   manifest.files[0].path = {"dir0", "nested", "file"};
   auto count = std::uint64_t{0};
   {
      auto baseline = temporary_root{};
      count = child_trial(baseline.path, manifest, files, publish);
   }
   for (const auto edge : {faults::crash_edge::before, faults::crash_edge::after}) {
      for (auto point = std::uint64_t{1}; point <= count; ++point) {
         BOOST_TEST_CONTEXT("publish=" << publish << " fsync=" << point << " after=" << (edge == faults::crash_edge::after)) {
            auto trial = temporary_root{};
            child_trial(trial.path, manifest, files, publish, point, edge);
            verify_recovered(trial.path, manifest, files, publish);
         }
      }
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_storage_faults)

BOOST_AUTO_TEST_CASE(every_checkpoint_fsync_failure_preserves_durable_generation_and_retry_consumes_a_generation) {
   auto count = std::uint64_t{0};
   {
      auto env = fixture{};
      const auto bytes = data(core::block_size + 17U);
      const auto manifest = describe({bytes});
      auto store = run(storage::file_store::async_open(env.options(manifest)));
      fill(*store, manifest, {bytes});
      auto observation = faults::scope{};
      static_cast<void>(run(store->async_checkpoint()));
      count = observation.snapshot().syncs;
      observation.disarm();
      run(store->async_close());
   }
   BOOST_REQUIRE_MESSAGE(count > 0U, "NOT_PROVEN: production fsync interposition inactive");
   BOOST_REQUIRE_LE(count, 64U);
   for (auto point = std::uint64_t{1}; point <= count; ++point) {
      BOOST_TEST_CONTEXT("fsync failure=" << point) {
         auto env = fixture{};
         const auto bytes = data(core::block_size + 17U);
         const auto manifest = describe({bytes});
         auto store = run(storage::file_store::async_open(env.options(manifest)));
         fill(*store, manifest, {bytes});
         const auto before = run(store->async_checkpoint());
         auto fault = faults::scope{{.fsync_eio = point}};
         BOOST_CHECK_THROW(run(store->async_checkpoint()), storage::exceptions::io_error);
         BOOST_CHECK_EQUAL(fault.snapshot().injected_errors, 1U);
         BOOST_CHECK_EQUAL(store->snapshot().durable_generation, before.generation);
         fault.disarm();
         const auto retried = run(store->async_checkpoint());
         BOOST_CHECK_EQUAL(retried.generation, before.generation + 2U);
         BOOST_CHECK_EQUAL(store->snapshot().durable_generation, retried.generation);
         BOOST_CHECK(core::decode_checkpoint(read_file(env.sidecar()), manifest) == retried);
         BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
         run(store->async_close());
      }
   }
}

BOOST_AUTO_TEST_CASE(partial_enospc_write_never_marks_blocks_or_pieces_available_and_can_be_retried) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto writer = store->begin({0, 0});
   auto fault = faults::scope{{.write_enospc = 2, .short_write = 97}};
   BOOST_CHECK_THROW(run_with_diagnostics(writer->async_write(0, bytes), [&] {
      const auto disk = env.disk.snapshot();
      const auto failures = fault.snapshot();
      std::fprintf(stderr, "disk: failed=%llu completed=%llu pending=%zu running=%zu; writes=%llu errors=%llu\n",
         static_cast<unsigned long long>(disk.failed), static_cast<unsigned long long>(disk.completed),
         static_cast<std::size_t>(disk.pending), static_cast<std::size_t>(disk.running),
         static_cast<unsigned long long>(failures.writes), static_cast<unsigned long long>(failures.injected_errors));
   }), storage::exceptions::io_error);
   BOOST_REQUIRE_EQUAL(fault.snapshot().injected_errors, 1U);
   BOOST_CHECK_EQUAL(fault.snapshot().writes, 2U);
   const auto state = store->snapshot();
   BOOST_CHECK(!core::bitmap_test(state.verified, 0));
   BOOST_REQUIRE_EQUAL(state.unfinished.size(), 1U);
   BOOST_CHECK(!core::bitmap_test(state.unfinished.front().blocks, 0));
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::invalid_write);
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::unverified_piece);
   fault.disarm();
   run(writer->async_write(0, bytes));
   run(writer->async_commit());
   BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(payload_close_error_after_successful_pwrite_never_marks_coverage_and_can_be_retried) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto writer = store->begin({0, 0});
   auto fault = faults::scope{{.close_eio = 1}};
   BOOST_CHECK_THROW(run(writer->async_write(0, bytes)), storage::exceptions::io_error);
   const auto seen = fault.snapshot();
   BOOST_REQUIRE_MESSAGE(seen.injected_errors == 1U && seen.writes == 1U,
                         "NOT_PROVEN: payload close after successful pwrite was not intercepted");
   BOOST_CHECK_EQUAL(seen.closes, 1U);
   fault.disarm();
   // Even fully written bytes cannot acknowledge a block when its resource close failed.
   BOOST_CHECK(read_file(env.stage()) == bytes);
   const auto state = store->snapshot();
   BOOST_CHECK(!core::bitmap_test(state.verified, 0));
   BOOST_REQUIRE_EQUAL(state.unfinished.size(), 1U);
   BOOST_CHECK(!core::bitmap_test(state.unfinished.front().blocks, 0));
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::invalid_write);
   BOOST_CHECK_THROW(run(store->async_read({0, 0}, 0, 1)), core::exceptions::unverified_piece);
   run(writer->async_write(0, bytes));
   run(writer->async_commit());
   BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(external_checkpoint_cannot_commit_before_the_payload_parent_directory_fence) {
   auto env = fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   auto backend = std::make_shared<counting_checkpoint>();
   auto options = env.options(manifest);
   options.checkpoint = backend;
   auto store = run(storage::file_store::async_open(std::move(options)));
   fill(*store, manifest, {bytes});
   const auto before = store->snapshot().durable_generation;
   auto fault = faults::scope{{.fsync_eio = 2}}; // Single payload file, then its containing parent.
   BOOST_CHECK_THROW(run(store->async_checkpoint()), storage::exceptions::io_error);
   BOOST_CHECK_EQUAL(fault.snapshot().syncs, 2U);
   BOOST_REQUIRE_EQUAL(fault.snapshot().injected_errors, 1U);
   BOOST_CHECK_EQUAL(backend->saves.load(), 0U);
   BOOST_CHECK_EQUAL(store->snapshot().durable_generation, before);
   fault.disarm();
   const auto retried = run(store->async_checkpoint());
   BOOST_CHECK_EQUAL(retried.generation, 2U);
   BOOST_CHECK_EQUAL(backend->saves.load(), 1U);
   BOOST_CHECK_EQUAL(store->snapshot().durable_generation, retried.generation);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(short_positional_io_and_eintr_are_retried_without_losing_offsets) {
   auto env = fixture{};
   const auto bytes = data(core::block_size + 17U, 37);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{{.write_eintr = 1, .read_eintr = 1, .short_write = 97, .short_read = 113}};
   fill(*store, manifest, {bytes});
   BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
   BOOST_CHECK(read_file(env.stage()) == bytes);
   const auto seen = fault.snapshot();
   BOOST_CHECK_EQUAL(seen.injected_errors, 2U);
   BOOST_CHECK_GT(seen.writes, 2U);
   BOOST_CHECK_GT(seen.reads, 2U);
   BOOST_CHECK(!seen.timed_out);
   fault.disarm();
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(checkpoint_temporary_close_error_preserves_the_old_checkpoint_and_durable_generation) {
   auto env = fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {bytes});
   const auto before = run(store->async_checkpoint());
   const auto old_bytes = read_file(env.sidecar());
   auto fault = faults::scope{{.close_eio = 1, .close_filter = faults::close_target::checkpoint_temporary}};
   BOOST_CHECK_THROW(run(store->async_checkpoint()), storage::exceptions::io_error);
   BOOST_REQUIRE_MESSAGE(fault.snapshot().injected_errors == 1U,
                         "NOT_PROVEN: checkpoint temporary close was not intercepted");
   BOOST_CHECK_EQUAL(fault.snapshot().closes, 1U);
   BOOST_CHECK_EQUAL(store->snapshot().durable_generation, before.generation);
   fault.disarm();
   BOOST_CHECK(read_file(env.sidecar()) == old_bytes);
   BOOST_CHECK_EQUAL(core::decode_checkpoint(old_bytes, manifest).generation, before.generation);
   const auto retried = run(store->async_checkpoint());
   BOOST_CHECK_EQUAL(retried.generation, before.generation + 2U);
   BOOST_CHECK(core::decode_checkpoint(read_file(env.sidecar()), manifest) == retried);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(explicit_resource_close_reports_eio_but_releases_ownership_and_remains_idempotent) {
   auto env = fixture{};
   const auto bytes = data(core::block_size + 3U);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {bytes});
   auto fault = faults::scope{{.close_eio = 1, .close_filter = faults::close_target::ownership_lock}};
   BOOST_CHECK_THROW(run(store->async_close()), storage::exceptions::io_error);
   BOOST_REQUIRE_MESSAGE(fault.snapshot().injected_errors == 1U,
                         "NOT_PROVEN: explicit ownership close was not intercepted");
   BOOST_CHECK_EQUAL(fault.snapshot().closes, 1U);
   BOOST_CHECK_THROW((void)store->begin({0, 0}), core::exceptions::storage_closed);
   fault.disarm();
   run(store->async_close());
   // Reopen while the old store object still exists: the injected error must not retain its lock FD.
   auto reopened = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume)));
   BOOST_CHECK(run(reopened->async_read({0, 0}, 0, bytes.size())) == bytes);
   run(reopened->async_close());
}

#if defined(__APPLE__)
// Linux uses raw renameat2; this executable only interposes Darwin's libc entry point.
BOOST_AUTO_TEST_CASE(publication_exdev_is_typed_and_never_falls_back_to_copy) {
   BOOST_REQUIRE_MESSAGE(faults::rename_available(),
      "NOT_PROVEN: macOS renameatx_np is required; Linux publication uses a non-interposed raw renameat2 syscall");
   auto env = fixture{};
   const auto bytes = data(3U * core::block_size + 7U);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {bytes});
   const auto stage = env.stage();
   const auto before = read_file(stage);
   auto fault = faults::scope{{.rename_exdev_once = true}};
   // Same-parent staging is deliberate: this injects the EXDEV branch, not a real cross-mount move.
   BOOST_CHECK_THROW(run(store->async_publish()), storage::exceptions::cross_device);
   BOOST_REQUIRE_MESSAGE(fault.snapshot().injected_errors == 1U, "NOT_PROVEN: publication rename was not intercepted");
   BOOST_CHECK_EQUAL(fault.snapshot().renames, 1U);
   BOOST_CHECK(!store->snapshot().published);
   BOOST_CHECK(!std::filesystem::exists(env.root / "result"));
   BOOST_CHECK(std::filesystem::is_regular_file(stage));
   BOOST_CHECK(read_file(stage) == before);
   BOOST_CHECK(before == bytes);
   BOOST_CHECK(run(store->async_read({0, 0}, 0, manifest.piece_size)) ==
               std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + manifest.piece_size));
   fault.disarm();
   run(store->async_close());
}
#endif

BOOST_AUTO_TEST_CASE(checkpoint_process_crash_before_and_after_every_observed_fsync) { crash_matrix(false); }
BOOST_AUTO_TEST_CASE(publication_process_crash_before_and_after_every_observed_fsync) { crash_matrix(true); }

BOOST_AUTO_TEST_SUITE_END()
