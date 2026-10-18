#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <chrono>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include "support.hpp"
#include "io_faults.hpp"

using namespace swarm_test;
using namespace std::chrono_literals;

namespace {

template <typename T> T take(std::future<T>& result) {
   BOOST_REQUIRE_MESSAGE(result.wait_for(5s) == std::future_status::ready, "asynchronous operation exceeded its bounded deadline");
   return result.get();
}

struct concurrent_io {
   boost::asio::io_context io;
   boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{boost::asio::make_work_guard(io)};
   std::jthread worker{[this] { io.run(); }};
   ~concurrent_io() { io.stop(); }

   template <typename T> auto start(boost::asio::awaitable<T> operation) {
      return boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
   }
   void fence() {
      auto done = std::make_shared<std::promise<void>>();
      auto result = done->get_future();
      boost::asio::post(io, [done] { done->set_value(); });
      take(result);
   }
};

struct blocking_checkpoint final : core::checkpoint_store {
   std::atomic<unsigned> saves{0};
   std::atomic<unsigned> commits{0};
   std::atomic<unsigned> canceled{0};
   std::atomic<bool> timed_out{false};
   std::promise<void> entered;
   std::mutex mutex;
   std::shared_ptr<boost::asio::steady_timer> timer;
   std::optional<core::resume_state> saved;
   bool released = false;

   boost::asio::awaitable<std::optional<core::resume_state>> async_load(core::swarm_id, std::stop_token) override {
      const auto lock = std::scoped_lock{mutex};
      co_return saved;
   }
   boost::asio::awaitable<void> async_save(core::resume_state value, std::stop_token token) override {
      if (token.stop_requested()) {
         canceled.fetch_add(1U);
         throw forge::asio::exceptions::canceled{"test checkpoint canceled before admission"};
      }
      if (saves.fetch_add(1U) == 0U) {
         const auto executor = co_await boost::asio::this_coro::executor;
         auto gate = std::make_shared<boost::asio::steady_timer>(executor);
         gate->expires_after(4s);
         { const auto lock = std::scoped_lock{mutex}; timer = gate; }
         const auto stop = std::stop_callback{token, [gate, executor] {
            boost::asio::post(executor, [gate] { gate->cancel(); });
         }};
         entered.set_value();
         auto error = boost::system::error_code{};
         co_await gate->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         const auto lock = std::scoped_lock{mutex};
         timer.reset();
         if (error != boost::asio::error::operation_aborted) {
            timed_out.store(true);
            throw std::runtime_error{"checkpoint barrier exceeded its bounded deadline"};
         }
         if (!released && !token.stop_requested()) {
            throw std::runtime_error{"checkpoint barrier was canceled without release or stop"};
         }
      }
      if (token.stop_requested()) {
         canceled.fetch_add(1U);
         throw forge::asio::exceptions::canceled{"test checkpoint canceled"};
      }
      { const auto lock = std::scoped_lock{mutex}; saved = std::move(value); commits.fetch_add(1U); }
   }
   void release() {
      auto gate = std::shared_ptr<boost::asio::steady_timer>{};
      { const auto lock = std::scoped_lock{mutex}; released = true; gate = timer; }
      if (gate) boost::asio::post(gate->get_executor(), [gate] { gate->cancel(); });
   }
};

boost::asio::awaitable<core::resume_state> announced_checkpoint(
   storage::file_store* store, std::stop_token token, std::shared_ptr<std::promise<void>> entered) {
   entered->set_value();
   co_return co_await store->async_checkpoint(token);
}

boost::asio::awaitable<void> close_then_cancellable_timer(
   storage::file_store* store, std::shared_ptr<std::promise<void>> closed) {
   co_await store->async_close();
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(4s);
   closed->set_value();
   co_await timer.async_wait(boost::asio::use_awaitable);
   throw std::runtime_error{"caller timer expired: cancellation was lost across close"};
}

} // namespace

BOOST_AUTO_TEST_SUITE(swarm_storage_concurrency)

