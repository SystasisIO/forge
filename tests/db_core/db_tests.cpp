#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.db.core.driver;
import forge.db.core.exceptions;
import forge.db.core.record;

namespace {

static_assert(static_cast<unsigned>(forge::db::core::mutation_policy::forbidden) == 3U);
static_assert(static_cast<unsigned>(forge::db::core::mutation_policy::forbidden_when_captured) == 4U);

using record_map = std::map<forge::db::core::record_key, std::vector<std::byte>>;
using family_map = std::map<std::string, record_map>;

std::vector<std::byte> bytes(std::string text) {
   return std::vector<std::byte>{reinterpret_cast<const std::byte*>(text.data()),
                                 reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

std::string text(const std::vector<std::byte>& bytes_value) {
   return std::string{reinterpret_cast<const char*>(bytes_value.data()),
                      reinterpret_cast<const char*>(bytes_value.data() + bytes_value.size())};
}

forge::db::core::record_key key(std::string text_value) {
   return forge::db::core::record_key{bytes(std::move(text_value))};
}

bool starts_with(const forge::db::core::record_key& value, const forge::db::core::record_key& prefix) {
   const auto& bytes_value = value.bytes();
   const auto& prefix_value = prefix.bytes();
   return prefix_value.empty() || (bytes_value.size() >= prefix_value.size() &&
                                   std::equal(prefix_value.begin(), prefix_value.end(), bytes_value.begin()));
}

struct memory_state {
   family_map records;
   bool fail_commit = false;
   bool fail_rollback = false;
   bool fail_close = false;
   std::atomic_bool block_open = false;
   std::atomic_bool open_started = false;
   std::atomic_bool release_open = false;
   std::atomic_bool block_close = false;
   std::atomic_bool close_started = false;
   std::atomic_bool release_close = false;
   std::atomic_bool block_flush = false;
   std::atomic_bool flush_started = false;
   std::atomic_bool release_flush = false;
   std::size_t close_calls = 0;
   std::size_t rollback_calls = 0;
   std::size_t destroyed_sessions = 0;
   bool support_savepoints = true;
   bool support_record_locks = true;
   std::vector<std::pair<std::string, forge::db::core::record_key>> lock_requests;
   std::vector<std::string> events;
};

class memory_session final : public forge::db::core::session {
 public:
   memory_session(std::shared_ptr<memory_state> state, bool snapshot, bool writable)
       : state_{std::move(state)}, working_{state_->records}, snapshot_{snapshot}, writable_{writable} {}

   ~memory_session() override {
      ++state_->destroyed_sessions;
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{
          .snapshot_reads = snapshot_,
          .writes = writable_,
          .savepoints = writable_ && state_->support_savepoints,
          .record_locks = writable_ && state_->support_record_locks,
      };
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family family,
                                                                     forge::db::core::record_key record) override {
      const auto family_found = working_.find(family.name);
      if (family_found == working_.end()) {
         co_return std::nullopt;
      }
      const auto found = family_found->second.find(record);
      if (found == family_found->second.end()) {
         co_return std::nullopt;
      }
      co_return found->second;
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(forge::db::core::family family, forge::db::core::record_key record) override {
      state_->events.push_back("lock:" + family.name + ":" + text(record.bytes()));
      state_->lock_requests.emplace_back(family.name, record);
      co_return co_await get(std::move(family), std::move(record));
   }

   boost::asio::awaitable<void> put(forge::db::core::family family, forge::db::core::record_key record,
                                    std::vector<std::byte> value) override {
      working_[family.name][std::move(record)] = std::move(value);
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family family, forge::db::core::record_key record) override {
      working_[family.name].erase(record);
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::family family,
                                                                  forge::db::core::record_range range,
                                                                  forge::db::core::page_request request) override {
      forge::db::core::validate_page_request(request);
      auto result = forge::db::core::record_page{};
      const auto family_found = working_.find(family.name);
      if (family_found == working_.end()) {
         co_return result;
      }

      auto current = family_found->second.lower_bound(request.after ? request.after->boundary : range.begin);
      if (request.after && current != family_found->second.end() && current->first == request.after->boundary) {
         ++current;
      }

      auto last = std::optional<forge::db::core::record_key>{};
      while (current != family_found->second.end()) {
         if (!starts_with(current->first, range.prefix)) {
            break;
         }
         if (range.has_end && !(current->first.bytes() < range.end.bytes())) {
            break;
         }
         result.entries.push_back(forge::db::core::record_entry{.key = current->first, .value = current->second});
         last = current->first;
         ++current;
         if (result.entries.size() == request.limit) {
            break;
         }
      }

      if (last && current != family_found->second.end() && starts_with(current->first, range.prefix) &&
          (!range.has_end || current->first.bytes() < range.end.bytes())) {
         result.next = forge::db::core::cursor{.boundary = *last};
      }
      co_return result;
   }

   boost::asio::awaitable<void> create_savepoint() override {
      state_->events.push_back("savepoint");
      savepoints_.push_back(working_);
      co_return;
   }

   boost::asio::awaitable<void> rollback_to_savepoint() override {
      if (savepoints_.empty()) {
         throw std::logic_error{"db test savepoint stack is empty"};
      }
      working_ = std::move(savepoints_.back());
      savepoints_.pop_back();
      co_return;
   }

   boost::asio::awaitable<void> release_savepoint() override {
      if (savepoints_.empty()) {
         throw std::logic_error{"db test savepoint stack is empty"};
      }
      savepoints_.pop_back();
      co_return;
   }

   boost::asio::awaitable<void> commit() override {
      if (state_->fail_commit) {
         throw std::runtime_error{"db test commit failure"};
      }
      if (writable_) {
         state_->records = std::move(working_);
      }
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      ++state_->rollback_calls;
      if (state_->fail_rollback) {
         throw std::runtime_error{"db test rollback failure"};
      }
      co_return;
   }

 private:
   std::shared_ptr<memory_state> state_;
   family_map working_;
   std::vector<family_map> savepoints_;
   bool snapshot_ = false;
   bool writable_ = false;
};

class tracking_participant final : public forge::db::core::transaction_participant {
 public:
   [[nodiscard]] std::string_view name() const noexcept override {
      return "db-test-tracking";
   }

   [[nodiscard]] bool captures_mutations() const noexcept override {
      return true;
   }

   boost::asio::awaitable<void> prepare_mutation(const forge::db::core::record_mutation& mutation) override {
      pending_ = mutation;
      co_return;
   }

   void publish_mutation() noexcept override {
      captured_.push_back(std::move(*pending_));
      pending_.reset();
   }

   void discard_mutation() noexcept override {
      pending_.reset();
   }

   boost::asio::awaitable<void> prepare_savepoint(forge::db::core::savepoint_id_t) override {
      pending_frame_ = captured_.size();
      co_return;
   }

   void publish_savepoint(forge::db::core::savepoint_id_t) noexcept override {
      frames_.push_back(*pending_frame_);
      pending_frame_.reset();
   }

   void discard_savepoint(forge::db::core::savepoint_id_t) noexcept override {
      pending_frame_.reset();
   }

   boost::asio::awaitable<void> rollback_to_savepoint(forge::db::core::savepoint_id_t,
                                                      forge::db::core::participant_access&) override {
      captured_.resize(frames_.back());
      frames_.pop_back();
      if (fail_restore) {
         throw std::runtime_error{"db test participant restore failure"};
      }
      co_return;
   }

   boost::asio::awaitable<void> release_savepoint(forge::db::core::savepoint_id_t,
                                                  forge::db::core::participant_access&) override {
      frames_.pop_back();
      co_return;
   }

   boost::asio::awaitable<void> prepare_commit(forge::db::core::participant_access&) override {
      prepared = true;
      co_return;
   }

   [[nodiscard]] std::size_t captured() const noexcept {
      return captured_.size();
   }

   bool fail_restore = false;
   bool prepared = false;

 private:
   std::optional<forge::db::core::record_mutation> pending_;
   std::optional<std::size_t> pending_frame_;
   std::vector<std::size_t> frames_;
   std::vector<forge::db::core::record_mutation> captured_;
};

class claiming_participant final : public forge::db::core::transaction_participant {
 public:
   explicit claiming_participant(std::string name, std::vector<forge::db::core::family> families = {})
       : name_{std::move(name)}, families_{std::move(families)} {}

   [[nodiscard]] std::string_view name() const noexcept override {
      return name_;
   }

   [[nodiscard]] std::span<const forge::db::core::family> exclusive_families() const noexcept override {
      return families_;
   }

 private:
   std::string name_;
   std::vector<forge::db::core::family> families_;
};

class unclaimed_participant final : public forge::db::core::transaction_participant {
 public:
   explicit unclaimed_participant(std::string name) : name_{std::move(name)} {}

   [[nodiscard]] std::string_view name() const noexcept override {
      return name_;
   }

 private:
   std::string name_;
};

class locking_participant final : public forge::db::core::transaction_participant {
 public:
   locking_participant(std::string name, std::vector<forge::db::core::record_lock_claim> locks)
       : name_{std::move(name)}, locks_{std::move(locks)} {}

   [[nodiscard]] std::string_view name() const noexcept override {
      return name_;
   }

   [[nodiscard]] std::span<const forge::db::core::record_lock_claim> prewrite_locks() const noexcept override {
      return locks_;
   }

 private:
   std::string name_;
   std::vector<forge::db::core::record_lock_claim> locks_;
};

class policy_participant final : public forge::db::core::transaction_participant {
 public:
   policy_participant(std::string name, forge::db::core::family protected_family,
                      forge::db::core::mutation_policy policy)
       : name_{std::move(name)}, protected_family_{std::move(protected_family)}, policy_{policy} {}

   [[nodiscard]] std::string_view name() const noexcept override {
      return name_;
   }

   [[nodiscard]] forge::db::core::mutation_policy classify(const forge::db::core::family& family,
                                                           const forge::db::core::record_key&,
                                                           forge::db::core::mutation_kind) const noexcept override {
      return family.name == protected_family_.name ? policy_ : forge::db::core::mutation_policy::inherit;
   }

 private:
   std::string name_;
   forge::db::core::family protected_family_;
   forge::db::core::mutation_policy policy_;
};

class memory_driver final : public forge::db::core::driver {
 public:
   explicit memory_driver(std::shared_ptr<memory_state> state) : state_{std::move(state)} {}

   boost::asio::awaitable<void> async_flush(bool) override {
      auto admission = admit_operation();
      if (state_->block_flush.load(std::memory_order_acquire)) {
         state_->flush_started.store(true, std::memory_order_release);
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         while (!state_->release_flush.load(std::memory_order_acquire)) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      }
      co_return;
   }

 private:
   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_transaction() override {
      if (state_->block_open.load(std::memory_order_acquire)) {
         state_->open_started.store(true, std::memory_order_release);
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         while (!state_->release_open.load(std::memory_order_acquire)) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      }
      co_return std::make_unique<memory_session>(state_, false, true);
   }

   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_snapshot() override {
      co_return std::make_unique<memory_session>(state_, true, false);
   }

   boost::asio::awaitable<void> close_driver() override {
      ++state_->close_calls;
      if (state_->block_close.load(std::memory_order_acquire)) {
         state_->close_started.store(true, std::memory_order_release);
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         while (!state_->release_close.load(std::memory_order_acquire)) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      }
      if (state_->fail_close) {
         throw std::runtime_error{"db test close failure"};
      }
      co_return;
   }

   std::shared_ptr<memory_state> state_;
};

} // namespace

BOOST_AUTO_TEST_SUITE(db_test_suite)

BOOST_AUTO_TEST_CASE(db_driver_close_is_idempotent_and_rejects_new_sessions) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      co_await driver->async_close();
      co_await driver->async_close();

      BOOST_CHECK_EQUAL(state->close_calls, 1U);
      BOOST_CHECK_THROW(co_await driver->begin_transaction(), forge::db::core::exceptions::driver_closed);
      BOOST_CHECK_THROW(co_await driver->begin_read(), forge::db::core::exceptions::driver_closed);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_driver_close_with_active_sessions_is_retryable) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto transaction = co_await driver->begin_transaction();
      auto snapshot = co_await driver->begin_read();
      auto snapshot_copy = snapshot;

      BOOST_CHECK_THROW(co_await driver->async_close(), forge::db::core::exceptions::driver_busy);
      BOOST_CHECK_THROW(co_await driver->begin_transaction(), forge::db::core::exceptions::driver_closed);

      const auto records = forge::db::core::family{"records"};
      co_await transaction.put(records, key("active"), bytes("usable"));
      co_await transaction.rollback();
      snapshot = {};
      BOOST_CHECK_THROW(co_await driver->async_close(), forge::db::core::exceptions::driver_busy);
      snapshot_copy = {};

      co_await driver->async_close();
      BOOST_CHECK_EQUAL(state->close_calls, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_driver_close_rejects_opening_session_without_invalidating_it) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->block_open.store(true, std::memory_order_release);
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto opened = std::make_shared<std::optional<forge::db::core::transaction>>();
      auto completed = std::make_shared<std::atomic_bool>(false);
      auto failure = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [driver, opened, completed, failure]() -> boost::asio::awaitable<void> {
             try {
                opened->emplace(co_await driver->begin_transaction());
             } catch (...) {
                *failure = std::current_exception();
             }
             completed->store(true, std::memory_order_release);
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      while (!state->open_started.load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK_THROW(co_await driver->async_close(), forge::db::core::exceptions::driver_busy);
      state->release_open.store(true, std::memory_order_release);
      while (!completed->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK(!*failure);
      BOOST_REQUIRE(opened->has_value());
      co_await opened->value().rollback();
      opened->reset();
      co_await driver->async_close();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_driver_close_rejects_admitted_backend_operation) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->block_flush.store(true, std::memory_order_release);
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto completed = std::make_shared<std::atomic_bool>(false);
      auto failure = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [driver, completed, failure]() -> boost::asio::awaitable<void> {
             try {
                co_await driver->async_flush(true);
             } catch (...) {
                *failure = std::current_exception();
             }
             completed->store(true, std::memory_order_release);
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      while (!state->flush_started.load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK_THROW(co_await driver->async_close(), forge::db::core::exceptions::driver_busy);
      BOOST_CHECK_THROW(co_await driver->async_flush(true), forge::db::core::exceptions::driver_closed);

      state->release_flush.store(true, std::memory_order_release);
      while (!completed->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK(!*failure);
      co_await driver->async_close();
      BOOST_CHECK_EQUAL(state->close_calls, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_driver_concurrent_close_is_fail_fast) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->block_close.store(true, std::memory_order_release);
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto completed = std::make_shared<std::atomic_bool>(false);
      auto failure = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [driver, completed, failure]() -> boost::asio::awaitable<void> {
             try {
                co_await driver->async_close();
             } catch (...) {
                *failure = std::current_exception();
             }
             completed->store(true, std::memory_order_release);
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      while (!state->close_started.load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK_THROW(co_await driver->async_close(), forge::db::core::exceptions::driver_busy);
      state->release_close.store(true, std::memory_order_release);
      while (!completed->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK(!*failure);
      BOOST_CHECK_EQUAL(state->close_calls, 1U);
      co_await driver->async_close();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_driver_close_failure_keeps_close_retryable) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->fail_close = true;
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      BOOST_CHECK_THROW(co_await driver->async_close(), std::runtime_error);
      BOOST_CHECK_THROW(co_await driver->begin_read(), forge::db::core::exceptions::driver_closed);

      state->fail_close = false;
      co_await driver->async_close();
      BOOST_CHECK_EQUAL(state->close_calls, 2U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_savepoint_rolls_back_suffix_and_remains_active) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("a"), bytes("kept"));
      const auto point = co_await tx.create_savepoint();
      co_await tx.put(meta, key("b"), bytes("discarded"));

      co_await tx.rollback_to_savepoint(point);
      BOOST_CHECK(tx.active());
      BOOST_CHECK(!(co_await tx.get(meta, key("b"))).has_value());
      co_await tx.put(meta, key("c"), bytes("continued"));
      co_await tx.commit();

      auto read = co_await driver->begin_read();
      BOOST_CHECK_EQUAL(text(*(co_await read.get(meta, key("a")))), "kept");
      BOOST_CHECK(!(co_await read.get(meta, key("b"))).has_value());
      BOOST_CHECK_EQUAL(text(*(co_await read.get(meta, key("c")))), "continued");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_nested_savepoints_enforce_lifo_and_release_semantics) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto tx = co_await driver->begin_transaction();
      const auto outer = co_await tx.create_savepoint();
      co_await tx.put(meta, key("a"), bytes("outer"));
      const auto inner = co_await tx.create_savepoint();
      co_await tx.put(meta, key("b"), bytes("inner"));

      BOOST_CHECK_THROW(co_await tx.rollback_to_savepoint(outer), forge::db::core::exceptions::invalid_savepoint);
      co_await tx.release_savepoint(inner);
      BOOST_CHECK_THROW(co_await tx.release_savepoint(inner), forge::db::core::exceptions::invalid_savepoint);
      co_await tx.rollback_to_savepoint(outer);
      co_await tx.commit();

      auto read = co_await driver->begin_read();
      BOOST_CHECK(!(co_await read.get(meta, key("a"))).has_value());
      BOOST_CHECK(!(co_await read.get(meta, key("b"))).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_savepoint_restores_participant_and_prepares_commit) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto participant = std::make_shared<tracking_participant>();
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(participant);
      co_await tx.put(meta, key("a"), bytes("kept"));
      const auto point = co_await tx.create_savepoint();
      co_await tx.put(meta, key("b"), bytes("discarded"));
      BOOST_CHECK_EQUAL(participant->captured(), 2U);

      co_await tx.rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(participant->captured(), 1U);
      co_await tx.commit();
      BOOST_CHECK(participant->prepared);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_participants_reject_overlapping_exclusive_families) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(std::make_shared<claiming_participant>(
          "first", std::vector{forge::db::core::family{"one"}, forge::db::core::family{"shared"}}));
      tx.attach_participant(
          std::make_shared<claiming_participant>("independent", std::vector{forge::db::core::family{"two"}}));

      try {
         tx.attach_participant(
             std::make_shared<claiming_participant>("overlapping", std::vector{forge::db::core::family{"shared"}}));
         BOOST_FAIL("overlapping participant family was accepted");
      } catch (const forge::db::core::exceptions::participant_conflict& error) {
         const auto context_value = [&error](std::string_view key_value) -> std::string_view {
            const auto& context = error.context();
            const auto field =
                std::find_if(context.begin(), context.end(), [&](const auto& value) { return value.key == key_value; });
            return field == context.end() ? std::string_view{} : std::string_view{field->value};
         };
         BOOST_CHECK_EQUAL(context_value("family"), "shared");
         BOOST_CHECK_EQUAL(context_value("participant"), "overlapping");
         BOOST_CHECK_EQUAL(context_value("existing-participant"), "first");
      }

      tx.attach_participant(
          std::make_shared<claiming_participant>("still-independent", std::vector{forge::db::core::family{"three"}}));
      co_await tx.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_participant_claims_preserve_default_and_name_duplicate_behavior) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(std::make_shared<unclaimed_participant>("unclaimed-first"));
      tx.attach_participant(std::make_shared<unclaimed_participant>("unclaimed-second"));

      BOOST_CHECK_THROW(tx.attach_participant(std::make_shared<unclaimed_participant>("unclaimed-first")),
                        forge::db::core::exceptions::participant_conflict);
      co_await tx.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_prewrite_locks_are_canonical_and_precede_mutation) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(std::make_shared<locking_participant>(
          "locks-z", std::vector<forge::db::core::record_lock_claim>{
                         {.column_family = forge::db::core::family{"z"}, .key = key("b")},
                         {.column_family = forge::db::core::family{"a"}, .key = key("c")},
                     }));
      tx.attach_participant(std::make_shared<locking_participant>(
          "locks-a", std::vector<forge::db::core::record_lock_claim>{
                         {.column_family = forge::db::core::family{"a"}, .key = key("a")},
                     }));

      co_await tx.put(forge::db::core::family{"data"}, key("value"), bytes("stored"));
      co_await tx.put(forge::db::core::family{"data"}, key("other"), bytes("stored"));

      BOOST_REQUIRE_EQUAL(state->lock_requests.size(), 3U);
      BOOST_CHECK_EQUAL(state->lock_requests[0].first, "a");
      BOOST_CHECK_EQUAL(text(state->lock_requests[0].second.bytes()), "a");
      BOOST_CHECK_EQUAL(state->lock_requests[1].first, "a");
      BOOST_CHECK_EQUAL(text(state->lock_requests[1].second.bytes()), "c");
      BOOST_CHECK_EQUAL(state->lock_requests[2].first, "z");
      BOOST_CHECK_EQUAL(text(state->lock_requests[2].second.bytes()), "b");
      BOOST_CHECK_THROW(tx.attach_participant(std::make_shared<unclaimed_participant>("too-late")),
                        forge::db::core::exceptions::participant_conflict);
      co_await tx.rollback();

      auto prepared = co_await driver->begin_transaction();
      prepared.attach_participant(std::make_shared<locking_participant>(
          "prepared-lock", std::vector<forge::db::core::record_lock_claim>{
                               {.column_family = forge::db::core::family{"a"}, .key = key("a")},
                           }));
      static_cast<void>(co_await prepared.get_for_update(forge::db::core::family{"a"}, key("a")));
      prepared.attach_participant(std::make_shared<unclaimed_participant>("late-observer"));
      BOOST_CHECK_THROW(prepared.attach_participant(std::make_shared<locking_participant>(
                            "late-lock",
                            std::vector<forge::db::core::record_lock_claim>{
                                {.column_family = forge::db::core::family{"b"}, .key = key("b")},
                            })),
                        forge::db::core::exceptions::participant_conflict);
      co_await prepared.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_prewrite_locks_precede_native_savepoint) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(std::make_shared<locking_participant>(
          "savepoint-lock", std::vector<forge::db::core::record_lock_claim>{
                                {.column_family = forge::db::core::family{"objectdb"}, .key = key("coordinator")},
                            }));

      const auto point = co_await tx.create_savepoint();
      BOOST_REQUIRE_EQUAL(state->events.size(), 2U);
      BOOST_CHECK_EQUAL(state->events[0], "lock:objectdb:coordinator");
      BOOST_CHECK_EQUAL(state->events[1], "savepoint");

      co_await tx.rollback_to_savepoint(point);
      co_await tx.put(forge::db::core::family{"objectdb"}, key("value"), bytes("stored"));
      BOOST_CHECK_EQUAL(state->lock_requests.size(), 1U);
      co_await tx.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_forbidden_policy_blocks_without_capture) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto protected_family = forge::db::core::family{"protected"};
      const auto allowed_family = forge::db::core::family{"allowed"};

      auto seed = co_await driver->begin_transaction();
      co_await seed.put(protected_family, key("existing"), bytes("seed"));
      co_await seed.commit();

      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(std::make_shared<policy_participant>("forbidden", protected_family,
                                                                 forge::db::core::mutation_policy::forbidden));

      BOOST_CHECK_THROW(co_await tx.put(protected_family, key("new"), bytes("blocked")),
                        forge::db::core::exceptions::mutation_forbidden);
      BOOST_CHECK_THROW(co_await tx.erase(protected_family, key("existing")),
                        forge::db::core::exceptions::mutation_forbidden);

      BOOST_CHECK(tx.active());
      BOOST_CHECK(!(co_await tx.get(protected_family, key("new"))).has_value());
      BOOST_CHECK_EQUAL(text(*(co_await tx.get(protected_family, key("existing")))), "seed");
      co_await tx.put(allowed_family, key("continued"), bytes("yes"));
      co_await tx.commit();

      auto read = co_await driver->begin_read();
      BOOST_CHECK(!(co_await read.get(protected_family, key("new"))).has_value());
      BOOST_CHECK_EQUAL(text(*(co_await read.get(protected_family, key("existing")))), "seed");
      BOOST_CHECK_EQUAL(text(*(co_await read.get(allowed_family, key("continued")))), "yes");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_capture_forbidden_policy_only_blocks_active_capture) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto protected_family = forge::db::core::family{"protected"};
      const auto allowed_family = forge::db::core::family{"allowed"};

      auto seed = co_await driver->begin_transaction();
      co_await seed.put(protected_family, key("erasable"), bytes("seed"));
      co_await seed.commit();

      auto uncaptured = co_await driver->begin_transaction();
      uncaptured.attach_participant(std::make_shared<policy_participant>(
          "capture-policy-only", protected_family, forge::db::core::mutation_policy::forbidden_when_captured));
      BOOST_CHECK(!uncaptured.captures_mutations());
      co_await uncaptured.put(protected_family, key("permitted"), bytes("outside capture"));
      co_await uncaptured.erase(protected_family, key("erasable"));
      co_await uncaptured.commit();

      auto capturing = co_await driver->begin_transaction();
      capturing.attach_participant(std::make_shared<policy_participant>(
          "capture-policy", protected_family, forge::db::core::mutation_policy::forbidden_when_captured));
      auto tracker = std::make_shared<tracking_participant>();
      capturing.attach_participant(tracker);
      BOOST_CHECK(capturing.captures_mutations());

      BOOST_CHECK_THROW(co_await capturing.put(protected_family, key("blocked"), bytes("captured")),
                        forge::db::core::exceptions::mutation_forbidden);
      BOOST_CHECK_THROW(co_await capturing.erase(protected_family, key("permitted")),
                        forge::db::core::exceptions::mutation_forbidden);
      BOOST_CHECK_EQUAL(tracker->captured(), 0U);
      BOOST_CHECK(capturing.active());
      co_await capturing.put(allowed_family, key("continued"), bytes("yes"));
      BOOST_CHECK_EQUAL(tracker->captured(), 1U);
      co_await capturing.commit();
      BOOST_CHECK(!capturing.captures_mutations());