BOOST_AUTO_TEST_CASE(independent_piece_writes_overlap_and_cpu_progresses_while_all_disk_workers_are_blocked) {
   auto env = fixture{};
   const auto bytes = data(2U * core::block_size);
   const auto manifest = describe({bytes}, core::block_size);
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{{.pause_first_writes = 2}};
   auto loop = concurrent_io{};
   auto first = store->begin({0, 0});
   auto second = store->begin({0, 1});
   auto a = loop.start(first->async_write(0, {bytes.begin(), bytes.begin() + core::block_size}));
   auto b = loop.start(second->async_write(0, {bytes.begin() + core::block_size, bytes.end()}));
   BOOST_REQUIRE_MESSAGE(fault.wait_paused(2), "NOT_PROVEN: two production pwrite calls did not enter the barrier");
   auto cpu = loop.start(env.cpu.get_executor().execute({"swarm-test-cpu-progress", {}}, [] { return 42; }));
   auto disk = loop.start(env.disk.get_executor().execute({"swarm-test-disk-probe", {}}, [] { return 7; }));
   BOOST_CHECK_EQUAL(take(cpu), 42);
   BOOST_CHECK(disk.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(a.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(b.wait_for(0s) == std::future_status::timeout);
   fault.release();
   take(a); take(b);
   BOOST_CHECK_EQUAL(take(disk), 7);
   run(first->async_commit()); run(second->async_commit());
   BOOST_CHECK(run(store->async_read({0, 0}, 0, core::block_size)) == std::vector<std::uint8_t>(bytes.begin(), bytes.begin() + core::block_size));
   BOOST_CHECK(run(store->async_read({0, 1}, 0, core::block_size)) == std::vector<std::uint8_t>(bytes.begin() + core::block_size, bytes.end()));
   BOOST_CHECK(!fault.snapshot().timed_out);
   fault.disarm();
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(abandoned_writer_cannot_advance_block_state_or_revive_a_replacement_epoch) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{{.pause_first_writes = 1}};
   auto loop = concurrent_io{};
   auto writer = store->begin({0, 0});
   auto stale = loop.start(writer->async_write(0, bytes));
   BOOST_REQUIRE(fault.wait_paused());
   writer.reset();
   BOOST_CHECK_THROW((void)store->begin({0, 0}), core::exceptions::storage_busy);
   fault.release();
   BOOST_CHECK_THROW(take(stale), core::exceptions::invalid_write);
   writer = store->begin({0, 0});
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::invalid_write);
   const auto state = store->snapshot();
   BOOST_CHECK(!core::bitmap_test(state.verified, 0));
   BOOST_REQUIRE_EQUAL(state.unfinished.size(), 1U);
   BOOST_CHECK(!core::bitmap_test(state.unfinished.front().blocks, 0));
   run(writer->async_write(0, bytes));
   run(writer->async_commit());
   BOOST_CHECK(core::bitmap_test(store->snapshot().verified, 0));
   BOOST_CHECK(!fault.snapshot().timed_out);
   fault.disarm();
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(close_joins_blocked_io_and_waits_for_the_explicit_lease) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{{.pause_first_writes = 1}};
   auto loop = concurrent_io{};
   auto lease = store->acquire_lease();
   auto writer = store->begin({0, 0});
   auto write = loop.start(writer->async_write(0, bytes));
   BOOST_REQUIRE(fault.wait_paused());
   auto changed = loop.start(store->async_wait_change(store->snapshot().generation));
   auto close = loop.start(store->async_close());
   BOOST_CHECK_THROW(take(changed), core::exceptions::storage_closed); // Real shutdown notification.
   BOOST_CHECK(close.wait_for(0s) == std::future_status::timeout);
   fault.release();
   BOOST_CHECK_THROW(take(write), forge::asio::exceptions::canceled);
   loop.fence();
   BOOST_CHECK(close.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(lease.active());
   lease.release();
   take(close);
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   BOOST_CHECK_THROW((void)store->begin({0, 0}), core::exceptions::storage_closed);
   auto after_close = loop.start(store->async_wait_change(store->snapshot().generation));
   BOOST_CHECK_THROW(take(after_close), core::exceptions::storage_closed);
   BOOST_CHECK(!fault.snapshot().timed_out);
}

BOOST_AUTO_TEST_CASE(pre_cancellation_and_inflight_cancellation_never_mark_late_bytes_available) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto writer = store->begin({0, 0});
   auto fault = faults::scope{{.pause_first_writes = 1}};
   auto pre = std::stop_source{};
   pre.request_stop();
   BOOST_CHECK_THROW(run(writer->async_write(0, bytes, pre.get_token())), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(fault.snapshot().writes, 0U);
   auto loop = concurrent_io{};
   auto inflight = std::stop_source{};
   auto result = loop.start(writer->async_write(0, bytes, inflight.get_token()));
   BOOST_REQUIRE(fault.wait_paused());
   inflight.request_stop();
   fault.release();
   BOOST_CHECK_THROW(take(result), forge::asio::exceptions::canceled);
   BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, 0));
   BOOST_CHECK_THROW(run(writer->async_commit()), core::exceptions::invalid_write);
   run(writer->async_write(0, bytes));
   run(writer->async_commit());
   BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
   BOOST_CHECK(!fault.snapshot().timed_out);
   fault.disarm();
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(close_cannot_retire_a_blocked_operation_even_without_a_lease) {
   auto env = fixture{};
   const auto bytes = data(core::block_size);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{{.pause_first_writes = 1}};
   auto loop = concurrent_io{};
   auto writer = store->begin({0, 0});
   auto write = loop.start(writer->async_write(0, bytes));
   BOOST_REQUIRE(fault.wait_paused());
   auto changed = loop.start(store->async_wait_change(store->snapshot().generation));
   auto close = loop.start(store->async_close());
   BOOST_CHECK_THROW(take(changed), core::exceptions::storage_closed);
   loop.fence();
   BOOST_CHECK(close.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(fault.snapshot().syncs, 0U);
   fault.release();
   BOOST_CHECK_THROW(take(write), forge::asio::exceptions::canceled);
   take(close);
   BOOST_CHECK(!fault.snapshot().timed_out);
}

BOOST_AUTO_TEST_CASE(owner_stop_cancels_a_queued_disk_write_before_blocked_workers_are_released) {
   auto env = fixture{};
   const auto bytes = data(3U * core::block_size);
   const auto manifest = describe({bytes}, core::block_size);
   auto options = env.options(manifest);
   options.max_parallel_io = 3;
   auto store = run(storage::file_store::async_open(std::move(options)));
   auto fault = faults::scope{{.pause_first_writes = 2}};
   auto loop = concurrent_io{};
   auto first = store->begin({0, 0});
   auto second = store->begin({0, 1});
   auto third = store->begin({0, 2});
   auto a = loop.start(first->async_write(0, {bytes.begin(), bytes.begin() + core::block_size}));
   auto b = loop.start(second->async_write(0, {bytes.begin() + core::block_size, bytes.begin() + 2U * core::block_size}));
   BOOST_REQUIRE(fault.wait_paused(2));
   auto queued = loop.start(third->async_write(0, {bytes.begin() + 2U * core::block_size, bytes.end()}));
   loop.fence();
   BOOST_REQUIRE_MESSAGE(env.disk.snapshot().pending > 0U, "disk write was not admitted to the compute queue");
   store->request_stop();
   BOOST_CHECK_THROW(take(queued), forge::asio::exceptions::canceled);
   BOOST_CHECK_EQUAL(fault.snapshot().writes, 2U);
   BOOST_CHECK(!fault.snapshot().timed_out);
   BOOST_CHECK(a.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(b.wait_for(0s) == std::future_status::timeout);
   fault.release();
   BOOST_CHECK_THROW(take(a), forge::asio::exceptions::canceled);
   BOOST_CHECK_THROW(take(b), forge::asio::exceptions::canceled);
   run(store->async_close());
   for (auto index = 0U; index < 3U; ++index) BOOST_CHECK(!core::bitmap_test(store->snapshot().verified, index));
   BOOST_CHECK(!fault.snapshot().timed_out);
}

BOOST_AUTO_TEST_CASE(queued_checkpoint_cancellation_does_not_wait_for_or_commit_the_active_backend) {
   auto env = fixture{};
   const auto manifest = describe({data(17)});
   auto backend = std::make_shared<blocking_checkpoint>();
   auto entered = backend->entered.get_future();
   auto options = env.options(manifest);
   options.checkpoint = backend;
   auto store = run(storage::file_store::async_open(std::move(options)));
   fill(*store, manifest, {data(17)});
   auto fault = faults::scope{};
   auto loop = concurrent_io{};
   const auto release = boost::scope::scope_exit{[backend] { backend->release(); }};
   auto first = loop.start(store->async_checkpoint());
   take(entered);
   auto stop = std::stop_source{};
   auto announced = std::make_shared<std::promise<void>>();
   auto started = announced->get_future();
   auto queued = loop.start(announced_checkpoint(store.get(), stop.get_token(), announced));
   take(started);
   loop.fence();
   BOOST_CHECK(queued.wait_for(0s) == std::future_status::timeout);
   stop.request_stop();
   BOOST_CHECK_THROW(take(queued), forge::asio::exceptions::canceled);
   BOOST_CHECK(first.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK_EQUAL(backend->saves.load(), 1U);
   backend->release();
   static_cast<void>(take(first));
   BOOST_CHECK_EQUAL(backend->saves.load(), 1U);
   BOOST_CHECK(!backend->timed_out.load());
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(owner_stop_cancels_active_and_queued_checkpoints_but_close_still_waits_for_its_lease) {
   auto env = fixture{};
   const auto bytes = data(17);
   const auto manifest = describe({bytes});
   auto backend = std::make_shared<blocking_checkpoint>();
   auto entered = backend->entered.get_future();
   auto options = env.options(manifest);
   options.checkpoint = backend;
   options.max_parallel_io = 2;
   auto store = run(storage::file_store::async_open(std::move(options)));
   fill(*store, manifest, {bytes});
   auto fault = faults::scope{};
   auto loop = concurrent_io{};
   const auto release = boost::scope::scope_exit{[backend] { backend->release(); }};
   auto lease = store->acquire_lease();
   auto first = loop.start(store->async_checkpoint());
   take(entered);
   auto announced = std::make_shared<std::promise<void>>();
   auto started = announced->get_future();
   auto queued = loop.start(announced_checkpoint(store.get(), {}, announced));
   take(started);
   loop.fence();
   BOOST_CHECK(queued.wait_for(0s) == std::future_status::timeout);
   auto excess_checkpoint = loop.start(store->async_checkpoint());
   BOOST_CHECK_THROW(take(excess_checkpoint), core::exceptions::storage_busy);
   auto excess_publish = loop.start(store->async_publish());
   BOOST_CHECK_THROW(take(excess_publish), core::exceptions::storage_busy);
   auto close = loop.start(store->async_close());
   BOOST_CHECK_THROW(take(first), forge::asio::exceptions::canceled);
   BOOST_CHECK_THROW(take(queued), forge::asio::exceptions::canceled);
   loop.fence();
   BOOST_CHECK_EQUAL(backend->canceled.load(), 1U);
   BOOST_CHECK_EQUAL(backend->saves.load(), 1U);
   BOOST_CHECK_EQUAL(backend->commits.load(), 0U);
   BOOST_CHECK_EQUAL(store->snapshot().durable_generation, 0U);
   BOOST_CHECK(close.wait_for(0s) == std::future_status::timeout);
   BOOST_CHECK(lease.active());
   lease.release();
   take(close);
   BOOST_CHECK_EQUAL(backend->saves.load(), 2U);
   BOOST_CHECK_EQUAL(backend->commits.load(), 1U);
   BOOST_CHECK_EQUAL(store->snapshot().durable_generation, 2U);
   BOOST_CHECK(!backend->timed_out.load());
}

BOOST_AUTO_TEST_CASE(caller_timer_remains_cancellable_after_joined_close) {
   auto env = fixture{};
   const auto manifest = describe({data(1)});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   auto fault = faults::scope{};
   auto loop = concurrent_io{};
   auto cancellation = boost::asio::cancellation_signal{};
   auto closed = std::make_shared<std::promise<void>>();
   auto reached = closed->get_future();
   auto result = boost::asio::co_spawn(loop.io, close_then_cancellable_timer(store.get(), closed),
      boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   take(reached);
   loop.fence();
   boost::asio::post(loop.io, [&] { cancellation.emit(boost::asio::cancellation_type::all); });
   BOOST_CHECK_EXCEPTION(take(result), boost::system::system_error, [](const auto& error) {
      return error.code() == boost::asio::error::operation_aborted;
   });
}

BOOST_AUTO_TEST_CASE(cancellation_crossing_rename_reconciles_actual_publication_and_can_resume) {
   auto env = fixture{};
   const auto bytes = data(core::block_size + 3U);
   const auto manifest = describe({bytes});
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, {bytes});
   auto fault = faults::scope{{.pause_published_parent = true}};
   auto loop = concurrent_io{};
   auto stop = std::stop_source{};
   auto publish = loop.start(store->async_publish(stop.get_token()));
   BOOST_REQUIRE_MESSAGE(fault.wait_paused(), "NOT_PROVEN: post-rename parent fsync was not intercepted");
   BOOST_CHECK(read_file(env.root / "result") == bytes);
   stop.request_stop();
   fault.release();
   BOOST_CHECK_THROW(take(publish), forge::asio::exceptions::canceled);
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(!fault.snapshot().timed_out);
   fault.disarm();
   run(store->async_close());
   store.reset();
   store = run(storage::file_store::async_open(env.options(manifest, storage::open_mode::resume)));
   BOOST_CHECK(store->snapshot().published);
   BOOST_CHECK(run(store->async_read({0, 0}, 0, bytes.size())) == bytes);
   run(store->async_close());
}

BOOST_AUTO_TEST_CASE(directory_created_at_the_actual_rename_boundary_is_never_overwritten_or_merged) {
   BOOST_REQUIRE_MESSAGE(faults::rename_available(),
      "NOT_PROVEN: Linux raw renameat2 syscall is outside this executable's libc interposition");
   auto env = fixture{};
   const auto files = std::vector<std::vector<std::uint8_t>>{data(17), data(core::block_size + 3U, 19)};
   const auto manifest = describe(files);
   auto store = run(storage::file_store::async_open(env.options(manifest)));
   fill(*store, manifest, files);
   const auto stage = env.stage();
   auto fault = faults::scope{{.pause_publish_rename = true}};
   auto loop = concurrent_io{};
   auto publication = loop.start(store->async_publish());
   BOOST_REQUIRE_MESSAGE(fault.wait_paused(), "NOT_PROVEN: publication rename barrier was not reached");
   const auto foreign = data(29, 71);
   BOOST_REQUIRE(std::filesystem::create_directory(env.root / "result"));
   write_file(env.root / "result/foreign", foreign);
   fault.release();
   BOOST_CHECK_THROW(take(publication), storage::exceptions::conflict);
   BOOST_CHECK_EQUAL(fault.snapshot().renames, 1U);
   BOOST_CHECK(!fault.snapshot().timed_out);
   BOOST_CHECK(!store->snapshot().published);
   BOOST_CHECK(read_file(env.root / "result/foreign") == foreign);
   for (auto file = 0U; file < files.size(); ++file) {
      const auto relative = std::filesystem::path{"dir" + std::to_string(file)} / "file";
      BOOST_CHECK(read_file(stage / relative) == files[file]);
      BOOST_CHECK(!std::filesystem::exists(env.root / "result" / relative));
   }
   fault.disarm();
   run(store->async_close());
}

BOOST_AUTO_TEST_SUITE_END()