      auto read = co_await driver->begin_read();
      BOOST_CHECK_EQUAL(text(*(co_await read.get(protected_family, key("permitted")))), "outside capture");
      BOOST_CHECK(!(co_await read.get(protected_family, key("erasable"))).has_value());
      BOOST_CHECK(!(co_await read.get(protected_family, key("blocked"))).has_value());
      BOOST_CHECK_EQUAL(text(*(co_await read.get(allowed_family, key("continued")))), "yes");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_reports_claimed_families) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto claimed = forge::db::core::family{"claimed"};
      auto tx = co_await driver->begin_transaction();
      BOOST_CHECK(!tx.claims_family(claimed));

      tx.attach_participant(std::make_shared<claiming_participant>("owner", std::vector{claimed}));
      BOOST_CHECK(tx.claims_family(claimed));
      BOOST_CHECK(!tx.claims_family(forge::db::core::family{"other"}));

      co_await tx.rollback();
      BOOST_CHECK(!tx.claims_family(claimed));
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_participant_restore_failure_marks_rollback_only) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto participant = std::make_shared<tracking_participant>();
      auto tx = co_await driver->begin_transaction();
      tx.attach_participant(participant);
      const auto point = co_await tx.create_savepoint();
      co_await tx.put(meta, key("a"), bytes("discarded"));
      participant->fail_restore = true;

      BOOST_CHECK_THROW(co_await tx.rollback_to_savepoint(point), std::runtime_error);
      BOOST_CHECK_THROW(co_await tx.put(meta, key("b"), bytes("rejected")),
                        forge::db::core::exceptions::transaction_rollback_only);
      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::core::exceptions::transaction_rollback_only);
      co_await tx.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_savepoint_requires_backend_capability) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->support_savepoints = false;
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      BOOST_CHECK_THROW(co_await tx.create_savepoint(), forge::db::core::exceptions::unsupported_operation);
      co_await tx.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_commit_and_rollback_are_atomic) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};

      {
         auto tx = co_await driver->begin_transaction();
         co_await tx.put(meta, key("a"), bytes("rollback"));
         co_await tx.rollback();
      }
      {
         auto read = co_await driver->begin_read();
         BOOST_CHECK(!(co_await read.get(meta, key("a"))).has_value());
      }

      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("a"), bytes("commit"));
      co_await tx.commit();

      auto read = co_await driver->begin_read();
      const auto value = co_await read.get(meta, key("a"));
      BOOST_REQUIRE(value.has_value());
      BOOST_CHECK_EQUAL(text(*value), "commit");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_snapshot_reads_preserve_precommit_state) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};

      auto initial = co_await driver->begin_transaction();
      co_await initial.put(meta, key("a"), bytes("old"));
      co_await initial.commit();

      auto snapshot = co_await driver->begin_read();
      auto update = co_await driver->begin_transaction();
      co_await update.put(meta, key("a"), bytes("new"));
      co_await update.commit();

      BOOST_CHECK_EQUAL(text(*(co_await snapshot.get(meta, key("a")))), "old");
      auto after = co_await driver->begin_read();
      BOOST_CHECK_EQUAL(text(*(co_await after.get(meta, key("a")))), "new");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_snapshot_origin_is_stable_across_copies_and_driver_scoped) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);
   auto foreign = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto active = co_await driver->begin_read();
      auto copy = active;
      BOOST_CHECK(active.belongs_to(*driver));
      BOOST_CHECK(copy.belongs_to(*driver));
      BOOST_CHECK(!active.belongs_to(*foreign));

      auto unbound = forge::db::core::snapshot{std::make_unique<memory_session>(state, true, false)};
      BOOST_CHECK(unbound.active());
      BOOST_CHECK(!unbound.belongs_to(*driver));
      BOOST_CHECK(!unbound.belongs_to(*foreign));
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_snapshot_session_lives_until_last_copy_is_released) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto active = co_await driver->begin_read();
      auto copy = active;
      active = {};
      BOOST_CHECK_EQUAL(state->destroyed_sessions, 0U);

      copy = {};
      BOOST_CHECK_EQUAL(state->destroyed_sessions, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_origin_is_driver_scoped_moves_and_does_not_grant_manual_sessions) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);
   auto foreign = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto active = co_await driver->begin_transaction();
      BOOST_CHECK(active.active());
      BOOST_CHECK(active.belongs_to(*driver));
      BOOST_CHECK(!active.belongs_to(*foreign));
      auto moved = std::move(active);
      BOOST_CHECK(!active.belongs_to(*driver));
      BOOST_CHECK(moved.belongs_to(*driver));
      active = std::move(moved);
      BOOST_CHECK(!moved.belongs_to(*driver));
      BOOST_CHECK(active.belongs_to(*driver));
      co_await active.rollback();
      BOOST_CHECK(!active.active());
      // Identity and active lifecycle are independent, just as for snapshots.
      BOOST_CHECK(active.belongs_to(*driver));

      auto manual = forge::db::core::transaction{
          std::make_unique<memory_session>(state, false, true), co_await boost::asio::this_coro::executor};
      BOOST_CHECK(manual.active());
      BOOST_CHECK(!manual.belongs_to(*driver));
      BOOST_CHECK(!manual.belongs_to(*foreign));
      co_await manual.rollback();
      auto empty = forge::db::core::transaction{};
      BOOST_CHECK(!empty.belongs_to(*driver));
   }());
}

BOOST_AUTO_TEST_CASE(db_snapshot_copies_support_parallel_reads) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("a"), bytes("first"));
      co_await tx.put(meta, key("b"), bytes("second"));
      co_await tx.commit();

      auto first = co_await driver->begin_read();
      auto second = first;
      auto completed = std::make_shared<std::atomic_size_t>(0U);
      auto first_value = std::make_shared<std::optional<std::vector<std::byte>>>();
      auto second_value = std::make_shared<std::optional<std::vector<std::byte>>>();
      auto first_error = std::make_shared<std::exception_ptr>();
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;

      boost::asio::co_spawn(
          executor,
          [first = std::move(first), meta, first_value, first_error,
           completed]() mutable -> boost::asio::awaitable<void> {
             try {
                *first_value = co_await first.get(meta, key("a"));
             } catch (...) {
                *first_error = std::current_exception();
             }
             completed->fetch_add(1U, std::memory_order_release);
          },
          boost::asio::detached);
      boost::asio::co_spawn(
          executor,
          [second = std::move(second), meta, second_value, second_error,
           completed]() mutable -> boost::asio::awaitable<void> {
             try {
                *second_value = co_await second.get(meta, key("b"));
             } catch (...) {
                *second_error = std::current_exception();
             }
             completed->fetch_add(1U, std::memory_order_release);
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      while (completed->load(std::memory_order_acquire) != 2U) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*first_error) {
         std::rethrow_exception(*first_error);
      }
      if (*second_error) {
         std::rethrow_exception(*second_error);
      }

      BOOST_REQUIRE(first_value->has_value());
      BOOST_REQUIRE(second_value->has_value());
      BOOST_CHECK_EQUAL(text(**first_value), "first");
      BOOST_CHECK_EQUAL(text(**second_value), "second");
   }());
}

BOOST_AUTO_TEST_CASE(db_scan_pages_use_opaque_cursor_boundaries) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("a"), bytes("1"));
      co_await tx.put(meta, key("b"), bytes("2"));
      co_await tx.put(meta, key("c"), bytes("3"));
      co_await tx.commit();

      auto snapshot = co_await driver->begin_read();
      auto first = co_await snapshot.scan_page(meta, forge::db::core::record_range{.begin = key("a"), .end = key("z")},
                                               {.limit = 2});
      BOOST_REQUIRE_EQUAL(first.entries.size(), 2U);
      BOOST_REQUIRE(first.next.has_value());
      BOOST_CHECK_EQUAL(text(first.entries[0].value), "1");
      BOOST_CHECK_EQUAL(text(first.entries[1].value), "2");

      auto second = co_await snapshot.scan_page(meta, forge::db::core::record_range{.begin = key("a"), .end = key("z")},
                                                forge::db::core::page_request{.after = first.next, .limit = 2});
      BOOST_REQUIRE_EQUAL(second.entries.size(), 1U);
      BOOST_CHECK_EQUAL(text(second.entries[0].value), "3");
      BOOST_CHECK(!second.next.has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_before_commit_hook_reads_writes_and_commits_active_transaction) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("source"), bytes("visible"));
      tx.before_commit([&tx, meta]() -> boost::asio::awaitable<void> {
         BOOST_CHECK(tx.active());
         const auto source = co_await tx.get(meta, key("source"));
         BOOST_REQUIRE(source.has_value());
         BOOST_CHECK_EQUAL(text(*source), "visible");
         co_await tx.put(meta, key("hook"), bytes("committed"));
         co_return;
      });

      co_await tx.commit();

      auto read = co_await driver->begin_read();
      const auto hook_value = co_await read.get(meta, key("hook"));
      BOOST_REQUIRE(hook_value.has_value());
      BOOST_CHECK_EQUAL(text(*hook_value), "committed");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_before_commit_hook_failure_is_rollback_only_and_explicitly_rolls_back) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto rollback_hook_called = false;
      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("discarded"), bytes("value"));
      tx.before_commit([]() -> boost::asio::awaitable<void> {
         throw std::runtime_error{"db test before-commit hook failure"};
         co_return;
      });
      tx.after_rollback([&]() -> boost::asio::awaitable<void> {
         rollback_hook_called = true;
         co_return;
      });

      BOOST_CHECK_THROW(co_await tx.commit(), std::runtime_error);
      BOOST_CHECK(tx.active());
      BOOST_CHECK_THROW(co_await tx.get(meta, key("discarded")),
                        forge::db::core::exceptions::transaction_rollback_only);
      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::core::exceptions::transaction_rollback_only);
      co_await tx.rollback();

      BOOST_CHECK(!tx.active());
      BOOST_CHECK(rollback_hook_called);
      BOOST_CHECK_EQUAL(state->rollback_calls, 1U);
      auto read = co_await driver->begin_read();
      BOOST_CHECK(!(co_await read.get(meta, key("discarded"))).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_before_commit_hook_rejects_recursive_completion_and_late_attachment) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto commit_rejected = false;
      auto rollback_rejected = false;
      auto hook_rejected = false;
      auto participant_rejected = false;
      auto late_hook_called = false;
      auto tx = co_await driver->begin_transaction();
      tx.before_commit([&]() -> boost::asio::awaitable<void> {
         try {
            co_await tx.commit();
         } catch (const forge::db::core::exceptions::participant_conflict&) {
            commit_rejected = true;
         }
         try {
            co_await tx.rollback();
         } catch (const forge::db::core::exceptions::participant_conflict&) {
            rollback_rejected = true;
         }
         try {
            tx.before_commit([&late_hook_called]() -> boost::asio::awaitable<void> {
               late_hook_called = true;
               co_return;
            });
         } catch (const forge::db::core::exceptions::participant_conflict&) {
            hook_rejected = true;
         }
         try {
            tx.attach_participant(std::make_shared<unclaimed_participant>("late-before-commit"));
         } catch (const forge::db::core::exceptions::participant_conflict&) {
            participant_rejected = true;
         }
         co_return;
      });

      co_await tx.commit();

      BOOST_CHECK(commit_rejected);
      BOOST_CHECK(rollback_rejected);
      BOOST_CHECK(hook_rejected);
      BOOST_CHECK(participant_rejected);
      BOOST_CHECK(!late_hook_called);
      BOOST_CHECK(!tx.active());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_completion_hooks_reject_default_and_closed_transactions) {
   auto no_op = []() -> boost::asio::awaitable<void> { co_return; };
   auto empty = forge::db::core::transaction{};
   BOOST_CHECK_THROW(empty.after_commit(no_op), forge::db::core::exceptions::transaction_closed);
   BOOST_CHECK_THROW(empty.after_rollback(no_op), forge::db::core::exceptions::transaction_closed);

   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto closed = co_await driver->begin_transaction();
      co_await closed.rollback();

      BOOST_CHECK_THROW(closed.after_commit(no_op), forge::db::core::exceptions::transaction_closed);
      BOOST_CHECK_THROW(closed.after_rollback(no_op), forge::db::core::exceptions::transaction_closed);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_before_commit_hook_rejects_late_completion_hooks) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto tx = co_await driver->begin_transaction();
      tx.before_commit([&tx]() -> boost::asio::awaitable<void> {
         auto no_op = []() -> boost::asio::awaitable<void> { co_return; };
         BOOST_CHECK_THROW(tx.after_commit(no_op), forge::db::core::exceptions::participant_conflict);
         BOOST_CHECK_THROW(tx.after_rollback(no_op), forge::db::core::exceptions::participant_conflict);
         co_return;
      });

      co_await tx.commit();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_participant_hooks_follow_commit_and_rollback) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto commits = 0U;
      auto rollbacks = 0U;
      {
         auto tx = co_await driver->begin_transaction();
         tx.after_commit([&]() -> boost::asio::awaitable<void> {
            ++commits;
            co_return;
         });
         tx.after_rollback([&]() -> boost::asio::awaitable<void> {
            ++rollbacks;
            co_return;
         });
         co_await tx.commit();
      }
      {
         auto tx = co_await driver->begin_transaction();
         tx.after_commit([&]() -> boost::asio::awaitable<void> {
            ++commits;
            co_return;
         });
         tx.after_rollback([&]() -> boost::asio::awaitable<void> {
            ++rollbacks;
            co_return;
         });
         co_await tx.rollback();
      }

      BOOST_CHECK_EQUAL(commits, 1U);
      BOOST_CHECK_EQUAL(rollbacks, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_commit_hook_failure_keeps_commit_boundary_closed) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto rollback_hook_called = false;
      auto session_destroyed_before_commit_hook = false;

      auto tx = co_await driver->begin_transaction();
      co_await tx.put(meta, key("a"), bytes("committed"));
      tx.after_commit([&]() -> boost::asio::awaitable<void> {
         session_destroyed_before_commit_hook = state->destroyed_sessions == 1U;
         throw std::runtime_error{"db test commit hook failure"};
         co_return;
      });
      tx.after_rollback([&]() -> boost::asio::awaitable<void> {
         rollback_hook_called = true;
         co_return;
      });

      BOOST_CHECK_THROW(co_await tx.commit(), std::runtime_error);
      BOOST_CHECK(!tx.active());
      co_await tx.rollback();

      auto read = co_await driver->begin_read();
      const auto value = co_await read.get(meta, key("a"));
      BOOST_REQUIRE(value.has_value());
      BOOST_CHECK_EQUAL(text(*value), "committed");
      BOOST_CHECK(session_destroyed_before_commit_hook);
      BOOST_CHECK(!rollback_hook_called);
      BOOST_CHECK_EQUAL(state->rollback_calls, 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_transaction_awaits_async_rollback_hooks_before_returning) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>(std::make_shared<memory_state>());

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto hook_completed = false;

      auto tx = co_await driver->begin_transaction();
      tx.after_rollback([&]() -> boost::asio::awaitable<void> {
         auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
         hook_completed = true;
         co_return;
      });

      co_await tx.rollback();
      BOOST_CHECK(hook_completed);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_dropped_transaction_swallows_rollback_hook_failure) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto hook_called = false;

      {
         auto tx = co_await driver->begin_transaction();
         tx.after_rollback([&]() -> boost::asio::awaitable<void> {
            hook_called = true;
            throw std::runtime_error{"db test rollback hook failure"};
            co_return;
         });
      }

      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (auto attempt = 0; attempt != 100 && !hook_called; ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK(hook_called);
      BOOST_CHECK_EQUAL(state->rollback_calls, 1U);
      BOOST_CHECK_EQUAL(state->destroyed_sessions, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_dropped_transaction_runs_rollback_hooks_after_backend_rollback_failure) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->fail_rollback = true;
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto hook_called = false;

      {
         auto tx = co_await driver->begin_transaction();
         tx.after_rollback([&]() -> boost::asio::awaitable<void> {
            hook_called = true;
            co_return;
         });
      }

      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (auto attempt = 0; attempt != 100 && !hook_called; ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_CHECK(hook_called);
      BOOST_CHECK_EQUAL(state->rollback_calls, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_dropped_transaction_destroys_session_before_rollback_hooks) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto hook_called = false;
      auto session_destroyed_before_hook = false;

      {
         auto tx = co_await driver->begin_transaction();
         tx.after_rollback([&]() -> boost::asio::awaitable<void> {
            hook_called = true;
            session_destroyed_before_hook = state->destroyed_sessions == 1U;
            co_return;
         });
      }

      auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
      for (auto attempt = 0; attempt != 100 && !hook_called; ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      BOOST_REQUIRE(hook_called);
      BOOST_CHECK(session_destroyed_before_hook);
      BOOST_CHECK_EQUAL(state->destroyed_sessions, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_commit_failure_preserves_rollback_state) {
   auto runtime = forge::asio::runtime{};
   auto state = std::make_shared<memory_state>();
   state->fail_commit = true;
   auto driver = std::make_shared<memory_driver>(state);

   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto meta = forge::db::core::family{"meta"};
      auto rolled_back = false;

      auto tx = co_await driver->begin_transaction();
      tx.after_rollback([&]() -> boost::asio::awaitable<void> {
         rolled_back = true;
         co_return;
      });
      co_await tx.put(meta, key("a"), bytes("pending"));

      BOOST_CHECK_THROW(co_await tx.commit(), std::runtime_error);
      BOOST_CHECK(tx.active());

      co_await tx.rollback();
      BOOST_CHECK(!tx.active());
      BOOST_CHECK(rolled_back);
      BOOST_CHECK_EQUAL(state->rollback_calls, 1U);

      state->fail_commit = false;
      auto read = co_await driver->begin_read();
      BOOST_CHECK(!(co_await read.get(meta, key("a"))).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_SUITE_END()
