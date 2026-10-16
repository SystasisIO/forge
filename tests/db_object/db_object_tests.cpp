#include <boost/asio/awaitable.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/error_code.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ranked_index.hpp>
#include <boost/multi_index_container.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/exceptions/macros.hpp>
#include <forge/db/object/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <typeindex>
#include <type_traits>
#include <utility>
#include <vector>

import forge.asio.runtime;
import forge.asio.blocking;
import forge.asio.exceptions;
import forge.chain.protocol.fixed_key;
import forge.codec.hex;
import forge.crypto.digest.sha256;
import forge.db.core.exceptions;
import forge.db.ids.object_id;
import forge.db.object.cursor;
import forge.db.object.exceptions;
import forge.db.object.header;
import forge.db.object.hooks;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.core.driver;
import forge.db.core.record;
import forge.db.core.participant;
import forge.db.object.snapshot;
import forge.db.object.store;
import forge.db.object.system;
import forge.db.object.transaction;
import forge.raw.raw;

#if FORGE_HAS_ROCKSDB
import forge.db.rocksdb.driver;
#endif

namespace db_object_tests {

struct toy_ordered {
   std::int64_t value = 0;

   bool operator==(const toy_ordered&) const = default;
};

struct unsupported_key {};

BOOST_DESCRIBE_STRUCT(toy_ordered, (), (value))

} // namespace db_object_tests

template <> struct forge::db::object::sort_key<db_object_tests::toy_ordered> {
   [[nodiscard]] forge::db::object::sort_key_bytes operator()(const db_object_tests::toy_ordered& value) const {
      if (value.value == std::numeric_limits<std::int64_t>::min()) {
         throw std::domain_error{"toy unordered value"};
      }

      auto ordered = static_cast<std::uint64_t>(value.value) ^ (std::uint64_t{1U} << 63U);
      auto bytes = forge::db::object::sort_key_bytes{};
      bytes.reserve(sizeof(ordered));
      for (auto index = sizeof(ordered); index > 0U; --index) {
         const auto shift = static_cast<unsigned>((index - 1U) * 8U);
         bytes.push_back(static_cast<std::byte>((ordered >> shift) & 0xffU));
      }
      return bytes;
   }
};

namespace db_object_tests {

struct by_id;
struct by_name;
struct by_region_balance;
struct by_region;
struct by_document_id;
struct by_tenant_email;
struct by_tenant_rank;
struct by_external_key;
struct by_digest;
struct by_email_method;
struct by_rank_function;
struct by_score;
struct by_ranked_id;
struct by_ranked_token;
struct by_ranked_state;
struct by_ranked_tenant_score;
struct by_payload_bytes;
struct by_score_total;
struct by_conversion_id;
struct by_signed_total;
struct by_unsigned_total;
struct reference_by_state_id;

struct account : forge::db::object::object<account, 1, 7> {
   std::string name;
   std::uint64_t balance = 0;
   std::uint32_t region = 0;

   bool operator==(const account&) const = default;
};

BOOST_DESCRIBE_STRUCT(account, (forge::db::object::object<account, 1, 7>), (name, balance, region))

std::uint32_t account_region(const account& value) {
   return value.region;
}

using account_object = forge::db::object::object_index<
    account, forge::db::object::indexed_by<
                 forge::db::object::primary_unique<by_id>,
                 forge::db::object::ordered_unique<by_name, forge::db::object::member<&account::name>>,
                 forge::db::object::ordered_non_unique<
                     by_region_balance, forge::db::object::composite_key<forge::db::object::member<&account::region>,
                                                                         forge::db::object::member<&account::balance>>>,
                 forge::db::object::ordered_non_unique<by_region, forge::db::object::global_fun<&account_region>>>>;

struct payload_ref {
   std::uint64_t size = 0;

   bool operator==(const payload_ref&) const = default;
};

struct ranked_upload : forge::db::object::object<ranked_upload, 3, 11> {
   std::string token;
   std::uint32_t state = 0;
   std::uint32_t tenant = 0;
   std::int64_t score = 0;
   payload_ref payload;

   bool operator==(const ranked_upload&) const = default;
};

BOOST_DESCRIBE_STRUCT(payload_ref, (), (size))
BOOST_DESCRIBE_STRUCT(ranked_upload, (forge::db::object::object<ranked_upload, 3, 11>),
                      (token, state, tenant, score, payload))

std::int64_t ranked_upload_score(const ranked_upload& value) noexcept {
   return value.score;
}

using payload_bytes_sum =
    forge::db::object::sum<by_payload_bytes, forge::db::object::member<&ranked_upload::payload, &payload_ref::size>>;
using score_total_sum = forge::db::object::sum<by_score_total, forge::db::object::global_fun<&ranked_upload_score>>;

using ranked_upload_object = forge::db::object::object_index<
    ranked_upload,
    forge::db::object::indexed_by<
        forge::db::object::ranked_primary_unique<by_ranked_id, forge::db::object::ranked_schema<1>, payload_bytes_sum,
                                                 score_total_sum>,
        forge::db::object::ranked_unique<by_ranked_token, forge::db::object::member<&ranked_upload::token>,
                                         forge::db::object::ranked_schema<1>, payload_bytes_sum>,
        forge::db::object::ranked_non_unique<by_ranked_state, forge::db::object::member<&ranked_upload::state>,
                                             forge::db::object::ranked_schema<1>, payload_bytes_sum>,
        forge::db::object::ranked_non_unique<
            by_ranked_tenant_score,
            forge::db::object::composite_key<
                forge::db::object::member<&ranked_upload::tenant>,
                forge::db::object::descending<forge::db::object::member<&ranked_upload::score>>>,
            forge::db::object::ranked_schema<1>, payload_bytes_sum>>>;

using ranked_upload_schema_v2_object = forge::db::object::object_index<
    ranked_upload, forge::db::object::indexed_by<forge::db::object::ranked_primary_unique<
                       by_ranked_id, forge::db::object::ranked_schema<2>, payload_bytes_sum, score_total_sum>>>;

using ranked_upload_kind_mismatch_object = forge::db::object::object_index<
    ranked_upload,
    forge::db::object::indexed_by<
        forge::db::object::ranked_primary_unique<by_ranked_id, forge::db::object::ranked_schema<1>, payload_bytes_sum,
                                                 score_total_sum>,
        forge::db::object::ranked_non_unique<by_ranked_token, forge::db::object::member<&ranked_upload::token>,
                                             forge::db::object::ranked_schema<1>, payload_bytes_sum>>>;

using ranked_upload_sum_mismatch_object =
    forge::db::object::object_index<ranked_upload,
                                    forge::db::object::indexed_by<forge::db::object::ranked_primary_unique<
                                        by_ranked_id, forge::db::object::ranked_schema<1>, payload_bytes_sum>>>;

struct ranked_conversion : forge::db::object::object<ranked_conversion, 4, 12> {
   std::uint64_t unsigned_value = 0;
   std::int64_t signed_value = 0;
};

BOOST_DESCRIBE_STRUCT(ranked_conversion, (forge::db::object::object<ranked_conversion, 4, 12>),
                      (unsigned_value, signed_value))

using signed_total_sum =
    forge::db::object::sum<by_signed_total, forge::db::object::member<&ranked_conversion::unsigned_value>,
                           std::int64_t>;
using unsigned_total_sum =
    forge::db::object::sum<by_unsigned_total, forge::db::object::member<&ranked_conversion::signed_value>,
                           std::uint64_t>;
using ranked_conversion_object = forge::db::object::object_index<
    ranked_conversion,
    forge::db::object::indexed_by<forge::db::object::ranked_primary_unique<
        by_conversion_id, forge::db::object::ranked_schema<1>, signed_total_sum, unsigned_total_sum>>>;

struct ranked_reference {
   std::uint64_t id = 0;
   std::uint32_t state = 0;
};

using ranked_reference_index = boost::multi_index_container<
    ranked_reference,
    boost::multi_index::indexed_by<boost::multi_index::ranked_unique<
        boost::multi_index::tag<reference_by_state_id>,
        boost::multi_index::composite_key<
            ranked_reference, boost::multi_index::member<ranked_reference, std::uint32_t, &ranked_reference::state>,
            boost::multi_index::member<ranked_reference, std::uint64_t, &ranked_reference::id>>>>>;

struct document : forge::db::object::object<document, 2, 9> {
   std::uint32_t tenant = 0;
   std::string email;
   std::uint64_t rank = 0;
   forge::chain::protocol::key256 external_key;
   forge::crypto::digest::sha256 digest;
   toy_ordered score;

   [[nodiscard]] const std::string& email_key() const noexcept {
      return email;
   }

   bool operator==(const document&) const = default;
};

BOOST_DESCRIBE_STRUCT(document, (forge::db::object::object<document, 2, 9>),
                      (tenant, email, rank, external_key, digest, score))

std::uint64_t document_rank(const document& value) noexcept {
   return value.rank;
}

using document_object = forge::db::object::object_index<
    document,
    forge::db::object::indexed_by<
        forge::db::object::primary_unique<by_document_id>,
        forge::db::object::ordered_unique<
            by_tenant_email, forge::db::object::composite_key<forge::db::object::member<&document::tenant>,
                                                              forge::db::object::member<&document::email>>>,
        forge::db::object::ordered_non_unique<
            by_tenant_rank, forge::db::object::composite_key<
                                forge::db::object::member<&document::tenant>,
                                forge::db::object::descending<forge::db::object::member<&document::rank>>>>,
        forge::db::object::ordered_non_unique<by_external_key, forge::db::object::member<&document::external_key>>,
        forge::db::object::ordered_non_unique<by_digest, forge::db::object::member<&document::digest>>,
        forge::db::object::ordered_non_unique<by_email_method, forge::db::object::const_mem_fun<&document::email_key>>,
        forge::db::object::ordered_non_unique<by_rank_function, forge::db::object::global_fun<&document_rank>>,
        forge::db::object::ordered_non_unique<
            by_score, forge::db::object::ascending<forge::db::object::member<&document::score>>>>>;

struct bad_account {
   std::string name;
};

using bad_object =
    forge::db::object::object_index<bad_account,
                                    forge::db::object::indexed_by<forge::db::object::primary_unique<by_id>>>;

using duplicate_tag_object = forge::db::object::object_index<
    account,
    forge::db::object::indexed_by<forge::db::object::primary_unique<by_id>,
                                  forge::db::object::ordered_unique<by_id, forge::db::object::member<&account::name>>>>;

struct byte_less {
   bool operator()(const forge::db::core::record_key& left, const forge::db::core::record_key& right) const {
      return left.bytes() < right.bytes();
   }
};

struct memory_state {
   mutable std::mutex mutex;
   std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> records;
   std::size_t scan_calls = 0;
   std::size_t snapshot_calls = 0;
   std::size_t commit_calls = 0;
   std::size_t mutation_calls = 0;
   std::size_t fail_commits = 0;
   std::optional<std::size_t> fail_put_after;
   std::size_t active_writes = 0;
   std::size_t rollback_calls = 0;
   std::size_t destroyed_without_finish = 0;
   bool overlapping_writes = false;
   bool block_rollbacks = false;
   bool fail_rollbacks = false;
   bool rollback_started = false;
   bool support_record_locks = false;
   std::vector<forge::db::core::record_key> lock_requests;
};

class memory_session final : public forge::db::core::session {
 public:
   explicit memory_session(std::shared_ptr<memory_state> state) : state_{std::move(state)} {
      auto guard = std::scoped_lock{state_->mutex};
      working_ = state_->records;
      ++state_->active_writes;
      if (state_->active_writes > 1) {
         state_->overlapping_writes = true;
      }
   }

   ~memory_session() override {
      finish();
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{
          .snapshot_reads = false,
          .writes = true,
          .savepoints = true,
          .record_locks = state_->support_record_locks,
      };
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key key) override {
      const auto found = working_.find(key);
      if (found == working_.end()) {
         co_return std::nullopt;
      }
      co_return found->second;
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>>
   get_for_update(forge::db::core::family family, forge::db::core::record_key key) override {
      {
         auto guard = std::scoped_lock{state_->mutex};
         state_->lock_requests.push_back(key);
      }
      co_return co_await get(std::move(family), std::move(key));
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key key,
                                    std::vector<std::byte> value) override {
      {
         const auto lock = std::scoped_lock{state_->mutex};
         ++state_->mutation_calls;
         if (state_->fail_put_after) {
            if (*state_->fail_put_after == 0U) {
               state_->fail_put_after.reset();
               throw std::runtime_error{"injected object write failure"};
            }
            --*state_->fail_put_after;
         }
      }
      auto stored_key = key;
      working_[stored_key] = value;
      erased_.erase(stored_key);
      writes_[std::move(key)] = std::move(value);
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key key) override {
      {
         const auto lock = std::scoped_lock{state_->mutex};
         ++state_->mutation_calls;
      }
      working_.erase(key);
      writes_.erase(key);
      erased_.insert(std::move(key));
      co_return;
   }

   boost::asio::awaitable<void> create_savepoint() override {
      savepoints_.push_back(savepoint_state{.working = working_, .writes = writes_, .erased = erased_});
      co_return;
   }

   boost::asio::awaitable<void> rollback_to_savepoint() override {
      if (savepoints_.empty()) {
         throw std::logic_error{"db object test savepoint stack is empty"};
      }
      working_ = std::move(savepoints_.back().working);
      writes_ = std::move(savepoints_.back().writes);
      erased_ = std::move(savepoints_.back().erased);
      savepoints_.pop_back();
      co_return;
   }

   boost::asio::awaitable<void> release_savepoint() override {
      if (savepoints_.empty()) {
         throw std::logic_error{"db object test savepoint stack is empty"};
      }
      savepoints_.pop_back();
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::family,
                                                                  forge::db::core::record_range range,
                                                                  forge::db::core::page_request request) override {
      forge::db::object::validate_page_request(request);
      {
         auto guard = std::scoped_lock{state_->mutex};
         ++state_->scan_calls;
      }

      auto result = forge::db::core::record_page{};
      auto current = working_.lower_bound(request.after ? request.after->boundary : range.begin);
      if (request.after && current != working_.end() && current->first == request.after->boundary) {
         ++current;
      }

      auto last_returned = std::optional<forge::db::core::record_key>{};
      while (current != working_.end()) {
         if (range.has_end && !(current->first.bytes() < range.end.bytes())) {
            break;
         }
         result.entries.push_back(forge::db::core::record_entry{.key = current->first, .value = current->second});
         last_returned = current->first;
         ++current;
         if (result.entries.size() == request.limit) {
            break;
         }
      }

      if (current != working_.end() && (!range.has_end || current->first.bytes() < range.end.bytes())) {
         result.next = forge::db::core::cursor{.boundary = std::move(*last_returned)};
      }

      co_return result;
   }

   boost::asio::awaitable<void> commit() override {
      {
         auto guard = std::scoped_lock{state_->mutex};
         ++state_->commit_calls;
         if (state_->fail_commits > 0) {
            --state_->fail_commits;
            throw std::runtime_error{"db object test commit failure"};
         }
         for (const auto& key : erased_) {
            state_->records.erase(key);
         }
         for (auto& [key, value] : writes_) {
            state_->records[key] = std::move(value);
         }
         close_locked();
      }
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      {
         auto guard = std::scoped_lock{state_->mutex};
         ++state_->rollback_calls;
         state_->rollback_started = true;
      }
      const auto executor = co_await boost::asio::this_coro::executor;
      auto timer = boost::asio::steady_timer{executor};
      for (;;) {
         {
            auto guard = std::scoped_lock{state_->mutex};
            if (!state_->block_rollbacks) {
               break;
            }
         }
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      finish();
      working_.clear();
      auto fail = false;
      {
         auto guard = std::scoped_lock{state_->mutex};
         fail = state_->fail_rollbacks;
      }
      if (fail) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "db object test rollback failure");
      }
      co_return;
   }

   [[nodiscard]] bool closed() const noexcept {
      return closed_;
   }

 private:
   struct savepoint_state {
      std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> working;
      std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> writes;
      std::set<forge::db::core::record_key, byte_less> erased;
   };

   void close_locked() noexcept {
      if (!closed_) {
         closed_ = true;
         --state_->active_writes;
      }
   }

   void finish() noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      close_locked();
   }

   std::shared_ptr<memory_state> state_;
   std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> working_;
   std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> writes_;
   std::set<forge::db::core::record_key, byte_less> erased_;
   std::vector<savepoint_state> savepoints_;
   bool closed_ = false;
};

class drop_sensitive_session final : public forge::db::core::session {
 public:
   explicit drop_sensitive_session(std::shared_ptr<memory_state> state) : state_{std::move(state)} {
      ++state_->active_writes;
      if (state_->active_writes > 1) {
         state_->overlapping_writes = true;
      }
   }

   ~drop_sensitive_session() override {
      if (!closed_) {
         ++state_->destroyed_without_finish;
      }
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = false, .writes = true};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key) override {
      co_return std::nullopt;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key,
                                    std::vector<std::byte>) override {
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page>
   scan_page(forge::db::core::family, forge::db::core::record_range, forge::db::core::page_request) override {
      co_return forge::db::core::record_page{};
   }

   boost::asio::awaitable<void> commit() override {
      finish();
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      ++state_->rollback_calls;
      finish();
      co_return;
   }

 private:
   void finish() noexcept {
      if (!closed_) {
         closed_ = true;
         --state_->active_writes;
      }
   }

   std::shared_ptr<memory_state> state_;
   bool closed_ = false;
};

class throwing_rollback_session final : public forge::db::core::session {
 public:
   explicit throwing_rollback_session(std::shared_ptr<memory_state> state) : state_{std::move(state)} {
      ++state_->active_writes;
      if (state_->active_writes > 1) {
         state_->overlapping_writes = true;
      }
   }

   ~throwing_rollback_session() override {
      if (!closed_) {
         ++state_->destroyed_without_finish;
      }
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = false, .writes = true};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key) override {
      co_return std::nullopt;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key,
                                    std::vector<std::byte>) override {
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page>
   scan_page(forge::db::core::family, forge::db::core::record_range, forge::db::core::page_request) override {
      co_return forge::db::core::record_page{};
   }

   boost::asio::awaitable<void> commit() override {
      finish();
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      ++state_->rollback_calls;
      finish();
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "db object test rollback failure");
   }

 private:
   void finish() noexcept {
      if (!closed_) {
         closed_ = true;
         --state_->active_writes;
      }
   }

   std::shared_ptr<memory_state> state_;
   bool closed_ = false;
};

class throwing_commit_session final : public forge::db::core::session {
 public:
   explicit throwing_commit_session(std::shared_ptr<memory_state> state) : state_{std::move(state)} {
      ++state_->active_writes;
      if (state_->active_writes > 1) {
         state_->overlapping_writes = true;
      }
   }

   ~throwing_commit_session() override {
      if (!closed_) {
         ++state_->destroyed_without_finish;
      }
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = false, .writes = true};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key) override {
      co_return std::nullopt;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key,
                                    std::vector<std::byte>) override {
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page>
   scan_page(forge::db::core::family, forge::db::core::record_range, forge::db::core::page_request) override {
      co_return forge::db::core::record_page{};
   }

   boost::asio::awaitable<void> commit() override {
      if (state_->commit_calls++ == 0U) {
         finish();
         co_return;
      }
      throw std::runtime_error{"db object test commit failure"};
   }

   boost::asio::awaitable<void> rollback() override {
      ++state_->rollback_calls;
      finish();
      co_return;
   }

 private:
   void finish() noexcept {
      if (!closed_) {
         closed_ = true;
         --state_->active_writes;
      }
   }

   std::shared_ptr<memory_state> state_;
   bool closed_ = false;
};

class memory_snapshot_session final : public forge::db::core::session {
 public:
   explicit memory_snapshot_session(std::shared_ptr<memory_state> state) : state_{std::move(state)} {
      auto guard = std::scoped_lock{state_->mutex};
      snapshot_ = state_->records;
   }

   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = true, .writes = false};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key key) override {
      const auto found = snapshot_.find(key);
      if (found == snapshot_.end()) {
         co_return std::nullopt;
      }
      co_return found->second;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key,
                                    std::vector<std::byte>) override {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "memory snapshot is read-only");
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "memory snapshot is read-only");
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::family,
                                                                  forge::db::core::record_range range,
                                                                  forge::db::core::page_request request) override {
      forge::db::object::validate_page_request(request);
      {
         auto guard = std::scoped_lock{state_->mutex};
         ++state_->scan_calls;
      }

      auto result = forge::db::core::record_page{};
      auto current = snapshot_.lower_bound(request.after ? request.after->boundary : range.begin);
      if (request.after && current != snapshot_.end() && current->first == request.after->boundary) {
         ++current;
      }

      auto last_returned = std::optional<forge::db::core::record_key>{};
      while (current != snapshot_.end()) {
         if (range.has_end && !(current->first.bytes() < range.end.bytes())) {
            break;
         }
         result.entries.push_back(forge::db::core::record_entry{.key = current->first, .value = current->second});
         last_returned = current->first;
         ++current;
         if (result.entries.size() == request.limit) {
            break;
         }
      }

      if (current != snapshot_.end() && (!range.has_end || current->first.bytes() < range.end.bytes())) {
         result.next = forge::db::core::cursor{.boundary = std::move(*last_returned)};
      }

      co_return result;
   }

   boost::asio::awaitable<void> commit() override {
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::unsupported_operation, "memory snapshot cannot commit");
   }

   boost::asio::awaitable<void> rollback() override {
      co_return;
   }

 private:
   std::shared_ptr<memory_state> state_;
   std::map<forge::db::core::record_key, std::vector<std::byte>, byte_less> snapshot_;
};

class invalid_session final : public forge::db::core::session {
 public:
   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = false, .writes = false};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family,
                                                                     forge::db::core::record_key) override {
      co_return std::nullopt;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key,
                                    std::vector<std::byte>) override {
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page>
   scan_page(forge::db::core::family, forge::db::core::record_range, forge::db::core::page_request) override {
      co_return forge::db::core::record_page{};
   }

   boost::asio::awaitable<void> commit() override {
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      co_return;
   }
};

class memory_driver : public forge::db::core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override {
      co_return;
   }

   [[nodiscard]] std::size_t scan_calls() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->scan_calls;
   }

   [[nodiscard]] std::size_t snapshot_calls() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->snapshot_calls;
   }

   void support_record_locks(bool value) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->support_record_locks = value;
   }

   [[nodiscard]] std::vector<forge::db::core::record_key> lock_requests() const {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->lock_requests;
   }

   void clear_lock_requests() {
      auto guard = std::scoped_lock{state_->mutex};
      state_->lock_requests.clear();
   }

   [[nodiscard]] std::size_t record_count() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->records.size();
   }

   [[nodiscard]] auto records() const {
      const auto lock = std::scoped_lock{state_->mutex};
      return state_->records;
   }

   [[nodiscard]] std::size_t mutation_calls() const noexcept {
      const auto lock = std::scoped_lock{state_->mutex};
      return state_->mutation_calls;
   }

   void seed_record(forge::db::core::record_key key, std::vector<std::byte> value) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->records[std::move(key)] = std::move(value);
   }

   [[nodiscard]] std::optional<std::vector<std::byte>> record_value(const forge::db::core::record_key& key) const {
      auto guard = std::scoped_lock{state_->mutex};
      const auto found = state_->records.find(key);
      if (found == state_->records.end()) {
         return std::nullopt;
      }
      return found->second;
   }

   [[nodiscard]] std::size_t commit_calls() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->commit_calls;
   }

   [[nodiscard]] std::vector<forge::db::core::record_key> keys() const {
      auto guard = std::scoped_lock{state_->mutex};
      auto out = std::vector<forge::db::core::record_key>{};
      out.reserve(state_->records.size());
      for (const auto& [key, _] : state_->records) {
         out.push_back(key);
      }
      return out;
   }

   [[nodiscard]] std::optional<forge::db::core::record_key> first_key_with_kind(std::uint8_t kind) const {
      auto guard = std::scoped_lock{state_->mutex};
      for (const auto& [key, _] : state_->records) {
         if (!key.empty() && std::to_integer<std::uint8_t>(key.bytes().front()) == kind) {
            return key;
         }
      }
      return std::nullopt;
   }

   void erase_record(const forge::db::core::record_key& key) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->records.erase(key);
   }

   void replace_record(forge::db::core::record_key key, std::vector<std::byte> value) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->records[std::move(key)] = std::move(value);
   }

   [[nodiscard]] bool overlapping_writes() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->overlapping_writes;
   }

   [[nodiscard]] std::size_t active_writes() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->active_writes;
   }

   void block_rollbacks(bool value) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->block_rollbacks = value;
   }

   void fail_rollbacks(bool value) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->fail_rollbacks = value;
   }

   void fail_next_commits(std::size_t count) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->fail_commits = count;
   }

   void fail_put_after(std::size_t successful_writes) {
      auto guard = std::scoped_lock{state_->mutex};
      state_->fail_put_after = successful_writes;
   }

   [[nodiscard]] bool rollback_started() const noexcept {
      auto guard = std::scoped_lock{state_->mutex};
      return state_->rollback_started;
   }

 private:
   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_transaction() override {
      co_return std::make_unique<memory_session>(state_);
   }

   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_snapshot() override {
      {
         auto guard = std::scoped_lock{state_->mutex};
         ++state_->snapshot_calls;
      }
      co_return std::make_unique<memory_snapshot_session>(state_);
   }

   std::shared_ptr<memory_state> state_ = std::make_shared<memory_state>();
};

template <typename Session> class session_driver : public forge::db::core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override {
      co_return;
   }

   [[nodiscard]] std::size_t active_writes() const noexcept {
      return state_->active_writes;
   }

   [[nodiscard]] std::size_t rollback_calls() const noexcept {
      return state_->rollback_calls;
   }

   [[nodiscard]] std::size_t destroyed_without_finish() const noexcept {
      return state_->destroyed_without_finish;
   }

   [[nodiscard]] bool overlapping_writes() const noexcept {
      return state_->overlapping_writes;
   }

 private:
   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_transaction() override {
      if constexpr (std::is_constructible_v<Session, std::shared_ptr<memory_state>>) {
         co_return std::make_unique<Session>(state_);
      } else {
         co_return std::make_unique<Session>();
      }
   }

   boost::asio::awaitable<std::unique_ptr<forge::db::core::session>> open_snapshot() override {
      co_return std::make_unique<invalid_session>();
   }

   std::shared_ptr<memory_state> state_ = std::make_shared<memory_state>();
};

class pausing_allocation_participant final : public forge::db::core::transaction_participant {
 public:
   explicit pausing_allocation_participant(boost::asio::any_io_executor executor)
       : entered_event{executor, std::chrono::steady_clock::now() + std::chrono::seconds{2}},
         release_event{executor, std::chrono::steady_clock::now() + std::chrono::seconds{2}} {}

   std::string_view name() const noexcept override {
      return "test.allocation.pause";
   }
   bool captures_mutations() const noexcept override {
      return true;
   }

   boost::asio::awaitable<void> prepare_mutation(const forge::db::core::record_mutation& mutation) override {
      sequence = !mutation.key.empty() && mutation.key.bytes().front() == std::byte{2};
      if (sequence && fail_sequence) {
         fail_sequence = false;
         throw std::runtime_error{"allocation write veto"};
      }
      if (sequence && !entered) {
         entered = true;
         entered_event.cancel();
         auto error = boost::system::error_code{};
         co_await release_event.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
         if (!released) {
            throw std::runtime_error{"allocation barrier expired"};
         }
      }
   }
   void publish_mutation() noexcept override {
      sequence_writes += sequence ? 1U : 0U;
   }
   boost::asio::awaitable<void> wait_entered() {
      if (!entered) {
         auto error = boost::system::error_code{};
         co_await entered_event.async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, error));
      }
      if (!entered) {
         throw std::runtime_error{"allocation was not admitted"};
      }
   }
   void release() {
      released = true;
      release_event.cancel();
   }

   boost::asio::steady_timer entered_event;
   boost::asio::steady_timer release_event;
   bool entered = false;
   bool released = false;
   bool sequence = false;
   bool fail_sequence = false;
   std::size_t sequence_writes = 0;
};

class veto_interceptor final : public forge::db::object::interceptor {
 public:
   boost::asio::awaitable<void> before_mutation(const forge::db::object::object_mutation& mutation) override {
      ++calls;
      last = mutation.kind;
      FORGE_THROW_EXCEPTION(forge::db::object::exceptions::duplicate_object, "db object test interceptor veto");
   }

   std::size_t calls = 0;
   std::optional<forge::db::object::mutation_kind> last;
};

class counting_observer final : public forge::db::object::observer {
 public:
   boost::asio::awaitable<void> after_commit(const forge::db::object::change_set& changes) override {
      ++calls;
      mutation_count += changes.mutations.size();
      last = changes;
      co_return;
   }

   std::size_t calls = 0;
   std::size_t mutation_count = 0;
   std::optional<forge::db::object::change_set> last;
};

class recording_precommit_observer final : public forge::db::object::precommit_observer {
 public:
   boost::asio::awaitable<void> before_commit(const forge::db::object::change_set& changes) override {
      ++calls;
      last = changes;
      co_return;
   }

   std::size_t calls = 0;
   std::optional<forge::db::object::change_set> last;
};

class writing_precommit_observer final : public forge::db::object::precommit_observer {
 public:
   boost::asio::awaitable<void> before_commit(const forge::db::object::change_set& changes) override {
      ++calls;
      last = changes;
      if (write) {
         co_await write();
      }
      co_return;
   }

   std::function<boost::asio::awaitable<void>()> write;
   std::size_t calls = 0;
   std::optional<forge::db::object::change_set> last;
};

class reentrant_precommit_observer final : public forge::db::object::precommit_observer {
 public:
   forge::db::object::transaction* transaction = nullptr;
   std::shared_ptr<forge::db::object::precommit_observer> nested;
   bool rejected = false;

   boost::asio::awaitable<void> before_commit(const forge::db::object::change_set&) override {
      try {
         transaction->add_precommit_observer(nested);
      } catch (const forge::db::object::exceptions::transaction_closed&) {
         rejected = true;
      }
      co_return;
   }
};

class reentrant_completion_observer final : public forge::db::object::precommit_observer {
 public:
   forge::db::core::transaction* transaction = nullptr;
   bool commit_rejected = false;
   bool rollback_rejected = false;

   boost::asio::awaitable<void> before_commit(const forge::db::object::change_set&) override {
      try {
         co_await transaction->commit();
      } catch (const forge::db::core::exceptions::participant_conflict&) {
         commit_rejected = true;
      }
      try {
         co_await transaction->rollback();
      } catch (const forge::db::core::exceptions::participant_conflict&) {
         rollback_rejected = true;
      }
   }
};

std::string hex(const std::vector<std::byte>& bytes) {
   auto out = std::ostringstream{};
   out << std::hex << std::setfill('0');
   for (auto byte : bytes) {
      out << std::setw(2) << std::to_integer<unsigned>(byte);
   }
   return out.str();
}

[[nodiscard]] forge::db::core::record_key header_record_key() {
   return forge::db::core::record_key{std::vector<std::byte>(11U, std::byte{0U})};
}

[[nodiscard]] std::vector<std::byte> packed_header(std::uint32_t version) {
   auto value = forge::db::object::header{};
   value.id = forge::db::object::header_id;
   value.version = version;
   const auto packed = forge::raw::pack(value);
   auto bytes = std::vector<std::byte>{};
   bytes.reserve(packed.size());
   for (const auto byte : packed) {
      bytes.push_back(static_cast<std::byte>(byte));
   }
   return bytes;
}

[[nodiscard]] account make_account(std::uint64_t instance, std::string name, std::uint64_t balance,
                                   std::uint32_t region) {
   auto value = account{};
   value.id = account::id_t{instance};
   value.name = std::move(name);
   value.balance = balance;
   value.region = region;
   return value;
}

[[nodiscard]] document make_document(std::uint64_t instance, std::uint32_t tenant, std::string email,
                                     std::uint64_t rank, std::int64_t score = 0) {
   auto value = document{};
   value.id = document::id_t{instance};
   value.tenant = tenant;
   value.email = std::move(email);
   value.rank = rank;
   value.external_key = forge::chain::protocol::key256::make_from_word_sequence<std::uint64_t>(
       std::uint64_t{0U}, std::uint64_t{0U}, std::uint64_t{0U}, instance);
   value.digest = forge::crypto::digest::sha256::hash(value.email + ':' + std::to_string(instance));
   value.score = toy_ordered{score};
   return value;
}

[[nodiscard]] ranked_upload make_ranked_upload(std::uint64_t instance, std::uint32_t state, std::uint32_t tenant,
                                               std::int64_t score, std::uint64_t size) {
   auto value = ranked_upload{};
   value.id = ranked_upload::id_t{instance};
   value.token = "upload-" + std::to_string(instance);
   value.state = state;
   value.tenant = tenant;
   value.score = score;
   value.payload.size = size;
   return value;
}

[[nodiscard]] boost::asio::awaitable<forge::db::object::store>
make_store(const std::shared_ptr<memory_driver>& driver, forge::db::object::store::options options = {}) {
   auto store = co_await forge::db::object::store::open(driver, options);
   store.register_object<account_object>();
   co_return store;
}

} // namespace db_object_tests

using db_object_tests::account_object;

FORGE_DB_OBJECT(account_object)
FORGE_DB_OBJECT(db_object_tests::document_object)
FORGE_DB_OBJECT(db_object_tests::ranked_upload_object)
FORGE_DB_OBJECT(db_object_tests::ranked_conversion_object)

using namespace db_object_tests;

static_assert(forge::db::object::object_model<account_object>);
static_assert(forge::db::object::object_model<document_object>);
static_assert(forge::db::object::object_model<ranked_upload_object>);
static_assert(std::same_as<payload_bytes_sum::accumulator_type, std::uint64_t>);
static_assert(!forge::db::object::object_model<bad_object>);
static_assert(!forge::db::object::object_model<duplicate_tag_object>);
static_assert(std::same_as<forge::db::object::id_t_of<account_object>, forge::db::ids::typed_id<1, 7>>);
static_assert(std::same_as<forge::db::object::index_for_id_t<account::id_t>, account_object>);
static_assert(
    std::same_as<forge::db::object::index_for_id_t<forge::db::object::header::id_t>, forge::db::object::header_index>);
static_assert(std::movable<forge::db::object::transaction>);
static_assert(!std::copy_constructible<forge::db::object::transaction>);
static_assert(std::same_as<forge::db::object::index_by_tag<account_object, by_name>,
                           forge::db::object::ordered_unique<by_name, forge::db::object::member<&account::name>>>);
static_assert(forge::db::object::index_id_by_tag<account_object, by_id> == 0);
static_assert(forge::db::object::index_id_by_tag<account_object, by_name> == 1);
static_assert(forge::db::object::index_id_by_tag<account_object, by_region_balance> == 2);
static_assert(forge::db::object::index_id_by_tag<account_object, by_region> == 3);
static_assert(forge::db::object::sortable_key<forge::chain::protocol::key256>);
static_assert(forge::db::object::sortable_key<forge::crypto::digest::sha256>);
static_assert(forge::db::object::sortable_key<toy_ordered>);
static_assert(!forge::db::object::sortable_key<unsupported_key>);
static_assert(forge::db::object::key_extractor<forge::db::object::member<&document::email>>);
static_assert(forge::db::object::member<&document::email>::pointer == &document::email);
static_assert(forge::db::object::key_extractor<forge::db::object::const_mem_fun<&document::email_key>>);
static_assert(forge::db::object::key_extractor<forge::db::object::global_fun<&document_rank>>);

using tenant_email_view = forge::db::object::index_view<document_object, by_tenant_email>;
using email_view = forge::db::object::index_view<document_object, by_email_method>;

template <typename View>
concept can_find_tenant_only = requires(View& view) { view.find(std::uint32_t{1}); };

template <typename View>
concept can_query_three_components = requires(View& view) { view.equal_range(std::uint32_t{1}, "a", 3U); };

template <typename View>
concept can_find_two_strings = requires(View& view) { view.find("a", "b"); };

struct header_mutator {
   void operator()(forge::db::object::header&) const {}
};

template <typename Owner>
concept can_insert_header = requires(Owner& owner, forge::db::object::header value) { owner.insert(value); };

template <typename Owner>
concept can_replace_header = requires(Owner& owner, forge::db::object::header value) { owner.replace(value); };

template <typename Owner>
concept can_modify_header = requires(Owner& owner) { owner.modify(forge::db::object::header_id, header_mutator{}); };

template <typename Owner>
concept can_erase_header = requires(Owner& owner) { owner.erase(forge::db::object::header_id); };

static_assert(requires(tenant_email_view& view) { view.find(std::uint32_t{1}, std::string_view{"a"}); });
static_assert(requires(tenant_email_view& view) { view.equal_range(std::uint32_t{1}); });
static_assert(!can_find_tenant_only<tenant_email_view>);
static_assert(!can_query_three_components<tenant_email_view>);
static_assert(!can_find_two_strings<email_view>);
static_assert(forge::db::object::system_object_value<forge::db::object::header>);
static_assert(!forge::db::object::application_object_value<forge::db::object::header>);
static_assert(forge::db::object::system_object_model<forge::db::object::header_index>);
static_assert(forge::db::object::application_object_model<account_object>);
static_assert(requires(forge::db::object::store& store) { store.get(forge::db::object::header_id); });
static_assert(requires(const forge::db::object::store& store) {
   store.index<forge::db::object::header_index, forge::db::object::header_by_id>();
});
static_assert(requires(forge::db::object::snapshot& snapshot) { snapshot.get(forge::db::object::header_id); });
static_assert(requires(const forge::db::object::snapshot& snapshot) {
   snapshot.index<forge::db::object::header_index, forge::db::object::header_by_id>();
});
static_assert(requires(forge::db::object::transaction& transaction) { transaction.get(forge::db::object::header_id); });
static_assert(requires(const forge::db::object::transaction& transaction) {
   transaction.index<forge::db::object::header_index, forge::db::object::header_by_id>();
});
static_assert(!can_insert_header<forge::db::object::store>);
static_assert(!can_replace_header<forge::db::object::store>);
static_assert(!can_modify_header<forge::db::object::store>);
static_assert(!can_erase_header<forge::db::object::store>);
static_assert(!can_insert_header<forge::db::object::transaction>);
static_assert(!can_replace_header<forge::db::object::transaction>);
static_assert(!can_modify_header<forge::db::object::transaction>);
static_assert(!can_erase_header<forge::db::object::transaction>);

BOOST_AUTO_TEST_SUITE(db_object_test_suite)

BOOST_AUTO_TEST_CASE(db_object_base_object_raw_serializes_id_before_fields) {
   const auto value = make_account(42, "alice", 100, 3);
   const auto bytes = forge::raw::pack(value);

   BOOST_CHECK_EQUAL(forge::codec::hex::encode(bytes), "2a0000000000000005616c696365640000000000000003000000");
   BOOST_CHECK(forge::raw::unpack<account>(bytes) == value);
}

BOOST_AUTO_TEST_CASE(db_object_descriptor_derives_type_from_base_object_id) {
   constexpr auto type = forge::db::object::object_id_of<account_object>::value;

   BOOST_CHECK_EQUAL(type.space, 1U);
   BOOST_CHECK_EQUAL(type.type, 7U);
}

BOOST_AUTO_TEST_CASE(db_object_ranked_indexes_maintain_counts_sums_and_positions) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_upload_object>();

      auto primary = store.index<ranked_upload_object, by_ranked_id>();
      auto tokens = store.index<ranked_upload_object, by_ranked_token>();
      auto states = store.index<ranked_upload_object, by_ranked_state>();
      BOOST_CHECK_EQUAL(co_await primary.count(), 0U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_payload_bytes>(), 0U);

      const auto first = make_ranked_upload(10, 1, 7, 20, 10);
      const auto second = make_ranked_upload(11, 1, 7, -10, 20);
      const auto third = make_ranked_upload(12, 2, 8, 30, 30);
      co_await store.insert(first);
      co_await store.insert(second);
      co_await store.insert(third);

      BOOST_CHECK_EQUAL(co_await primary.count(), 3U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_payload_bytes>(), 60U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_score_total>(), 40);
      BOOST_REQUIRE((co_await primary.nth(0)).has_value());
      BOOST_CHECK_EQUAL((co_await primary.nth(0))->id.instance, 10U);
      BOOST_CHECK_EQUAL((co_await primary.nth(2))->id.instance, 12U);
      BOOST_CHECK(!(co_await primary.nth(3)).has_value());
      const auto snapshots_before_rank = driver->snapshot_calls();
      BOOST_CHECK_EQUAL(co_await primary.rank(second), 1U);
      BOOST_CHECK_EQUAL(driver->snapshot_calls(), snapshots_before_rank + 1U);
      BOOST_CHECK_EQUAL(co_await tokens.find_rank(second.token), 1U);
      BOOST_CHECK_EQUAL(co_await tokens.rank(second), 1U);
      BOOST_CHECK_EQUAL(co_await tokens.sum<by_payload_bytes>(), 60U);

      BOOST_CHECK_EQUAL(co_await states.count(), 3U);
      BOOST_CHECK_EQUAL(co_await states.sum<by_payload_bytes>(), 60U);
      BOOST_CHECK_EQUAL(co_await states.equal_range(1U).count(), 2U);
      BOOST_CHECK_EQUAL(co_await states.equal_range(1U).sum<by_payload_bytes>(), 30U);
      BOOST_CHECK((co_await states.equal_range_rank(1U)) == (std::pair<std::uint64_t, std::uint64_t>{0U, 2U}));
      BOOST_CHECK_EQUAL(co_await states.find_rank(1U), 0U);
      const auto snapshots_before_missing_rank = driver->snapshot_calls();
      BOOST_CHECK_EQUAL(co_await states.find_rank(9U), 3U);
      BOOST_CHECK_EQUAL(driver->snapshot_calls(), snapshots_before_missing_rank + 1U);
      BOOST_CHECK_EQUAL(co_await states.lower_bound_rank(2U), 2U);
      BOOST_CHECK_EQUAL(co_await states.upper_bound_rank(1U), 2U);
      BOOST_CHECK((co_await states.range_rank(1U, 2U)) == (std::pair<std::uint64_t, std::uint64_t>{0U, 2U}));
      BOOST_CHECK_EQUAL(co_await states.equal_range(9U).count(), 0U);
      BOOST_CHECK_EQUAL(co_await states.equal_range(9U).sum<by_payload_bytes>(), 0U);

      auto tenant_scores = store.index<ranked_upload_object, by_ranked_tenant_score>();
      BOOST_CHECK_EQUAL((co_await tenant_scores.nth(0))->id.instance, 10U);
      BOOST_CHECK_EQUAL((co_await tenant_scores.nth(1))->id.instance, 11U);
      BOOST_CHECK_EQUAL(co_await tenant_scores.equal_range(std::tuple{7U}).count(), 2U);

      auto moved = first;
      moved.state = 3;
      moved.payload.size = 15;
      co_await store.replace(moved);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_payload_bytes>(), 65U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_score_total>(), 40);
      BOOST_CHECK_EQUAL(co_await states.equal_range(1U).count(), 1U);
      BOOST_CHECK_EQUAL(co_await states.equal_range(1U).sum<by_payload_bytes>(), 20U);
      BOOST_CHECK_EQUAL(co_await states.rank(second), 0U);
      BOOST_CHECK_EQUAL(co_await states.rank(third), 1U);
      BOOST_CHECK_EQUAL(co_await states.rank(moved), 2U);

      co_await store.erase(second.id);
      BOOST_CHECK_EQUAL(co_await primary.count(), 2U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_payload_bytes>(), 45U);
      BOOST_CHECK_EQUAL(co_await primary.sum<by_score_total>(), 50);
      BOOST_CHECK_THROW(co_await states.rank(second), forge::db::object::exceptions::not_found);

      auto reused_token = make_ranked_upload(13, 4, 9, 40, 5);
      reused_token.token = second.token;
      co_await store.insert(reused_token);
      BOOST_CHECK_THROW(co_await tokens.rank(second), forge::db::object::exceptions::not_found);
      BOOST_CHECK_EQUAL(co_await tokens.rank(reused_token), 1U);
      co_await store.erase(reused_token.id);

      auto reopened = co_await forge::db::object::store::open(driver);
      reopened.register_object<ranked_upload_object>();
      auto reopened_primary = reopened.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_EQUAL(co_await reopened_primary.count(), 2U);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_upper_bound_accepts_source_end_sentinel) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_upload_object>();
      co_await store.insert(make_ranked_upload(10, 1, 7, 20, 10));
      co_await store.insert(make_ranked_upload(11, 1, 7, -10, 20));

      auto primary = store.index<ranked_upload_object, by_ranked_id>();
      const auto maximum = ranked_upload::id_t{std::numeric_limits<std::uint64_t>::max()};
      BOOST_CHECK_EQUAL(co_await primary.upper_bound_rank(maximum), 2U);
      BOOST_CHECK_EQUAL(co_await primary.upper_bound(maximum).count(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_guarded_transaction_index_stream_uses_page_fallback) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_upload_object>();
      co_await store.insert(make_ranked_upload(10, 1, 7, 20, 10));

      auto tx = co_await store.begin_transaction();
      auto guarded = tx.index<ranked_upload_object, by_ranked_id>().guarded([] {});
      auto stream = guarded.lower_bound(ranked_upload::id_t{0}).stream({.page_size = 1});
      const auto first = co_await stream.next();
      BOOST_REQUIRE(first.has_value());
      BOOST_CHECK_EQUAL(first->id.instance, 10U);
      BOOST_CHECK(!(co_await stream.next()).has_value());
      co_await tx.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_guarded_non_ranked_index_preserves_missing_ranked_callbacks) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto guarded = store.index<account_object, by_name>().guarded([] {});

      BOOST_CHECK_THROW(co_await guarded.query_aggregate(forge::db::core::record_range{}),
                        forge::db::object::exceptions::invalid_descriptor);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_indexes_fail_closed_for_missing_or_corrupt_state) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto missing_driver = std::make_shared<memory_driver>();
      auto missing_store = co_await forge::db::object::store::open(missing_driver);
      missing_store.register_object<ranked_upload_object>();
      co_await missing_store.insert(make_ranked_upload(1, 1, 1, 1, 10));
      const auto missing_root = missing_driver->first_key_with_kind(0x30U);
      BOOST_REQUIRE(missing_root.has_value());
      missing_driver->erase_record(*missing_root);
      auto missing_index = missing_store.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_THROW(co_await missing_index.count(), forge::db::object::exceptions::aggregate_rebuild_required);

      auto corrupt_driver = std::make_shared<memory_driver>();
      auto corrupt_store = co_await forge::db::object::store::open(corrupt_driver);
      corrupt_store.register_object<ranked_upload_object>();
      co_await corrupt_store.insert(make_ranked_upload(1, 1, 1, 1, 10));
      const auto corrupt_root = corrupt_driver->first_key_with_kind(0x30U);
      BOOST_REQUIRE(corrupt_root.has_value());
      corrupt_driver->replace_record(*corrupt_root, {std::byte{0xffU}});
      auto corrupt_index = corrupt_store.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_THROW(co_await corrupt_index.count(), forge::db::object::exceptions::aggregate_corruption);

      auto schema_driver = std::make_shared<memory_driver>();
      auto schema_store = co_await forge::db::object::store::open(schema_driver);
      schema_store.register_object<ranked_upload_object>();
      co_await schema_store.insert(make_ranked_upload(1, 1, 1, 1, 10));

      auto version_store = co_await forge::db::object::store::open(schema_driver);
      version_store.register_object<ranked_upload_schema_v2_object>();
      BOOST_CHECK_THROW((co_await version_store.index<ranked_upload_schema_v2_object, by_ranked_id>().count()),
                        forge::db::object::exceptions::aggregate_corruption);

      auto kind_store = co_await forge::db::object::store::open(schema_driver);
      kind_store.register_object<ranked_upload_kind_mismatch_object>();
      BOOST_CHECK_THROW((co_await kind_store.index<ranked_upload_kind_mismatch_object, by_ranked_token>().count()),
                        forge::db::object::exceptions::aggregate_corruption);

      auto sum_store = co_await forge::db::object::store::open(schema_driver);
      sum_store.register_object<ranked_upload_sum_mismatch_object>();
      BOOST_CHECK_THROW((co_await sum_store.index<ranked_upload_sum_mismatch_object, by_ranked_id>().count()),
                        forge::db::object::exceptions::aggregate_corruption);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_indexes_match_boost_ranked_order_and_remain_sublinear) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_upload_object>();

      auto order = std::vector<std::uint64_t>(128U);
      for (auto index = std::uint64_t{0}; index < order.size(); ++index) {
         order[index] = index;
      }
      auto random = std::mt19937_64{0x5eedU};
      std::shuffle(order.begin(), order.end(), random);

      auto reference = ranked_reference_index{};
      for (const auto id : order) {
         const auto state = static_cast<std::uint32_t>((id * 37U) % 13U);
         co_await store.insert(make_ranked_upload(id, state, 1U, static_cast<std::int64_t>(id), id + 1U));
         reference.insert(ranked_reference{.id = id, .state = state});
      }

      auto ranked = store.index<ranked_upload_object, by_ranked_state>();
      const auto& expected = reference.get<reference_by_state_id>();
      for (auto position = std::uint64_t{0}; position < order.size(); ++position) {
         const auto actual = co_await ranked.nth(position);
         BOOST_REQUIRE(actual.has_value());
         BOOST_CHECK_EQUAL(actual->id.instance, expected.nth(position)->id);
         BOOST_CHECK_EQUAL(co_await ranked.rank(*actual), position);
      }

      for (auto state = std::uint32_t{0}; state < 13U; ++state) {
         const auto expected_lower = expected.lower_bound_rank(boost::make_tuple(state));
         const auto expected_upper = expected.upper_bound_rank(boost::make_tuple(state));
         const auto actual = co_await ranked.equal_range_rank(state);
         BOOST_CHECK_EQUAL(actual.first, expected_lower);
         BOOST_CHECK_EQUAL(actual.second, expected_upper);
      }

      const auto scans_before_global = driver->scan_calls();
      BOOST_CHECK_EQUAL(co_await ranked.count(), order.size());
      BOOST_CHECK_EQUAL(driver->scan_calls(), scans_before_global);

      const auto scans_before_range = driver->scan_calls();
      (void)co_await ranked.equal_range(7U).count();
      const auto range_scans = driver->scan_calls() - scans_before_range;
      BOOST_CHECK_LT(range_scans, order.size());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_backend_policy_requires_record_locks_before_mutation) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<ranked_upload_object>();

      const auto value = make_ranked_upload(1, 1, 1, 1, 10);
      BOOST_CHECK_THROW(co_await store.insert(value), forge::db::object::exceptions::unsupported_operation);
      BOOST_CHECK(!(co_await store.find(value.id)).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transactional_id_allocation_requires_single_writer_policy) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto options = forge::db::object::store::options{
          .writes = forge::db::object::write_policy::backend,
          .id_allocation = forge::db::object::id_allocation_policy::transactional,
      };
      BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(driver, options)),
                        forge::db::object::exceptions::invalid_descriptor);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_backend_policy_locks_family_coordinator_before_roots) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   driver->support_record_locks(true);
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<ranked_upload_object>();
      driver->clear_lock_requests();

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_ranked_upload(1, 1, 1, 1, 10));
      const auto locks = driver->lock_requests();
      BOOST_REQUIRE(!locks.empty());
      BOOST_REQUIRE(!locks.front().empty());
      BOOST_CHECK_EQUAL(std::to_integer<std::uint8_t>(locks.front().bytes().front()), 0x32U);
      BOOST_CHECK(std::any_of(locks.begin(), locks.end(), [](const auto& value) {
         return !value.empty() && std::to_integer<std::uint8_t>(value.bytes().front()) == 0x30U;
      }));
      co_await tx.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_backend_join_rejects_late_coordinator_claim) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   driver->support_record_locks(true);
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<ranked_upload_object>();

      auto active = co_await driver->begin_transaction();
      (void)co_await active.get_for_update(forge::db::core::family{"preexisting"},
                                           forge::db::core::record_key{std::vector<std::byte>{std::byte{0x01}}});

      BOOST_CHECK_THROW(co_await store.join(active), forge::db::core::exceptions::participant_conflict);
      BOOST_CHECK(!active.claims_family(forge::db::core::family{"objectdb"}));
      co_await active.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_indexes_follow_snapshot_savepoint_and_overflow_contracts) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_upload_object>();
      co_await store.insert(make_ranked_upload(1, 1, 1, 1, 10));

      auto snapshot = co_await store.begin_read();
      co_await store.insert(make_ranked_upload(2, 1, 1, 2, 20));
      auto snapshot_primary = snapshot.index<ranked_upload_object, by_ranked_id>();
      auto current_primary = store.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_EQUAL(co_await snapshot_primary.count(), 1U);
      BOOST_CHECK_EQUAL(co_await current_primary.count(), 2U);

      auto tx = co_await store.begin_transaction();
      const auto point = co_await tx.db_transaction().create_savepoint();
      co_await tx.insert(make_ranked_upload(3, 2, 1, 3, 30));
      auto transaction_primary = tx.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_EQUAL(co_await transaction_primary.count(), 3U);
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(co_await transaction_primary.count(), 2U);
      co_await tx.commit();

      auto rolled_back = co_await store.begin_transaction();
      auto rolled_back_primary = rolled_back.index<ranked_upload_object, by_ranked_id>();
      co_await rolled_back.insert(make_ranked_upload(4, 3, 1, 4, 40));
      BOOST_CHECK_EQUAL(co_await rolled_back_primary.count(), 3U);
      co_await rolled_back.rollback();
      BOOST_CHECK_EQUAL(co_await current_primary.count(), 2U);

      auto overflow_driver = std::make_shared<memory_driver>();
      auto overflow_store = co_await forge::db::object::store::open(overflow_driver);
      overflow_store.register_object<ranked_upload_object>();
      co_await overflow_store.insert(make_ranked_upload(1, 1, 1, 1, std::numeric_limits<std::uint64_t>::max()));
      BOOST_CHECK_THROW(co_await overflow_store.insert(make_ranked_upload(2, 1, 1, 2, 1)),
                        forge::db::object::exceptions::aggregate_overflow);
      auto overflow_primary = overflow_store.index<ranked_upload_object, by_ranked_id>();
      BOOST_CHECK_EQUAL(co_await overflow_primary.count(), 1U);
      BOOST_CHECK_EQUAL(co_await overflow_primary.sum<by_payload_bytes>(), std::numeric_limits<std::uint64_t>::max());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_ranked_projection_overflow_uses_public_exception) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<ranked_conversion_object>();

      auto unsigned_overflow = ranked_conversion{};
      unsigned_overflow.id = ranked_conversion::id_t{1};
      unsigned_overflow.unsigned_value = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
      BOOST_CHECK_THROW(co_await store.insert(unsigned_overflow), forge::db::object::exceptions::aggregate_overflow);
      BOOST_CHECK(!(co_await store.find(unsigned_overflow.id)).has_value());

      auto negative_signed = ranked_conversion{};
      negative_signed.id = ranked_conversion::id_t{2};
      negative_signed.signed_value = -1;
      BOOST_CHECK_THROW(co_await store.insert(negative_signed), forge::db::object::exceptions::aggregate_overflow);
      BOOST_CHECK(!(co_await store.find(negative_signed.id)).has_value());

      auto primary = store.index<ranked_conversion_object, by_conversion_id>();
      BOOST_CHECK_EQUAL(co_await primary.count(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_require_pristine_keeps_same_transaction_and_alias_writer) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      const auto before = driver->records();
      const auto mutations = driver->mutation_calls();
      const auto commits = driver->commit_calls();
      auto core = co_await driver->begin_transaction();
      {
         auto active = co_await store.join(core);
         auto alias = co_await store.join(active);
         co_await store.require_pristine(alias);
         BOOST_CHECK(driver->records() == before);
         BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
         BOOST_CHECK_EQUAL(driver->commit_calls(), commits);
         BOOST_CHECK_EQUAL(driver->active_writes(), 1U);
         co_await alias.insert(make_account(42U, "provisioned", 1U, 1U));
      }
      // The Core-owned participant retains the SAME writer lease after both
      // local Object wrappers retire; no second transaction or Revision scope.
      BOOST_CHECK(core.claims_family(store.family()));
      BOOST_CHECK_EQUAL(driver->active_writes(), 1U);
      co_await core.commit();
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42U})).name, "provisioned");
      auto occupied = co_await store.begin_transaction();
      BOOST_CHECK_THROW(co_await store.require_pristine(occupied), forge::db::object::exceptions::invalid_descriptor);
      co_await occupied.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_require_pristine_rejects_every_nonheader_record_without_writes) {
   auto runtime = forge::asio::runtime{};
   for (const auto mode : {0U, 1U, 2U, 3U, 4U}) {
      auto driver = std::make_shared<memory_driver>();
      forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
         auto store = co_await make_store(driver);
         const auto created = co_await store.create<account>([](account& value) { value.name = "occupied"; });
         static_cast<void>(created);
         const auto kind = mode == 0U ? 0x10U : mode == 1U ? 0x20U : 0x02U;
         auto extra = driver->first_key_with_kind(kind);
         BOOST_REQUIRE(extra.has_value());
         auto value = driver->record_value(*extra);
         BOOST_REQUIRE(value.has_value());
         // Keep only ONE authentic application/index/sequence record alongside
         // the header. Sequence0 is still persisted occupancy, never "missing".
         for (const auto& key : driver->keys()) {
            if (key != header_record_key())
               driver->erase_record(key);
         }
         if (mode == 2U)
            std::fill(value->begin(), value->end(), std::byte{0});
         if (mode == 3U) {
            extra = forge::db::object::system::access::record_key(
                forge::db::ids::object_id{.space = 0U, .type = 99U, .instance = 0U});
            value = std::vector<std::byte>{std::byte{0xffU}};
         } else if (mode == 4U) {
            extra = forge::db::core::record_key{std::vector<std::byte>{std::byte{0xffU}}};
            value = std::vector<std::byte>{std::byte{0xffU}};
         }
         driver->seed_record(*extra, *value);
         const auto before = driver->records();
         const auto mutations = driver->mutation_calls();
         const auto commits = driver->commit_calls();
         const auto scans = driver->scan_calls();
         auto active = co_await store.begin_transaction();
         BOOST_CHECK_THROW(co_await store.require_pristine(active), forge::db::object::exceptions::invalid_descriptor);
         BOOST_CHECK_EQUAL(driver->scan_calls(), scans + 1U);
         BOOST_CHECK(driver->records() == before);
         BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
         BOOST_CHECK_EQUAL(driver->commit_calls(), commits);
         co_await active.rollback();
      }());
   }
}

BOOST_AUTO_TEST_CASE(db_object_require_pristine_rejects_wrong_owner_closed_and_backend_policy_before_scan) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store = co_await make_store(driver);
      auto other = co_await forge::db::object::store::open(driver);
      auto active = co_await store.begin_transaction();
      const auto scans = driver->scan_calls();
      BOOST_CHECK_THROW(co_await other.require_pristine(active), forge::db::object::exceptions::invalid_descriptor);
      auto manual = forge::db::object::transaction{};
      BOOST_CHECK_THROW(co_await store.require_pristine(manual), forge::db::object::exceptions::invalid_descriptor);
      co_await active.rollback();
      BOOST_CHECK_THROW(co_await store.require_pristine(active), forge::db::object::exceptions::transaction_closed);
      BOOST_CHECK_EQUAL(driver->scan_calls(), scans);
      {
         auto raw = co_await driver->begin_transaction();
         auto custom = forge::db::object::transaction{
             raw, store.family(), [](forge::db::ids::object_id, std::type_index) {}, {}, {}};
         BOOST_CHECK_THROW(co_await store.require_pristine(custom), forge::db::object::exceptions::invalid_descriptor);
         co_await raw.rollback();
         BOOST_CHECK_EQUAL(driver->scan_calls(), scans);
      }
      auto backend = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      auto unsafe = co_await backend.begin_transaction();
      BOOST_CHECK_THROW(co_await backend.require_pristine(unsafe),
                        forge::db::object::exceptions::unsupported_operation);
      BOOST_CHECK_EQUAL(driver->scan_calls(), scans);
      co_await unsafe.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_require_pristine_revalidates_actual_header_without_normalizing) {
   auto runtime = forge::asio::runtime{};
   for (const auto malformed : {false, true}) {
      auto driver = std::make_shared<memory_driver>();
      forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
         auto store = co_await make_store(driver);
         driver->replace_record(header_record_key(),
                                malformed ? std::vector<std::byte>{std::byte{0xffU}} : packed_header(0U));
         const auto before = driver->records();
         const auto mutations = driver->mutation_calls();
         auto active = co_await store.begin_transaction();
         BOOST_CHECK_THROW(co_await store.require_pristine(active), forge::db::object::exceptions::invalid_header);
         BOOST_CHECK(driver->records() == before);
         BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
         co_await active.rollback();
      }());
   }
}

BOOST_AUTO_TEST_CASE(db_object_open_existing_rejects_missing_header_without_mutation) {
   auto runtime = forge::asio::runtime{};
   for (const auto populated : {false, true}) {
      auto driver = std::make_shared<memory_driver>();
      if (populated) {
         driver->seed_record(forge::db::core::record_key{std::vector<std::byte>{std::byte{0x10U}}}, {std::byte{0x41U}});
      }
      const auto before = driver->records();
      const auto mutations = driver->mutation_calls();
      const auto commits = driver->commit_calls();
      forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
         BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open_existing(
                               driver, forge::db::object::store::config{}, forge::db::object::store::options{})),
                           forge::db::object::exceptions::invalid_header);
         co_return;
      }());
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
      BOOST_CHECK_EQUAL(driver->commit_calls(), commits);
      BOOST_CHECK(!driver->record_value(header_record_key()).has_value());
   }
}

BOOST_AUTO_TEST_CASE(db_object_open_existing_preserves_current_records_and_allows_later_writes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto original = co_await forge::db::object::store::open(driver);
      original.register_object<db_object_tests::account_object>();
      auto value = db_object_tests::account{};
      value.id = db_object_tests::account::id_t{7U};
      value.name = "existing";
      value.balance = 42U;
      co_await original.insert(value);
      const auto before = driver->records();
      const auto mutations = driver->mutation_calls();
      const auto commits = driver->commit_calls();

      auto opened = co_await forge::db::object::store::open_existing(driver, forge::db::object::store::config{},
                                                                     forge::db::object::store::options{});
      opened.register_object<db_object_tests::account_object>();
      BOOST_CHECK(opened.header() == original.header());
      BOOST_CHECK((co_await opened.get(value.id)) == value);
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
      BOOST_CHECK_EQUAL(driver->commit_calls(), commits);

      co_await opened.modify(value.id, [](auto& stored) { ++stored.balance; });
      BOOST_CHECK_EQUAL((co_await opened.get(value.id)).balance, 43U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_open_existing_rejects_bad_headers_without_rewriting) {
   auto runtime = forge::asio::runtime{};
   for (const auto kind : {0U, 1U, 2U}) {
      auto driver = std::make_shared<memory_driver>();
      const auto encoded = kind == 0U
                               ? std::vector<std::byte>{std::byte{0xffU}}
                               : packed_header(kind == 1U ? 0U : forge::db::object::header::current_version + 1U);
      driver->seed_record(header_record_key(), encoded);
      const auto before = driver->records();
      const auto mutations = driver->mutation_calls();
      const auto commits = driver->commit_calls();
      forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
         if (kind == 0U) {
            BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open_existing(
                                  driver, forge::db::object::store::config{}, forge::db::object::store::options{})),
                              forge::db::object::exceptions::invalid_header);
         } else {
            BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open_existing(
                                  driver, forge::db::object::store::config{}, forge::db::object::store::options{})),
                              forge::db::object::exceptions::incompatible_version);
         }
         co_return;
      }());
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
      BOOST_CHECK_EQUAL(driver->commit_calls(), commits);
   }
}

BOOST_AUTO_TEST_CASE(db_object_store_open_creates_and_reads_system_header) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);

      const auto cached = store.header();
      BOOST_CHECK(cached.id == forge::db::object::header_id);
      BOOST_CHECK_EQUAL(cached.version, forge::db::object::header::current_version);

      const auto direct = co_await store.get(forge::db::object::header_id);
      BOOST_CHECK(direct == cached);

      auto transaction = co_await store.begin_transaction();
      BOOST_CHECK((co_await transaction.get(forge::db::object::header_id)) == cached);
      co_await transaction.rollback();

      auto snapshot = co_await store.begin_read();
      BOOST_CHECK((co_await snapshot.get(forge::db::object::header_id)) == cached);
      co_return;
   }());

   BOOST_CHECK_EQUAL(driver->record_count(), 1U);
   BOOST_CHECK_EQUAL(hex(header_record_key().bytes()), "0000000000000000000000");
   const auto persisted = driver->record_value(header_record_key());
   BOOST_REQUIRE(persisted.has_value());
   BOOST_CHECK_EQUAL(hex(*persisted), "000000000000000001000000");
}

BOOST_AUTO_TEST_CASE(db_object_store_reopen_preserves_single_header) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto first = co_await forge::db::object::store::open(driver);
      const auto second = co_await forge::db::object::store::open(driver);
      BOOST_CHECK(first.header() == second.header());
      co_return;
   }());

   BOOST_CHECK_EQUAL(driver->record_count(), 1U);
   BOOST_CHECK(driver->record_value(header_record_key()).has_value());
}

BOOST_AUTO_TEST_CASE(db_object_store_concurrent_open_creates_single_header) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto completed = std::make_shared<std::atomic_size_t>(0U);
      auto errors = std::make_shared<std::vector<std::exception_ptr>>(2U);
      auto versions = std::make_shared<std::array<std::uint32_t, 2U>>();
      const auto executor = co_await boost::asio::this_coro::executor;

      for (auto index = std::size_t{0}; index < 2U; ++index) {
         boost::asio::co_spawn(
             executor,
             [driver, completed, errors, versions, index]() -> boost::asio::awaitable<void> {
                try {
                   const auto store = co_await forge::db::object::store::open(driver);
                   (*versions)[index] = store.header().version;
                } catch (...) {
                   (*errors)[index] = std::current_exception();
                }
                ++*completed;
                co_return;
             },
             boost::asio::detached);
      }

      auto timer = boost::asio::steady_timer{executor};
      while (completed->load() != 2U) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      for (const auto& error : *errors) {
         if (error) {
            std::rethrow_exception(error);
         }
      }
      for (const auto version : *versions) {
         BOOST_CHECK_EQUAL(version, forge::db::object::header::current_version);
      }
      BOOST_CHECK(!driver->overlapping_writes());
      co_return;
   }());

   BOOST_CHECK_EQUAL(driver->record_count(), 1U);
}

BOOST_AUTO_TEST_CASE(db_object_store_rejects_non_empty_family_without_header) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   driver->seed_record(forge::db::core::record_key{std::vector<std::byte>{std::byte{0x10U}}}, {std::byte{0x01U}});

   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(driver)),
                        forge::db::object::exceptions::incompatible_version);
      co_return;
   }());
   BOOST_CHECK_EQUAL(driver->record_count(), 1U);
}

BOOST_AUTO_TEST_CASE(db_object_store_rejects_corrupt_and_incompatible_headers) {
   auto runtime = forge::asio::runtime{};

   auto corrupt = std::make_shared<memory_driver>();
   corrupt->seed_record(header_record_key(), {std::byte{0x01U}});
   forge::asio::blocking::run(runtime, [&corrupt]() -> boost::asio::awaitable<void> {
      BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(corrupt)),
                        forge::db::object::exceptions::invalid_header);
      co_return;
   }());

   for (const auto version : {std::uint32_t{0U}, forge::db::object::header::current_version + 1U}) {
      auto incompatible = std::make_shared<memory_driver>();
      incompatible->seed_record(header_record_key(), packed_header(version));
      forge::asio::blocking::run(runtime, [&incompatible]() -> boost::asio::awaitable<void> {
         BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(incompatible)),
                           forge::db::object::exceptions::incompatible_version);
         co_return;
      }());
      BOOST_CHECK(incompatible->record_value(header_record_key()) == packed_header(version));
   }
}

BOOST_AUTO_TEST_CASE(db_object_store_header_commit_failure_rolls_back_creation) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   driver->fail_next_commits(1U);

   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(driver)), std::runtime_error);
      co_return;
   }());
   BOOST_CHECK_EQUAL(driver->record_count(), 0U);

   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto store = co_await forge::db::object::store::open(driver);
      BOOST_CHECK_EQUAL(store.header().version, forge::db::object::header::current_version);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_materializes_object_record_key_from_base_id) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      co_await store.insert(make_account(42, "alice", 100, 3));
      co_return;
   }());

   const auto keys = driver->keys();
   BOOST_REQUIRE(!keys.empty());
   BOOST_CHECK(
       std::ranges::any_of(keys, [](const auto& key) { return hex(key.bytes()) == "10010007000000000000002a"; }));
}

BOOST_AUTO_TEST_CASE(db_object_store_materializes_tuple_composite_index_key) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      co_await store.insert(make_account(42, "alice", 100, 3));
      co_return;
   }());

   const auto keys = driver->keys();
   BOOST_REQUIRE_EQUAL(keys.size(), 5U);
   BOOST_CHECK(std::ranges::any_of(keys, [](const auto& key) {
      return hex(key.bytes()) == "210100070000000200ff00ff00ff03000000ff00ff00ff00ff00ff00ff00ff640000000000000000002a";
   }));
}

BOOST_AUTO_TEST_CASE(db_object_store_registers_objects_and_rejects_duplicate_registration) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      BOOST_CHECK_NO_THROW(store.register_object<account_object>());
      BOOST_CHECK_THROW(store.register_object<account_object>(), forge::db::object::exceptions::invalid_descriptor);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_accepts_owner_driver) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();
      co_await store.insert(make_account(42, "alice", 100, 3));
      auto loaded = co_await store.get(account::id_t{42});
      BOOST_CHECK_EQUAL(loaded.name, "alice");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_direct_api_autocommits_and_reads_indexes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));
      co_await store.insert(make_account(43, "bob", 50, 3));
      co_await store.insert(make_account(44, "carol", 75, 4));

      const auto alice = co_await store.get(account::id_t{42});
      BOOST_CHECK_EQUAL(alice.name, "alice");

      const auto found = co_await store.index<account_object, by_name>().find("bob");
      BOOST_REQUIRE(found.has_value());
      BOOST_CHECK_EQUAL(found->id.instance, 43U);

      const auto page = co_await store.index<account_object, by_region_balance>()
                            .equal_range(std::make_tuple(std::uint32_t{3}))
                            .page({.limit = 100});
      BOOST_REQUIRE_EQUAL(page.items.size(), 2U);
      BOOST_CHECK_EQUAL(page.items[0].name, "bob");
      BOOST_CHECK_EQUAL(page.items[1].name, "alice");

      co_await store.erase(account::id_t{43});
      BOOST_CHECK(!(co_await store.find(account::id_t{43})).has_value());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_composite_unique_rejects_duplicates_and_maintains_mutations) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<document_object>();

      co_await store.insert(make_document(1, 7, "alice@example.test", 100));
      co_await store.insert(make_document(2, 7, "bob@example.test", 50));
      const auto records_before_duplicate = driver->record_count();

      BOOST_CHECK_THROW(co_await store.insert(make_document(3, 7, "alice@example.test", 200)),
                        forge::db::object::exceptions::duplicate_object);
      BOOST_CHECK_EQUAL(driver->record_count(), records_before_duplicate);
      BOOST_CHECK(!(co_await store.find(document::id_t{3})).has_value());

      const auto variadic = co_await store.index<document_object, by_tenant_email>().find(
          std::uint32_t{7}, std::string_view{"alice@example.test"});
      const auto tuple = co_await store.index<document_object, by_tenant_email>().find(
          std::tuple{std::uint32_t{7}, std::string{"alice@example.test"}});
      BOOST_REQUIRE(variadic.has_value());
      BOOST_REQUIRE(tuple.has_value());
      BOOST_CHECK_EQUAL(variadic->id.instance, 1U);
      BOOST_CHECK_EQUAL(tuple->id.instance, variadic->id.instance);

      BOOST_CHECK_THROW(
          co_await store.modify(document::id_t{2}, [](document& value) { value.email = "alice@example.test"; }),
          forge::db::object::exceptions::duplicate_object);
      BOOST_CHECK_EQUAL((co_await store.get(document::id_t{2})).email, "bob@example.test");

      auto replacement = co_await store.get(document::id_t{2});
      replacement.email = "carol@example.test";
      co_await store.replace(replacement);
      BOOST_CHECK(!(co_await store.index<document_object, by_tenant_email>().find(std::uint32_t{7}, "bob@example.test"))
                       .has_value());
      BOOST_REQUIRE(
          (co_await store.index<document_object, by_tenant_email>().find(std::uint32_t{7}, "carol@example.test"))
              .has_value());

      co_await store.erase(document::id_t{1});
      co_await store.insert(make_document(4, 7, "alice@example.test", 300));
      BOOST_CHECK_EQUAL(
          (co_await store.index<document_object, by_tenant_email>().find(std::uint32_t{7}, "alice@example.test"))
              ->id.instance,
          4U);

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_document(5, 8, "rollback@example.test", 10));
      co_await tx.rollback();
      BOOST_CHECK(
          !(co_await store.index<document_object, by_tenant_email>().find(std::uint32_t{8}, "rollback@example.test"))
               .has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_composite_non_unique_orders_descending_and_supports_prefix_bounds) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<document_object>();

      co_await store.insert(make_document(9, 1, "nine", 100));
      co_await store.insert(make_document(3, 1, "three", 100));
      co_await store.insert(make_document(5, 1, "five", 50));
      co_await store.insert(make_document(7, 2, "seven", 999));

      const auto first_page =
          co_await store.index<document_object, by_tenant_rank>().equal_range(std::uint32_t{1}).page({.limit = 2});
      BOOST_REQUIRE_EQUAL(first_page.items.size(), 2U);
      BOOST_CHECK_EQUAL(first_page.items[0].id.instance, 3U);
      BOOST_CHECK_EQUAL(first_page.items[1].id.instance, 9U);
      BOOST_REQUIRE(first_page.next.has_value());

      const auto second_page = co_await store.index<document_object, by_tenant_rank>()
                                   .equal_range(std::uint32_t{1})
                                   .page({.after = first_page.next, .limit = 2});
      BOOST_REQUIRE_EQUAL(second_page.items.size(), 1U);
      BOOST_CHECK_EQUAL(second_page.items[0].id.instance, 5U);

      const auto duplicate =
          co_await store.index<document_object, by_tenant_rank>().find(std::uint32_t{1}, std::uint64_t{100});
      BOOST_REQUIRE(duplicate.has_value());
      BOOST_CHECK_EQUAL(duplicate->id.instance, 3U);

      const auto lower = co_await store.index<document_object, by_tenant_rank>()
                             .lower_bound(std::uint32_t{1}, std::uint64_t{100})
                             .page({.limit = 3});
      BOOST_REQUIRE_EQUAL(lower.items.size(), 3U);
      BOOST_CHECK_EQUAL(lower.items[0].id.instance, 3U);
      BOOST_CHECK_EQUAL(lower.items[1].id.instance, 9U);
      BOOST_CHECK_EQUAL(lower.items[2].id.instance, 5U);

      const auto upper = co_await store.index<document_object, by_tenant_rank>()
                             .upper_bound(std::tuple{std::uint32_t{1}})
                             .page({.limit = 1});
      BOOST_REQUIRE_EQUAL(upper.items.size(), 1U);
      BOOST_CHECK_EQUAL(upper.items[0].id.instance, 7U);

      auto stream =
          store.index<document_object, by_tenant_rank>().equal_range(std::uint32_t{1}).stream({.page_size = 1});
      BOOST_CHECK_EQUAL((co_await stream.next())->id.instance, 3U);
      BOOST_CHECK_EQUAL((co_await stream.next())->id.instance, 9U);
      BOOST_CHECK_EQUAL((co_await stream.next())->id.instance, 5U);
      BOOST_CHECK(!(co_await stream.next()).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_extractors_and_fixed_byte_sort_keys_are_typed) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<document_object>();
      const auto value = make_document(11, 4, "typed@example.test", 77, -9);
      co_await store.insert(value);

      BOOST_CHECK_EQUAL(
          (co_await store.index<document_object, by_external_key>().find(value.external_key))->id.instance, 11U);
      BOOST_CHECK_EQUAL((co_await store.index<document_object, by_digest>().find(value.digest))->id.instance, 11U);
      BOOST_CHECK_EQUAL(
          (co_await store.index<document_object, by_email_method>().find("typed@example.test"))->id.instance, 11U);
      BOOST_CHECK_EQUAL(
          (co_await store.index<document_object, by_rank_function>().find(std::uint64_t{77}))->id.instance, 11U);
      BOOST_CHECK_EQUAL((co_await store.index<document_object, by_score>().find(toy_ordered{-9}))->id.instance, 11U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_custom_sort_key_failure_is_typed_and_does_not_poison_transaction) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<document_object>();
      auto invalid = make_document(1, 1, "invalid", 1, std::numeric_limits<std::int64_t>::min());

      BOOST_CHECK_THROW(co_await store.insert(invalid), forge::db::object::exceptions::invalid_index_key);
      BOOST_CHECK_EQUAL(driver->record_count(), 1U);

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_document(2, 1, "before", 2, -1));
      BOOST_CHECK_THROW(co_await tx.insert(invalid), forge::db::object::exceptions::invalid_index_key);
      co_await tx.insert(make_document(3, 1, "after", 3, 1));
      co_await tx.commit();

      BOOST_REQUIRE((co_await store.find(document::id_t{2})).has_value());
      BOOST_REQUIRE((co_await store.find(document::id_t{3})).has_value());
      BOOST_CHECK(!(co_await store.find(document::id_t{1})).has_value());

      auto update = co_await store.begin_transaction();
      BOOST_CHECK_THROW(
          co_await update.modify(document::id_t{2},
                                 [](document& value) { value.score.value = std::numeric_limits<std::int64_t>::min(); }),
          forge::db::object::exceptions::invalid_index_key);
      co_await update.modify(document::id_t{3}, [](document& value) { value.score.value = 2; });
      co_await update.commit();
      BOOST_CHECK_EQUAL((co_await store.get(document::id_t{2})).score.value, -1);
      BOOST_CHECK_EQUAL((co_await store.get(document::id_t{3})).score.value, 2);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_generates_monotonic_ids_and_returns_object) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto alice = co_await store.create<account>([](account& value) {
         value.name = "alice";
         value.balance = 100;
         value.region = 3;
      });
      auto bob = co_await store.create<account>([](account& value) {
         value.name = "bob";
         value.balance = 50;
         value.region = 4;
      });

      BOOST_CHECK_EQUAL(alice.id.instance, 0U);
      BOOST_CHECK_EQUAL(bob.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await store.get(alice.id)).name, "alice");
      BOOST_CHECK_EQUAL((co_await store.get(bob.id)).name, "bob");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_does_not_reuse_deleted_ids) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto alice = co_await store.create<account>([](account& value) { value.name = "alice"; });
      co_await store.erase(alice.id);

      auto bob = co_await store.create<account>([](account& value) { value.name = "bob"; });

      BOOST_CHECK_EQUAL(alice.id.instance, 0U);
      BOOST_CHECK_EQUAL(bob.id.instance, 1U);
      BOOST_CHECK(!(co_await store.find(alice.id)).has_value());
      BOOST_CHECK_EQUAL((co_await store.get(bob.id)).name, "bob");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_skips_existing_manual_primary_ids) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(0, "manual", 1, 1));
      auto generated = co_await store.create<account>([](account& value) { value.name = "generated"; });

      BOOST_CHECK_EQUAL(generated.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await store.get(generated.id)).name, "generated");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_failure_consumes_id_without_persisting_object) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto alice = co_await store.create<account>([](account& value) { value.name = "alice"; });
      BOOST_CHECK_THROW(co_await store.create<account>([](account& value) { value.name = "alice"; }),
                        forge::db::object::exceptions::duplicate_object);

      auto bob = co_await store.create<account>([](account& value) { value.name = "bob"; });

      BOOST_CHECK_EQUAL(alice.id.instance, 0U);
      BOOST_CHECK_EQUAL(bob.id.instance, 2U);
      BOOST_CHECK(!(co_await store.find(account::id_t{1})).has_value());
      BOOST_CHECK_EQUAL((co_await store.get(bob.id)).name, "bob");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_direct_create_commit_failure_rolls_back_releases_writer_and_seals_id) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);
         driver->fail_next_commits(1);
         BOOST_CHECK_THROW(co_await store.create<account>([](account& value) { value.name = "commit-fails"; }),
                           std::runtime_error);
      }

      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      BOOST_CHECK_GE(driver->commit_calls(), 2U);

      auto reopened = co_await make_store(driver);
      auto committed = co_await reopened.create<account>([](account& value) { value.name = "after-commit-failure"; });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await reopened.get(committed.id)).name, "after-commit-failure");
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_transaction_rollback_consumes_id) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto tx = co_await store.begin_transaction();
      auto draft = co_await tx.create<account>([](account& value) { value.name = "draft"; });
      BOOST_CHECK_EQUAL(draft.id.instance, 0U);
      co_await tx.rollback();

      BOOST_CHECK(!(co_await store.find(draft.id)).has_value());

      auto committed = co_await store.create<account>([](account& value) { value.name = "committed"; });
      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await store.get(committed.id)).name, "committed");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transactional_allocation_trace_reads_zero_and_preserves_skipped_ids) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto options =
          forge::db::object::store::options{.id_allocation = forge::db::object::id_allocation_policy::transactional};
      auto store = co_await make_store(driver, options);
      const auto records = driver->record_count();
      {
         auto view = co_await store.begin_read();
         BOOST_CHECK_EQUAL(co_await view.allocation_next<account_object>(), 0U);
      }
      BOOST_CHECK_EQUAL(driver->record_count(), records);
      driver->seed_record(
          forge::db::core::record_key{std::vector<std::byte>{std::byte{2}, std::byte{1}, std::byte{0}, std::byte{7}}},
          std::vector<std::byte>(8U));
      auto observer = std::make_shared<counting_observer>();
      store.add_observer(observer);
      auto tx = co_await store.begin_transaction();
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 0U);
      co_await tx.insert(make_account(0, "reserved", 0, 0));
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 0U);
      const auto created = co_await tx.create<account>([](auto& value) { value.name = "allocated"; });
      BOOST_CHECK_EQUAL(created.id.instance, 1U);
      const auto alias = co_await store.join(tx);
      BOOST_CHECK_EQUAL(co_await alias.allocation_next<account_object>(), 2U);
      BOOST_CHECK(tx.tracks_allocations());
      BOOST_CHECK(alias.tracks_allocations());
      const auto changes = tx.projected_changes();
      BOOST_REQUIRE_EQUAL(changes.allocations.size(), 1U);
      BOOST_CHECK_EQUAL(changes.allocations.front().type.instance, 0U);
      BOOST_CHECK_EQUAL(changes.allocations.front().before, 0U);
      BOOST_CHECK_EQUAL(changes.allocations.front().after, 2U);
      co_await tx.commit();
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK(observer->last->allocations == changes.allocations);

      auto erased = co_await store.begin_transaction();
      co_await erased.erase(created.id);
      co_await erased.erase(account::id_t{0});
      BOOST_CHECK(erased.projected_changes().allocations.empty());
      BOOST_CHECK_EQUAL(co_await erased.allocation_next<account_object>(), 2U);
      co_await erased.commit();
      auto reopened = co_await make_store(driver, options);
      auto view = co_await reopened.begin_read();
      BOOST_CHECK_EQUAL(co_await view.allocation_next<account_object>(), 2U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_counter_only_failed_initializer_and_savepoint_trace_are_transactional) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      auto observer = std::make_shared<counting_observer>();
      store.add_observer(observer);
      auto tx = co_await store.begin_transaction();
      BOOST_CHECK_THROW(co_await tx.create<account>([](auto&) { throw std::runtime_error{"initializer"}; }),
                        std::runtime_error);
      BOOST_CHECK(tx.projected_changes().mutations.empty());
      BOOST_CHECK(!tx.projected_changes().empty());
      BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 1U);
      const auto point = co_await tx.db_transaction().create_savepoint();
      auto alias = co_await store.join(tx);
      const auto discarded = co_await alias.create<account>([](auto& value) { value.name = "discarded"; });
      BOOST_CHECK_EQUAL(discarded.id.instance, 1U);
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 1U);
      BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
      co_await tx.commit();
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK(observer->last->mutations.empty());
      BOOST_REQUIRE_EQUAL(observer->last->allocations.size(), 1U);
      auto next = co_await store.begin_transaction();
      const auto reused = co_await next.create<account>([](auto& value) { value.name = "reused"; });
      BOOST_CHECK_EQUAL(reused.id.instance, 1U);
      co_await next.rollback();
      auto view = co_await store.begin_read();
      BOOST_CHECK_EQUAL(co_await view.allocation_next<account_object>(), 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_projection_rejects_later_counter_only_change) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      auto tx = co_await store.begin_transaction();
      auto observer = std::make_shared<recording_precommit_observer>();
      tx.add_precommit_observer(observer);
      tx.db_transaction().before_commit([&]() -> boost::asio::awaitable<void> {
         try {
            (void)co_await tx.create<account>([](auto&) { throw std::runtime_error{"late initializer"}; });
         } catch (const std::runtime_error&) {
         }
      });
      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::object::exceptions::stale_precommit_projection);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK(observer->last->empty());
      co_await tx.rollback();
      auto view = co_await store.begin_read();
      BOOST_CHECK_EQUAL(co_await view.allocation_next<account_object>(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_allocation_alias_overlap_is_rejected_before_second_write) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      const auto strand = boost::asio::make_strand(co_await boost::asio::this_coro::executor);
      auto exercise = [&]() -> boost::asio::awaitable<void> {
         auto store =
             co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
         auto tx = co_await store.begin_transaction();
         auto alias = co_await store.join(tx);
         auto pause = std::make_shared<pausing_allocation_participant>(strand);
         tx.db_transaction().attach_participant(pause);
         auto first = [&]() -> boost::asio::awaitable<void> {
            const auto value = co_await tx.create<account>([](auto& row) { row.name = "first"; });
            BOOST_CHECK_EQUAL(value.id.instance, 0U);
         };
         auto second = [&]() -> boost::asio::awaitable<void> {
            co_await pause->wait_entered();
            BOOST_CHECK_EQUAL(pause->sequence_writes, 0U);
            auto rejected = false;
            try {
               (void)co_await alias.create<account>([](auto& row) { row.name = "overlap"; });
            } catch (const forge::db::core::exceptions::participant_conflict&) {
               rejected = true;
            }
            pause->release();
            BOOST_CHECK(rejected);
         };
         using namespace boost::asio::experimental::awaitable_operators;
         co_await (first() && second());
         BOOST_CHECK_EQUAL(pause->sequence_writes, 1U);
         BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
         const auto next = co_await alias.create<account>([](auto& row) { row.name = "after"; });
         BOOST_CHECK_EQUAL(next.id.instance, 1U);
         BOOST_CHECK_EQUAL(pause->sequence_writes, 2U);
         BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 2U);
         pause->fail_sequence = true;
         BOOST_CHECK_THROW(co_await alias.create<account>([](auto& row) { row.name = "vetoed"; }), std::runtime_error);
         BOOST_CHECK_EQUAL(co_await alias.allocation_next<account_object>(), 2U);
         BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 2U);
         const auto retried = co_await alias.create<account>([](auto& row) { row.name = "retried"; });
         BOOST_CHECK_EQUAL(retried.id.instance, 2U);
         BOOST_CHECK_EQUAL(pause->sequence_writes, 3U);
         BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 3U);
         co_await tx.commit();
      };
      co_await boost::asio::co_spawn(strand, exercise(), boost::asio::use_awaitable);
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transactional_id_allocation_reuses_rolled_back_id) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto options = forge::db::object::store::options{
          .writes = forge::db::object::write_policy::single_writer,
          .id_allocation = forge::db::object::id_allocation_policy::transactional,
      };
      auto store = co_await make_store(driver, options);

      auto tx = co_await store.begin_transaction();
      auto draft = co_await tx.create<account>([](account& value) { value.name = "draft"; });
      BOOST_CHECK_EQUAL(draft.id.instance, 0U);
      co_await tx.rollback();

      auto reopened = co_await make_store(driver, options);
      auto committed = co_await reopened.create<account>([](account& value) { value.name = "committed"; });
      BOOST_CHECK_EQUAL(committed.id.instance, 0U);
      BOOST_CHECK_EQUAL((co_await reopened.get(committed.id)).name, "committed");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_savepoint_rollback_restores_records_indexes_and_observer_input) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<counting_observer>();
      store.add_observer(observer);

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_account(42, "kept", 100, 3));
      const auto point = co_await tx.db_transaction().create_savepoint();
      co_await tx.insert(make_account(43, "rolled-back", 50, 4));
      co_await tx.db_transaction().rollback_to_savepoint(point);
      co_await tx.commit();

      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "kept");
      BOOST_CHECK(!(co_await store.find(account::id_t{43})).has_value());
      BOOST_CHECK_EQUAL(observer->calls, 1U);
      BOOST_CHECK_EQUAL(observer->mutation_count, 1U);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK_EQUAL(observer->last->mutations.front().id.instance, 42U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_observer_sees_final_savepoint_changes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<recording_precommit_observer>();

      auto tx = co_await store.begin_transaction();
      tx.add_precommit_observer(observer);
      co_await tx.insert(make_account(42, "kept", 100, 3));
      const auto point = co_await tx.db_transaction().create_savepoint();
      co_await tx.insert(make_account(43, "rolled-back", 50, 4));
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(observer->calls, 0U);
      const auto projected = tx.projected_changes();
      BOOST_REQUIRE_EQUAL(projected.mutations.size(), 1U);
      BOOST_CHECK_EQUAL(projected.mutations.front().id.instance, 42U);
      co_await tx.commit();

      BOOST_CHECK_EQUAL(observer->calls, 1U);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_REQUIRE_EQUAL(observer->last->mutations.size(), 1U);
      BOOST_CHECK_EQUAL(observer->last->mutations.front().id.instance, 42U);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "kept");
      BOOST_CHECK(!(co_await store.find(account::id_t{43})).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_observer_writes_final_post_savepoint_transaction) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto committed = std::make_shared<counting_observer>();
      auto observer = std::make_shared<writing_precommit_observer>();
      store.add_observer(committed);

      auto tx = co_await store.begin_transaction();
      observer->write = [&tx]() -> boost::asio::awaitable<void> {
         const auto kept = co_await tx.get(account::id_t{42});
         BOOST_CHECK_EQUAL(kept.name, "kept");
         co_await tx.insert(make_account(44, "from-precommit", 75, 5));
         co_return;
      };
      tx.add_precommit_observer(observer);
      co_await tx.insert(make_account(42, "kept", 100, 3));
      const auto point = co_await tx.db_transaction().create_savepoint();
      co_await tx.insert(make_account(43, "rolled-back", 50, 4));
      co_await tx.db_transaction().rollback_to_savepoint(point);
      co_await tx.commit();

      BOOST_CHECK_EQUAL(observer->calls, 1U);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_REQUIRE_EQUAL(observer->last->mutations.size(), 1U);
      BOOST_CHECK_EQUAL(observer->last->mutations.front().id.instance, 42U);
      BOOST_CHECK_EQUAL(committed->calls, 1U);
      BOOST_CHECK_EQUAL(committed->mutation_count, 2U);
      BOOST_REQUIRE(committed->last.has_value());
      BOOST_REQUIRE_EQUAL(committed->last->mutations.size(), 2U);
      BOOST_CHECK_EQUAL(committed->last->mutations.back().id.instance, 44U);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "kept");
      BOOST_CHECK(!(co_await store.find(account::id_t{43})).has_value());
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{44})).name, "from-precommit");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_observers_compose_ordered_writes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = std::make_shared<recording_precommit_observer>();
      auto second = std::make_shared<writing_precommit_observer>();
      auto committed = std::make_shared<counting_observer>();
      store.add_observer(committed);

      auto tx = co_await store.begin_transaction();
      second->write = [&tx]() -> boost::asio::awaitable<void> {
         co_await tx.insert(make_account(43, "second-observer", 50, 4));
      };
      tx.add_precommit_observer(first);
      tx.add_precommit_observer(second);
      co_await tx.insert(make_account(42, "initial", 100, 3));
      co_await tx.commit();

      BOOST_REQUIRE(first->last.has_value());
      BOOST_REQUIRE_EQUAL(first->last->mutations.size(), 1U);
      BOOST_REQUIRE(second->last.has_value());
      BOOST_REQUIRE_EQUAL(second->last->mutations.size(), 1U);
      BOOST_CHECK_EQUAL(committed->mutation_count, 2U);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "initial");
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{43})).name, "second-observer");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_projection_rejects_later_core_hook_mutations) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<recording_precommit_observer>();

      auto tx = co_await store.begin_transaction();
      tx.add_precommit_observer(observer);
      tx.db_transaction().before_commit(
          [&tx]() -> boost::asio::awaitable<void> { co_await tx.insert(make_account(43, "late-core-hook", 50, 4)); });
      co_await tx.insert(make_account(42, "projected", 100, 3));

      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::object::exceptions::stale_precommit_projection);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_REQUIRE_EQUAL(observer->last->mutations.size(), 1U);
      BOOST_CHECK_EQUAL(observer->last->mutations.front().id.instance, 42U);
      co_await tx.rollback();

      BOOST_CHECK(!(co_await store.find(account::id_t{42})).has_value());
      BOOST_CHECK(!(co_await store.find(account::id_t{43})).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_observer_rejects_reentrant_registration) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto nested = std::make_shared<recording_precommit_observer>();
      auto observer = std::make_shared<reentrant_precommit_observer>();

      auto tx = co_await store.begin_transaction();
      observer->transaction = std::addressof(tx);
      observer->nested = nested;
      tx.add_precommit_observer(observer);
      co_await tx.insert(make_account(42, "kept", 100, 3));
      co_await tx.commit();

      BOOST_TEST(observer->rejected);
      BOOST_CHECK_EQUAL(nested->calls, 0U);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "kept");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_precommit_observer_rejects_reentrant_completion) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<reentrant_completion_observer>();

      auto tx = co_await store.begin_transaction();
      observer->transaction = std::addressof(tx.db_transaction());
      tx.add_precommit_observer(observer);
      co_await tx.insert(make_account(42, "kept", 100, 3));
      co_await tx.commit();

      BOOST_TEST(observer->commit_rejected);
      BOOST_TEST(observer->rollback_rejected);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "kept");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_shared_transaction_supports_distinct_store_families) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto first = co_await forge::db::object::store::open(
          driver, forge::db::object::store::config{.family = forge::db::core::family{"objects.first"}});
      auto second = co_await forge::db::object::store::open(
          driver, forge::db::object::store::config{.family = forge::db::core::family{"objects.second"}});
      first.register_object<account_object>();
      second.register_object<account_object>();

      auto first_observer = std::make_shared<counting_observer>();
      auto second_observer = std::make_shared<counting_observer>();
      first.add_observer(first_observer);
      second.add_observer(second_observer);

      auto shared = co_await driver->begin_transaction();
      auto first_tx = co_await first.join(shared);
      auto second_tx = co_await second.join(shared);

      co_await first_tx.insert(make_account(10, "first-kept", 10, 1));
      const auto point = co_await shared.create_savepoint();
      co_await first_tx.insert(make_account(11, "first-rolled-back", 11, 1));
      co_await second_tx.insert(make_account(20, "second-rolled-back", 20, 2));
      co_await shared.rollback_to_savepoint(point);
      co_await second_tx.insert(make_account(21, "second-kept", 21, 2));
      co_await shared.commit();

      BOOST_CHECK_EQUAL((co_await first.get(account::id_t{10})).name, "first-kept");
      BOOST_CHECK(!(co_await first.find(account::id_t{11})).has_value());
      BOOST_CHECK(!(co_await second.find(account::id_t{20})).has_value());
      BOOST_CHECK_EQUAL((co_await second.get(account::id_t{21})).name, "second-kept");
      BOOST_CHECK_EQUAL(first_observer->calls, 1U);
      BOOST_CHECK_EQUAL(first_observer->mutation_count, 1U);
      BOOST_CHECK_EQUAL(second_observer->calls, 1U);
      BOOST_CHECK_EQUAL(second_observer->mutation_count, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_shared_transaction_rejects_duplicate_store_family) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto config = forge::db::object::store::config{
          .family = forge::db::core::family{"objects.shared"},
      };
      auto first = co_await forge::db::object::store::open(driver, config);
      auto duplicate = co_await forge::db::object::store::open(driver, config);

      auto shared = co_await driver->begin_transaction();
      auto first_tx = co_await first.join(shared);
      BOOST_CHECK_THROW(co_await duplicate.join(shared), forge::db::core::exceptions::participant_conflict);
      co_await shared.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_join_attach_failure_releases_writer_gate) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto invalid = co_await driver->begin_transaction();
      co_await invalid.put(forge::db::core::family{"unrelated"},
                           forge::db::core::record_key{std::vector<std::byte>{std::byte{0x01}}},
                           std::vector<std::byte>{std::byte{0x02}});

      BOOST_CHECK_THROW(co_await store.join(invalid), forge::db::core::exceptions::participant_conflict);
      co_await invalid.rollback();

      auto valid = co_await driver->begin_transaction();
      auto joined = co_await store.join(valid);
      co_await valid.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_serializes_joined_core_transactions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first_core = co_await driver->begin_transaction();
      auto first = co_await store.join(first_core);

      auto second_started = std::make_shared<std::atomic_bool>(false);
      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [driver, store, second_started, second_finished, second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto second_core = co_await driver->begin_transaction();
                auto second = co_await store.join(second_core);
                second_started->store(true, std::memory_order_release);
                co_await second_core.rollback();
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);
      BOOST_CHECK(!second_started->load(std::memory_order_acquire));

      co_await first_core.rollback();
      for (auto attempt = 0; attempt != 100 && !second_finished->load(std::memory_order_acquire); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(second_started->load(std::memory_order_acquire));
      BOOST_CHECK(second_finished->load(std::memory_order_acquire));
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_backend_policy_does_not_serialize_joined_core_transactions) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<account_object>();

      auto first_core = co_await driver->begin_transaction();
      auto second_core = co_await driver->begin_transaction();
      auto first = co_await store.join(first_core);
      auto second = co_await store.join(second_core);

      co_await first_core.rollback();
      co_await second_core.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_joined_facade_reuses_store_participant_without_commit_ownership) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto owner = co_await store.begin_transaction();
      auto joined = co_await store.join(owner);

      BOOST_CHECK_THROW(co_await joined.commit(), forge::db::object::exceptions::unsupported_operation);
      BOOST_CHECK_THROW(co_await joined.rollback(), forge::db::object::exceptions::unsupported_operation);
      co_await joined.insert(make_account(77, "joined", 10, 1));
      co_await owner.commit();

      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{77})).name, "joined");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_cancelled_join_does_not_lose_writer_gate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first_core = co_await driver->begin_transaction();
      auto first = co_await store.join(first_core);

      auto waiting = std::make_shared<std::atomic_bool>(false);
      auto cancelled = std::make_shared<std::atomic_bool>(false);
      auto finished = std::make_shared<std::atomic_bool>(false);
      auto error = std::make_shared<std::exception_ptr>();
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [driver, store, waiting, cancelled, finished, error]() mutable -> boost::asio::awaitable<void> {
             try {
                co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
                auto second_core = co_await driver->begin_transaction();
                waiting->store(true, std::memory_order_release);
                auto second = co_await store.join(second_core);
                co_await second_core.rollback();
             } catch (const forge::asio::exceptions::canceled&) {
                cancelled->store(true, std::memory_order_release);
             } catch (...) {
                *error = std::current_exception();
             }
             finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      while (!waiting->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      timer.expires_after(std::chrono::milliseconds{25});
      co_await timer.async_wait(boost::asio::use_awaitable);
      cancellation->emit(boost::asio::cancellation_type::all);

      for (auto attempt = 0; attempt != 100 && !finished->load(std::memory_order_acquire); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      if (*error) {
         std::rethrow_exception(*error);
      }
      BOOST_CHECK(cancelled->load(std::memory_order_acquire));
      BOOST_CHECK(finished->load(std::memory_order_acquire));

      co_await first_core.rollback();
      auto third_core = co_await driver->begin_transaction();
      auto third = co_await store.join(third_core);
      co_await third_core.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_savepoint_rollback_consumes_generated_ids_across_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);
         auto tx = co_await store.begin_transaction();
         auto kept = co_await tx.create<account>([](account& value) { value.name = "kept"; });
         const auto point = co_await tx.db_transaction().create_savepoint();
         auto discarded = co_await tx.create<account>([](account& value) { value.name = "discarded"; });
         BOOST_CHECK_EQUAL(kept.id.instance, 0U);
         BOOST_CHECK_EQUAL(discarded.id.instance, 1U);
         co_await tx.db_transaction().rollback_to_savepoint(point);
         co_await tx.commit();
      }

      auto reopened = co_await make_store(driver);
      auto next = co_await reopened.create<account>([](account& value) { value.name = "next"; });
      BOOST_CHECK_EQUAL(next.id.instance, 2U);
      BOOST_CHECK(!(co_await reopened.find(account::id_t{1})).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transactional_id_allocation_reuses_savepoint_id_across_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      const auto options = forge::db::object::store::options{
          .writes = forge::db::object::write_policy::single_writer,
          .id_allocation = forge::db::object::id_allocation_policy::transactional,
      };
      {
         auto store = co_await make_store(driver, options);
         auto tx = co_await store.begin_transaction();
         auto kept = co_await tx.create<account>([](account& value) { value.name = "kept"; });
         const auto point = co_await tx.db_transaction().create_savepoint();
         auto discarded = co_await tx.create<account>([](account& value) { value.name = "discarded"; });
         BOOST_CHECK_EQUAL(kept.id.instance, 0U);
         BOOST_CHECK_EQUAL(discarded.id.instance, 1U);
         co_await tx.db_transaction().rollback_to_savepoint(point);
         co_await tx.commit();
      }

      auto reopened = co_await make_store(driver, options);
      auto next = co_await reopened.create<account>([](account& value) { value.name = "next"; });
      BOOST_CHECK_EQUAL(next.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await reopened.get(next.id)).name, "next");
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_transaction_rollback_seals_id_across_store_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);

         auto tx = co_await store.begin_transaction();
         auto draft = co_await tx.create<account>([](account& value) { value.name = "draft"; });
         BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         co_await tx.rollback();
      }

      auto reopened = co_await make_store(driver);
      auto committed = co_await reopened.create<account>([](account& value) { value.name = "committed"; });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK_EQUAL((co_await reopened.get(committed.id)).name, "committed");
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_owned_transaction_drop_seals_id_across_store_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);
         {
            auto tx = co_await store.begin_transaction();
            auto draft = co_await tx.create<account>([](account& value) { value.name = "dropped"; });
            BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         }
      }

      auto reopened = co_await make_store(driver);
      auto committed = co_await reopened.create<account>([](account& value) { value.name = "after-dropped-rollback"; });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_owned_transaction_drop_reopen_waits_for_allocation_seal) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      driver->block_rollbacks(true);
      {
         auto store = co_await make_store(driver);
         {
            auto tx = co_await store.begin_transaction();
            auto draft = co_await tx.create<account>([](account& value) { value.name = "blocked-drop"; });
            BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         }
      }

      const auto executor = co_await boost::asio::this_coro::executor;
      auto timer = boost::asio::steady_timer{executor};
      for (auto attempt = 0; attempt != 100 && !driver->rollback_started(); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      BOOST_REQUIRE(driver->rollback_started());

      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_instance = std::make_shared<std::optional<std::uint64_t>>();
      auto second_error = std::make_shared<std::exception_ptr>();
      boost::asio::co_spawn(
          executor,
          [driver, second_finished, second_instance, second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto reopened = co_await make_store(driver);
                auto committed =
                    co_await reopened.create<account>([](account& value) { value.name = "after-blocked-drop"; });
                *second_instance = committed.id.instance;
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::detached);

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);
      BOOST_CHECK(!second_finished->load(std::memory_order_acquire));
      BOOST_CHECK_EQUAL(driver->active_writes(), 1U);
      BOOST_CHECK(!driver->overlapping_writes());

      driver->block_rollbacks(false);
      for (auto attempt = 0; attempt != 200 && !second_finished->load(std::memory_order_acquire); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_REQUIRE(second_finished->load(std::memory_order_acquire));
      BOOST_REQUIRE(second_instance->has_value());
      BOOST_CHECK_EQUAL(**second_instance, 1U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_owned_transaction_drop_rollback_failure_seals_id_across_store_reopen) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      driver->fail_rollbacks(true);
      {
         auto store = co_await make_store(driver);
         {
            auto tx = co_await store.begin_transaction();
            auto draft = co_await tx.create<account>([](account& value) { value.name = "rollback-fails"; });
            BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         }
      }

      const auto executor = co_await boost::asio::this_coro::executor;
      auto timer = boost::asio::steady_timer{executor};
      for (auto attempt = 0; attempt != 100 && !driver->rollback_started(); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      BOOST_REQUIRE(driver->rollback_started());

      auto reopened = co_await make_store(driver);
      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_cancelled = std::make_shared<std::atomic_bool>(false);
      auto second_instance = std::make_shared<std::optional<std::uint64_t>>();
      auto second_error = std::make_shared<std::exception_ptr>();
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
      boost::asio::co_spawn(
          executor,
          [reopened, second_finished, second_cancelled, second_instance,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto committed =
                    co_await reopened.create<account>([](account& value) { value.name = "after-rollback-failure"; });
                *second_instance = committed.id.instance;
             } catch (const forge::asio::exceptions::canceled&) {
                second_cancelled->store(true, std::memory_order_release);
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      for (auto attempt = 0; attempt != 100 && !second_finished->load(std::memory_order_acquire); ++attempt) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }
      if (!second_finished->load(std::memory_order_acquire)) {
         cancellation->emit(boost::asio::cancellation_type::all);
         for (auto attempt = 0; attempt != 100 && !second_finished->load(std::memory_order_acquire); ++attempt) {
            timer.expires_after(std::chrono::milliseconds{1});
            co_await timer.async_wait(boost::asio::use_awaitable);
         }
      }

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_REQUIRE(second_finished->load(std::memory_order_acquire));
      BOOST_CHECK(!second_cancelled->load(std::memory_order_acquire));
      BOOST_REQUIRE(second_instance->has_value());
      BOOST_CHECK_EQUAL(**second_instance, 1U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_joined_transaction_rollback_consumes_id) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto shared = co_await driver->begin_transaction();
      auto object_tx = co_await store.join(shared);
      auto draft = co_await object_tx.create<account>([](account& value) { value.name = "joined"; });
      BOOST_CHECK_EQUAL(draft.id.instance, 0U);
      co_await shared.rollback();

      BOOST_CHECK(!(co_await store.find(draft.id)).has_value());

      auto committed = co_await store.create<account>([](account& value) { value.name = "after-join-rollback"; });
      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_joined_transaction_rollback_seals_id_across_store_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);

         auto shared = co_await driver->begin_transaction();
         auto object_tx = co_await store.join(shared);
         auto draft = co_await object_tx.create<account>([](account& value) { value.name = "joined"; });
         BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         co_await shared.rollback();
      }

      auto reopened = co_await make_store(driver);
      auto committed = co_await reopened.create<account>([](account& value) { value.name = "after-join-rollback"; });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_joined_transaction_drop_before_rollback_seals_id_across_store_reopen) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      {
         auto store = co_await make_store(driver);
         auto shared = co_await driver->begin_transaction();
         {
            auto object_tx = co_await store.join(shared);
            auto draft = co_await object_tx.create<account>([](account& value) { value.name = "joined"; });
            BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         }
         co_await shared.rollback();
      }

      auto reopened = co_await make_store(driver);
      auto committed =
          co_await reopened.create<account>([](account& value) { value.name = "after-dropped-join-rollback"; });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_concurrent_calls_generate_unique_ids) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      constexpr auto count = std::size_t{12};
      auto completed = std::make_shared<std::atomic_size_t>(0);
      auto instances = std::make_shared<std::vector<std::uint64_t>>();
      auto lock = std::make_shared<std::mutex>();
      auto error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;

      for (auto index = std::size_t{0}; index < count; ++index) {
         boost::asio::co_spawn(
             executor,
             [store, completed, instances, lock, error, index]() mutable -> boost::asio::awaitable<void> {
                try {
                   auto created = co_await store.create<account>(
                       [index](account& value) { value.name = "user-" + std::to_string(index); });
                   {
                      auto guard = std::scoped_lock{*lock};
                      instances->push_back(created.id.instance);
                   }
                } catch (...) {
                   auto guard = std::scoped_lock{*lock};
                   if (!*error) {
                      *error = std::current_exception();
                   }
                }
                completed->fetch_add(1, std::memory_order_release);
                co_return;
             },
             boost::asio::detached);
      }

      auto timer = boost::asio::steady_timer{executor};
      for (auto attempts = 0; attempts < 200 && completed->load(std::memory_order_acquire) != count; ++attempts) {
         timer.expires_after(std::chrono::milliseconds{5});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*error) {
         std::rethrow_exception(*error);
      }
      BOOST_REQUIRE_EQUAL(completed->load(std::memory_order_acquire), count);

      std::sort(instances->begin(), instances->end());
      BOOST_REQUIRE_EQUAL(instances->size(), count);
      for (auto index = std::size_t{0}; index < count; ++index) {
         BOOST_CHECK_EQUAL((*instances)[index], index);
      }

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_does_not_open_nested_write_transaction) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      const auto created = co_await store.create<account>([](account& value) { value.name = "single-writer-safe"; });

      BOOST_CHECK_EQUAL(created.id.instance, 0U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transaction_typed_id_create_find_and_erase) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto tx = co_await store.begin_transaction();
      const auto created = co_await tx.create<account>([](account& value) {
         value.name = "alice";
         value.balance = 100;
         value.region = 3;
      });
      const auto found = co_await tx.find(created.id);
      BOOST_REQUIRE(found.has_value());
      BOOST_CHECK_EQUAL(found->name, "alice");

      co_await tx.erase(created.id);
      BOOST_CHECK(!(co_await tx.find(created.id)).has_value());
      co_await tx.commit();

      BOOST_CHECK(!(co_await store.find(created.id)).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transaction_create_rejects_allocator_id_for_another_type) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto active = co_await driver->begin_transaction();
      auto tx = forge::db::object::transaction{
          std::move(active),
          forge::db::core::family{"objectdb"},
          [](forge::db::ids::object_id, std::type_index) {},
          [](forge::db::ids::object_id, forge::db::core::transaction&)
              -> boost::asio::awaitable<forge::db::ids::object_id> {
             co_return forge::db::ids::object_id{
                 .space = account::space,
                 .type = static_cast<std::uint16_t>(account::type + 1U),
                 .instance = 42,
             };
          },
          {},
          {},
          [] {},
      };

      BOOST_CHECK_THROW(co_await tx.create<account>([](account&) {}),
                        forge::db::object::exceptions::invalid_descriptor);
      co_await tx.rollback();
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transaction_groups_mutations_and_requires_commit) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_account(42, "alice", 100, 3));
      BOOST_CHECK(!(co_await store.find(account::id_t{42})).has_value());
      co_await tx.commit();
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "alice");

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_transaction_destruction_discards_uncommitted_changes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      {
         auto tx = co_await store.begin_transaction();
         co_await tx.insert(make_account(42, "alice", 100, 3));
      }

      BOOST_CHECK(!(co_await store.find(account::id_t{42})).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_dropped_transaction_invokes_backend_rollback) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<session_driver<drop_sensitive_session>>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      {
         auto tx = co_await store.begin_transaction();
      }

      auto next = co_await store.begin_transaction();
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 1U);
      BOOST_CHECK_EQUAL(driver->destroyed_without_finish(), 0U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 1U);

      co_await next.rollback();
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 2U);
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_dropped_transaction_releases_writer_after_rollback_failure) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<session_driver<throwing_rollback_session>>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      {
         auto tx = co_await store.begin_transaction();
      }

      auto next = co_await store.begin_transaction();
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 1U);
      BOOST_CHECK_EQUAL(driver->destroyed_without_finish(), 0U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 1U);

      co_await next.commit();
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_explicit_rollback_failure_releases_writer_lane) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<session_driver<throwing_rollback_session>>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      auto tx = co_await store.begin_transaction();
      auto rollback_error = std::exception_ptr{};
      try {
         co_await tx.rollback();
      } catch (...) {
         rollback_error = std::current_exception();
      }

      BOOST_REQUIRE(rollback_error);
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 1U);
      BOOST_CHECK_EQUAL(driver->destroyed_without_finish(), 0U);

      auto second_started = std::make_shared<bool>(false);
      auto second_cancelled = std::make_shared<bool>(false);
      auto second_finished = std::make_shared<bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();

      boost::asio::co_spawn(
          executor,
          [store, second_started, second_cancelled, second_finished,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto second = co_await store.begin_transaction();
                *second_started = true;
                co_await second.commit();
             } catch (const forge::asio::exceptions::canceled&) {
                *second_cancelled = true;
             } catch (...) {
                *second_error = std::current_exception();
             }
             *second_finished = true;
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);
      cancellation->emit(boost::asio::cancellation_type::all);

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(*second_finished);
      BOOST_CHECK(*second_started);
      BOOST_CHECK(!*second_cancelled);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_direct_mutation_commit_failure_rolls_back_and_releases_writer) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<session_driver<throwing_commit_session>>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      BOOST_CHECK_THROW(co_await store.insert(make_account(91, "failed", 1, 1)), std::runtime_error);
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 1U);
      BOOST_CHECK_EQUAL(driver->destroyed_without_finish(), 0U);
      BOOST_CHECK(!driver->overlapping_writes());
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);

      auto next = co_await store.begin_transaction();
      BOOST_CHECK_EQUAL(driver->active_writes(), 1U);
      co_await next.rollback();
      BOOST_CHECK_EQUAL(driver->rollback_calls(), 2U);
      BOOST_CHECK_EQUAL(driver->active_writes(), 0U);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_begin_read_requires_snapshot_capability) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<session_driver<memory_session>>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      BOOST_CHECK_THROW((void)(co_await store.begin_read()), forge::db::object::exceptions::unsupported_operation);

      auto invalid = std::make_shared<session_driver<invalid_session>>();
      BOOST_CHECK_THROW((void)(co_await forge::db::object::store::open(invalid)),
                        forge::db::object::exceptions::unsupported_operation);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_memory_snapshot_preserves_old_state_across_writes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));

      auto snapshot = co_await store.begin_read();
      co_await store.modify(account::id_t{42}, [](account& value) {
         value.balance = 200;
         value.name = "alice-new";
      });

      const auto old_value = co_await snapshot.get(account::id_t{42});
      BOOST_CHECK_EQUAL(old_value.name, "alice");
      BOOST_CHECK_EQUAL(old_value.balance, 100U);

      const auto new_value = co_await store.get(account::id_t{42});
      BOOST_CHECK_EQUAL(new_value.name, "alice-new");
      BOOST_CHECK_EQUAL(new_value.balance, 200U);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_joins_external_snapshot_without_opening_another_view) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      co_await store.insert(make_account(42, "alice", 100, 3));

      auto core_view = co_await driver->begin_read();
      BOOST_CHECK_EQUAL(driver->snapshot_calls(), 1U);
      auto view = store.join(core_view);
      BOOST_CHECK_EQUAL(driver->snapshot_calls(), 1U);

      co_await store.modify(account::id_t{42}, [](account& value) {
         value.name = "bob";
         value.balance = 200;
      });

      const auto old_value = co_await view.get(account::id_t{42});
      BOOST_CHECK_EQUAL(old_value.name, "alice");
      BOOST_CHECK_EQUAL(old_value.balance, 100U);
      const auto old_index = co_await view.index<account_object, by_name>().find(std::string{"alice"});
      BOOST_REQUIRE(old_index.has_value());
      BOOST_CHECK_EQUAL(old_index->id.instance, 42U);
      BOOST_CHECK(!(co_await view.index<account_object, by_name>().find(std::string{"bob"})).has_value());
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_rejects_foreign_closed_and_originless_snapshots) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   auto foreign_driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto foreign = co_await foreign_driver->begin_read();
      BOOST_CHECK_THROW(static_cast<void>(store.join(foreign)), forge::db::object::exceptions::invalid_descriptor);

      auto closed = forge::db::core::snapshot{};
      BOOST_CHECK_THROW(static_cast<void>(store.join(closed)), forge::db::object::exceptions::transaction_closed);

      auto originless =
          forge::db::core::snapshot{std::make_unique<memory_snapshot_session>(std::make_shared<memory_state>())};
      BOOST_CHECK_THROW(static_cast<void>(store.join(originless)), forge::db::object::exceptions::invalid_descriptor);
      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_store_stream_uses_one_snapshot_across_pages) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));
      co_await store.insert(make_account(43, "bob", 50, 3));

      auto stream = store.index<account_object, by_region_balance>()
                        .equal_range(std::make_tuple(std::uint32_t{3}))
                        .stream({.page_size = 1});

      const auto first = co_await stream.next();
      BOOST_REQUIRE(first.has_value());
      BOOST_CHECK_EQUAL(first->name, "bob");

      co_await store.insert(make_account(44, "carol", 75, 3));

      const auto second = co_await stream.next();
      BOOST_REQUIRE(second.has_value());
      BOOST_CHECK_EQUAL(second->name, "alice");
      BOOST_CHECK(!(co_await stream.next()).has_value());

      const auto fresh = co_await store.index<account_object, by_region_balance>()
                             .equal_range(std::make_tuple(std::uint32_t{3}))
                             .page({.limit = 10});
      BOOST_REQUIRE_EQUAL(fresh.items.size(), 3U);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_serializes_concurrent_transactions) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = co_await store.begin_transaction();

      auto second_started = std::make_shared<bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      boost::asio::co_spawn(
          executor,
          [store, second_started, second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                auto second = co_await store.begin_transaction();
                *second_started = true;
                co_await second.rollback();
             } catch (...) {
                *second_error = std::current_exception();
             }
          },
          boost::asio::detached);

      auto timer = boost::asio::steady_timer{executor};
      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      BOOST_CHECK(!*second_started);
      BOOST_CHECK(!driver->overlapping_writes());

      co_await first.rollback();

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(*second_started);
      BOOST_CHECK(!driver->overlapping_writes());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_cancelled_wait_does_not_acquire_gate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = std::make_shared<std::optional<forge::db::object::transaction>>(co_await store.begin_transaction());

      auto second_waiting = std::make_shared<bool>(false);
      auto second_started = std::make_shared<bool>(false);
      auto second_cancelled = std::make_shared<bool>(false);
      auto second_finished = std::make_shared<bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();

      boost::asio::co_spawn(
          executor,
          [store, second_waiting, second_started, second_cancelled, second_finished,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
                *second_waiting = true;
                auto second = co_await store.begin_transaction();
                *second_started = true;
                co_await second.rollback();
             } catch (const forge::asio::exceptions::canceled&) {
                *second_cancelled = true;
             } catch (...) {
                *second_error = std::current_exception();
             }
             *second_finished = true;
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      while (!*second_waiting) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      cancellation->emit(boost::asio::cancellation_type::all);
      first->reset();

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(*second_finished);
      BOOST_CHECK(*second_cancelled);
      BOOST_CHECK(!*second_started);
      BOOST_CHECK(!first->has_value());
      BOOST_CHECK(!driver->overlapping_writes());

      auto third = co_await store.begin_transaction();
      co_await third.rollback();
      BOOST_CHECK(!driver->overlapping_writes());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_cancelled_armed_wait_does_not_acquire_gate) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = co_await store.begin_transaction();

      auto second_waiting = std::make_shared<std::atomic_bool>(false);
      auto second_started = std::make_shared<std::atomic_bool>(false);
      auto second_cancelled = std::make_shared<std::atomic_bool>(false);
      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();

      boost::asio::co_spawn(
          executor,
          [store, second_waiting, second_started, second_cancelled, second_finished,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
                second_waiting->store(true, std::memory_order_release);
                auto second = co_await store.begin_transaction();
                second_started->store(true, std::memory_order_release);
                co_await second.rollback();
             } catch (const forge::asio::exceptions::canceled&) {
                second_cancelled->store(true, std::memory_order_release);
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      while (!second_waiting->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      cancellation->emit(boost::asio::cancellation_type::all);
      co_await first.rollback();

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(second_finished->load(std::memory_order_acquire));
      BOOST_CHECK(second_cancelled->load(std::memory_order_acquire));
      BOOST_CHECK(!second_started->load(std::memory_order_acquire));
      BOOST_CHECK(!driver->overlapping_writes());

      auto third = co_await store.begin_transaction();
      co_await third.rollback();
      BOOST_CHECK(!driver->overlapping_writes());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_early_cancelled_wait_does_not_need_rescue) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = co_await store.begin_transaction();

      auto second_ready = std::make_shared<std::atomic_bool>(false);
      auto second_started = std::make_shared<std::atomic_bool>(false);
      auto second_cancelled = std::make_shared<std::atomic_bool>(false);
      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();

      boost::asio::co_spawn(
          executor,
          [store, second_ready, second_started, second_cancelled, second_finished,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
                co_await boost::asio::this_coro::throw_if_cancelled(false);
                second_ready->store(true, std::memory_order_release);

                auto second = co_await store.begin_transaction();
                second_started->store(true, std::memory_order_release);
                co_await second.rollback();
             } catch (const forge::asio::exceptions::canceled&) {
                second_cancelled->store(true, std::memory_order_release);
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      while (!second_ready->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      for (auto attempt = 0; attempt != 16 && !second_finished->load(std::memory_order_acquire); ++attempt) {
         cancellation->emit(boost::asio::cancellation_type::all);
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      const auto needed_rescue_cancellation = !second_finished->load(std::memory_order_acquire);
      if (needed_rescue_cancellation) {
         cancellation->emit(boost::asio::cancellation_type::all);
         timer.expires_after(std::chrono::milliseconds{50});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(!needed_rescue_cancellation);
      BOOST_CHECK(second_finished->load(std::memory_order_acquire));
      BOOST_CHECK(second_cancelled->load(std::memory_order_acquire));
      BOOST_CHECK(!second_started->load(std::memory_order_acquire));
      BOOST_CHECK(!driver->overlapping_writes());

      co_await first.rollback();

      auto third = co_await store.begin_transaction();
      co_await third.rollback();
      BOOST_CHECK(!driver->overlapping_writes());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_single_writer_precancelled_wait_does_not_hang) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto first = co_await store.begin_transaction();

      auto second_ready = std::make_shared<std::atomic_bool>(false);
      auto second_started = std::make_shared<std::atomic_bool>(false);
      auto second_cancelled = std::make_shared<std::atomic_bool>(false);
      auto second_finished = std::make_shared<std::atomic_bool>(false);
      auto second_error = std::make_shared<std::exception_ptr>();
      const auto executor = co_await boost::asio::this_coro::executor;
      auto cancellation = std::make_shared<boost::asio::cancellation_signal>();

      boost::asio::co_spawn(
          executor,
          [store, second_ready, second_started, second_cancelled, second_finished,
           second_error]() mutable -> boost::asio::awaitable<void> {
             try {
                co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::enable_total_cancellation{});
                co_await boost::asio::this_coro::throw_if_cancelled(false);
                second_ready->store(true, std::memory_order_release);

                const auto executor = co_await boost::asio::this_coro::executor;
                auto pre_cancel_wait =
                    boost::asio::steady_timer{executor, boost::asio::steady_timer::time_point::max()};
                auto wait_error = boost::system::error_code{};
                co_await pre_cancel_wait.async_wait(
                    boost::asio::redirect_error(boost::asio::use_awaitable, wait_error));
                if (wait_error != boost::asio::error::operation_aborted) {
                   throw std::runtime_error{"test pre-cancel wait was not cancelled"};
                }

                const auto state = co_await boost::asio::this_coro::cancellation_state;
                if (state.cancelled() == boost::asio::cancellation_type::none) {
                   throw std::runtime_error{"test did not enter store with pre-cancelled coroutine state"};
                }
                auto second = co_await store.begin_transaction();
                second_started->store(true, std::memory_order_release);
                co_await second.rollback();
             } catch (const forge::asio::exceptions::canceled&) {
                second_cancelled->store(true, std::memory_order_release);
             } catch (...) {
                *second_error = std::current_exception();
             }
             second_finished->store(true, std::memory_order_release);
             co_return;
          },
          boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::detached));

      auto timer = boost::asio::steady_timer{executor};
      while (!second_ready->load(std::memory_order_acquire)) {
         timer.expires_after(std::chrono::milliseconds{1});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      cancellation->emit(boost::asio::cancellation_type::all);
      timer.expires_after(std::chrono::milliseconds{50});
      co_await timer.async_wait(boost::asio::use_awaitable);

      const auto needed_rescue_cancellation = !second_finished->load(std::memory_order_acquire);
      if (needed_rescue_cancellation) {
         cancellation->emit(boost::asio::cancellation_type::all);
         timer.expires_after(std::chrono::milliseconds{50});
         co_await timer.async_wait(boost::asio::use_awaitable);
      }

      if (*second_error) {
         std::rethrow_exception(*second_error);
      }
      BOOST_CHECK(!needed_rescue_cancellation);
      BOOST_CHECK(second_finished->load(std::memory_order_acquire));
      BOOST_CHECK(second_cancelled->load(std::memory_order_acquire));
      BOOST_CHECK(!second_started->load(std::memory_order_acquire));
      BOOST_CHECK(!driver->overlapping_writes());

      co_await first.rollback();

      auto third = co_await store.begin_transaction();
      co_await third.rollback();
      BOOST_CHECK(!driver->overlapping_writes());

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_session_runtime_object_id_requires_explicit_object_model) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));
      const auto generic = forge::db::ids::object_id{.space = 1, .type = 7, .instance = 42};
      BOOST_CHECK_EQUAL((co_await store.get<account_object>(generic)).name, "alice");

      const auto wrong = forge::db::ids::object_id{.space = 1, .type = 8, .instance = 42};
      BOOST_CHECK_THROW((void)(co_await store.get<account_object>(wrong)),
                        forge::db::object::exceptions::invalid_descriptor);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_modify_updates_secondary_indexes_and_unique_constraints) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));
      co_await store.insert(make_account(43, "bob", 50, 3));

      co_await store.modify(account::id_t{42}, [](account& value) {
         value.name = "alice-2";
         value.balance = 200;
         value.region = 5;
      });

      BOOST_CHECK(!(co_await store.index<account_object, by_name>().find("alice")).has_value());
      BOOST_REQUIRE((co_await store.index<account_object, by_name>().find("alice-2")).has_value());

      BOOST_CHECK_THROW(co_await store.modify(account::id_t{43}, [](account& value) { value.name = "alice-2"; }),
                        forge::db::object::exceptions::duplicate_object);
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{43})).name, "bob");

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_index_supports_mapper_keys_tuple_prefix_stream_and_for_each) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);

      co_await store.insert(make_account(42, "alice", 100, 3));
      co_await store.insert(make_account(43, "bob", 50, 3));
      co_await store.insert(make_account(44, "carol", 75, 4));

      const auto exact = co_await store.index<account_object, by_region_balance>()
                             .equal_range(std::make_tuple(std::uint32_t{3}, std::uint64_t{100}))
                             .page({.limit = 100});
      BOOST_REQUIRE_EQUAL(exact.items.size(), 1U);
      BOOST_CHECK_EQUAL(exact.items[0].name, "alice");

      const auto mapped = co_await store.index<account_object, by_region>().equal_range(std::make_tuple(3U)).page();
      BOOST_REQUIRE_EQUAL(mapped.items.size(), 2U);

      auto stream = store.index<account_object, by_region_balance>()
                        .equal_range(std::make_tuple(std::uint32_t{3}))
                        .stream({.page_size = 1});
      BOOST_REQUIRE((co_await stream.next()).has_value());
      BOOST_REQUIRE((co_await stream.next()).has_value());
      BOOST_CHECK(!(co_await stream.next()).has_value());

      auto visited = std::vector<std::string>{};
      co_await store.index<account_object, by_region_balance>()
          .equal_range(std::make_tuple(std::uint32_t{3}))
          .for_each({.page_size = 1}, [&visited](const account& value) -> boost::asio::awaitable<void> {
             visited.push_back(value.name);
             co_return;
          });
      BOOST_REQUIRE_EQUAL(visited.size(), 2U);
      BOOST_CHECK_EQUAL(visited[0], "bob");
      BOOST_CHECK_EQUAL(visited[1], "alice");

      co_return;
   }());

   BOOST_CHECK_GE(driver->scan_calls(), 2U);
}

BOOST_AUTO_TEST_CASE(db_object_direct_mutation_rolls_back_when_interceptor_vetoes) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto veto = std::make_shared<veto_interceptor>();
      store.add_interceptor(veto);

      BOOST_CHECK_THROW(co_await store.insert(make_account(42, "alice", 100, 3)),
                        forge::db::object::exceptions::duplicate_object);
      BOOST_CHECK_EQUAL(veto->calls, 1U);
      BOOST_CHECK(!(co_await store.find(account::id_t{42})).has_value());
      BOOST_CHECK_EQUAL(driver->record_count(), 1U);

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_observer_runs_after_commit_only) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<counting_observer>();
      store.add_observer(observer);

      {
         auto tx = co_await store.begin_transaction();
         co_await tx.insert(make_account(42, "alice", 100, 3));
         co_await tx.rollback();
      }
      BOOST_CHECK_EQUAL(observer->calls, 0U);

      co_await store.insert(make_account(43, "bob", 50, 3));
      BOOST_CHECK_EQUAL(observer->calls, 1U);
      BOOST_CHECK_EQUAL(observer->mutation_count, 1U);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK_EQUAL(static_cast<int>(observer->last->mutations.front().kind),
                        static_cast<int>(forge::db::object::mutation_kind::insert));

      co_return;
   }());
}

BOOST_AUTO_TEST_CASE(db_object_create_observer_runs_after_commit_only) {
   auto runtime = forge::asio::runtime{};
   auto driver = std::make_shared<memory_driver>();
   forge::asio::blocking::run(runtime, [&driver]() -> boost::asio::awaitable<void> {
      auto store = co_await make_store(driver);
      auto observer = std::make_shared<counting_observer>();
      store.add_observer(observer);

      {
         auto tx = co_await store.begin_transaction();
         auto draft = co_await tx.create<account>([](account& value) {
            value.name = "draft";
            value.balance = 10;
            value.region = 1;
         });
         BOOST_CHECK_EQUAL(draft.id.instance, 0U);
         co_await tx.rollback();
      }
      BOOST_CHECK_EQUAL(observer->calls, 0U);

      auto committed = co_await store.create<account>([](account& value) {
         value.name = "committed";
         value.balance = 20;
         value.region = 2;
      });

      BOOST_CHECK_EQUAL(committed.id.instance, 1U);
      BOOST_CHECK_EQUAL(observer->calls, 1U);
      BOOST_CHECK_EQUAL(observer->mutation_count, 1U);
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_REQUIRE_EQUAL(observer->last->mutations.size(), 1U);
      BOOST_CHECK_EQUAL(observer->last->mutations.front().id.instance, 1U);
      BOOST_CHECK_EQUAL(static_cast<int>(observer->last->mutations.front().kind),
                        static_cast<int>(forge::db::object::mutation_kind::insert));

      co_return;
   }());
}

#if FORGE_HAS_ROCKSDB
BOOST_AUTO_TEST_CASE(db_object_db_rocksdb_driver_persists_objects_indexes_pages_and_streams) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto root = std::filesystem::temp_directory_path() / ("forge_db_object_driver_" + std::to_string(nonce));
   std::filesystem::remove_all(root);

   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, [root]() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
          .path = (root / "store").string(),
          .families = {"objectdb"},
      });
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<account_object>();
      store.register_object<document_object>();
      store.register_object<ranked_upload_object>();
      BOOST_CHECK_EQUAL(store.header().version, forge::db::object::header::current_version);

      auto tx = co_await store.begin_transaction();
      co_await tx.insert(make_account(42, "alice", 100, 3));
      co_await tx.insert(make_account(43, "bob", 50, 3));
      co_await tx.insert(make_document(1, 7, "rocks@example.test", 100));
      co_await tx.insert(make_ranked_upload(1, 3, 7, 50, 4096));
      co_await tx.insert(make_ranked_upload(2, 3, 7, 40, 2048));
      co_await tx.commit();
      auto ranked = store.index<ranked_upload_object, by_ranked_state>();
      BOOST_CHECK_EQUAL(co_await ranked.equal_range(3U).count(), 2U);
      BOOST_CHECK_EQUAL(co_await ranked.equal_range(3U).sum<by_payload_bytes>(), 6144U);
      BOOST_CHECK_THROW(co_await store.insert(make_document(2, 7, "rocks@example.test", 50)),
                        forge::db::object::exceptions::duplicate_object);
      driver->flush();
      driver->flush(false);
      co_await driver->async_flush(true);

      co_return;
   }());

   forge::asio::blocking::run(runtime, [root]() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
          .path = (root / "store").string(),
          .families = {"objectdb"},
      });
      auto store = co_await forge::db::object::store::open(
          driver, forge::db::object::store::options{.writes = forge::db::object::write_policy::backend});
      store.register_object<account_object>();
      store.register_object<document_object>();
      store.register_object<ranked_upload_object>();
      BOOST_CHECK((co_await store.get(forge::db::object::header_id)) == store.header());

      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "alice");
      const auto document =
          co_await store.index<document_object, by_tenant_email>().find(std::uint32_t{7}, "rocks@example.test");
      BOOST_REQUIRE(document.has_value());
      BOOST_CHECK_EQUAL(document->id.instance, 1U);

      const auto page = co_await store.index<account_object, by_region_balance>()
                            .equal_range(std::make_tuple(std::uint32_t{3}))
                            .page({.limit = 1});
      BOOST_REQUIRE_EQUAL(page.items.size(), 1U);
      BOOST_CHECK_EQUAL(page.items[0].name, "bob");
      BOOST_REQUIRE(page.next.has_value());

      auto stream = store.index<account_object, by_region_balance>()
                        .equal_range(std::make_tuple(std::uint32_t{3}))
                        .stream({.page_size = 1});
      BOOST_REQUIRE((co_await stream.next()).has_value());
      BOOST_REQUIRE((co_await stream.next()).has_value());
      BOOST_CHECK(!(co_await stream.next()).has_value());

      const auto lower = co_await store.index<account_object, by_region_balance>()
                             .lower_bound(std::make_tuple(std::uint32_t{3}, std::uint64_t{100}))
                             .page({.limit = 2});
      BOOST_REQUIRE_EQUAL(lower.items.size(), 1U);
      BOOST_CHECK_EQUAL(lower.items[0].name, "alice");

      auto ranked = store.index<ranked_upload_object, by_ranked_state>();
      BOOST_CHECK_EQUAL(co_await ranked.count(), 2U);
      BOOST_CHECK_EQUAL(co_await ranked.sum<by_payload_bytes>(), 6144U);
      BOOST_CHECK_EQUAL((co_await ranked.nth(0))->id.instance, 1U);

      co_return;
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_object_db_rocksdb_driver_rolls_back_uncommitted_transaction) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto root = std::filesystem::temp_directory_path() / ("forge_db_object_rollback_" + std::to_string(nonce));
   std::filesystem::remove_all(root);

   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, [root]() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
          .path = (root / "store").string(),
          .families = {"objectdb"},
      });
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      {
         auto tx = co_await store.begin_transaction();
         co_await tx.insert(make_account(42, "alice", 100, 3));
      }

      BOOST_CHECK(!(co_await store.find(account::id_t{42})).has_value());
      co_return;
   }());

   std::filesystem::remove_all(root);
}

BOOST_AUTO_TEST_CASE(db_object_db_rocksdb_snapshot_preserves_old_state_across_writes) {
   const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
   const auto root = std::filesystem::temp_directory_path() / ("forge_db_object_snapshot_" + std::to_string(nonce));
   std::filesystem::remove_all(root);

   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, [root]() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<forge::db::rocksdb::driver>(forge::db::rocksdb::config{
          .path = (root / "store").string(),
          .families = {"objectdb"},
      });
      auto store = co_await forge::db::object::store::open(driver);
      store.register_object<account_object>();

      co_await store.insert(make_account(42, "alice", 100, 3));
      auto snapshot = co_await store.begin_read();
      co_await store.modify(account::id_t{42}, [](account& value) {
         value.name = "alice-new";
         value.balance = 200;
      });

      BOOST_CHECK_EQUAL((co_await snapshot.get(account::id_t{42})).name, "alice");
      BOOST_CHECK_EQUAL((co_await store.get(account::id_t{42})).name, "alice-new");

      co_return;
   }());

   std::filesystem::remove_all(root);
}
#endif

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_preserves_erased_highest_and_future_id) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      const auto options =
          forge::db::object::store::options{.id_allocation = forge::db::object::id_allocation_policy::transactional};
      auto source = co_await make_store(std::make_shared<memory_driver>(), options);
      const auto first = co_await source.create<account>([](auto& value) { value.name = "first"; });
      const auto last = co_await source.create<account>([](auto& value) { value.name = "erased"; });
      co_await source.erase(last.id);
      auto read = co_await source.begin_read();
      const auto next = co_await read.allocation_next<account_object>();
      BOOST_CHECK_EQUAL(next, 2U);
      auto driver = std::make_shared<memory_driver>();
      auto target = co_await make_store(driver, options);
      auto observer = std::make_shared<counting_observer>();
      target.add_observer(observer);
      auto tx = co_await target.begin_transaction();
      auto alias = co_await target.join(tx);
      auto values = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = next}};
      co_await target.restore_allocations(alias, std::move(values));
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), next);
      co_await tx.insert(first);
      const auto trace = tx.projected_changes();
      BOOST_REQUIRE_EQUAL(trace.allocations.size(), 1U);
      BOOST_CHECK_EQUAL(trace.allocations.front().before, 0U);
      BOOST_CHECK_EQUAL(trace.allocations.front().after, next);
      co_await tx.commit();
      BOOST_REQUIRE(observer->last.has_value());
      BOOST_CHECK(observer->last->allocations == trace.allocations);
      auto reopened = co_await make_store(driver, options);
      auto reopened_read = co_await reopened.begin_read();
      BOOST_CHECK_EQUAL(co_await reopened_read.allocation_next<account_object>(), next);
      const auto actual = co_await reopened.create<account>([](auto& value) { value.name = "future"; });
      const auto expected = co_await source.create<account>([](auto& value) { value.name = "future"; });
      BOOST_CHECK_EQUAL(actual.id.instance, expected.id.instance);
      BOOST_CHECK_EQUAL(actual.id.instance, 2U);
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_canonical_zero_and_exhaustion) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      auto tx = co_await store.begin_transaction();
      auto zero = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = 0U}};
      const auto mutations = driver->mutation_calls();
      co_await store.restore_allocations(tx, std::move(zero));
      BOOST_CHECK_EQUAL(driver->mutation_calls(), mutations);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 0U);
      BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
      co_await tx.commit();
      BOOST_CHECK(!driver->first_key_with_kind(2U));
      auto exhausted = co_await store.begin_transaction();
      auto maximum = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value,
           .next = std::numeric_limits<std::uint64_t>::max()}};
      co_await store.restore_allocations(exhausted, std::move(maximum));
      co_await exhausted.commit();
      auto next = co_await store.begin_transaction();
      BOOST_CHECK_EQUAL(co_await next.allocation_next<account_object>(), std::numeric_limits<std::uint64_t>::max());
      BOOST_CHECK_THROW(co_await next.create<account>([](auto& value) { value.name = "overflow"; }),
                        forge::db::object::exceptions::invalid_descriptor);
      co_await next.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_success_obeys_savepoints_and_full_rollback) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      const auto type = forge::db::object::object_id_of<account_object>::value;
      auto tx = co_await store.begin_transaction();
      const auto point = co_await tx.db_transaction().create_savepoint();
      auto values = std::vector<forge::db::object::store::allocation>{{.type = type, .next = 9U}};
      co_await store.restore_allocations(tx, std::move(values));
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 9U);
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 0U);
      BOOST_CHECK(tx.projected_changes().empty());
      auto replacement = std::vector<forge::db::object::store::allocation>{{.type = type, .next = 7U}};
      co_await store.restore_allocations(tx, std::move(replacement));
      co_await tx.rollback();
      auto read = co_await store.begin_read();
      BOOST_CHECK_EQUAL(co_await read.allocation_next<account_object>(), 0U);
      BOOST_CHECK(!driver->first_key_with_kind(2U));
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_rejects_unregistered_system_duplicate_and_unsorted_types) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      store.register_object<document_object>();
      const auto account_type = forge::db::object::object_id_of<account_object>::value;
      const auto document_type = forge::db::object::object_id_of<document_object>::value;
      using allocation = forge::db::object::store::allocation;
      const auto invalid = std::vector<std::vector<allocation>>{
          {{.type = {.space = 0U, .type = 0U}, .next = 1U}},
          {{.type = {.space = 1U, .type = 999U}, .next = 1U}},
          {{.type = {.space = 1U, .type = 7U, .instance = 1U}, .next = 1U}},
          {{.type = account_type, .next = 1U}, {.type = account_type, .next = 2U}},
          {{.type = document_type, .next = 1U}, {.type = account_type, .next = 2U}},
          {{.type = account_type}, {.type = document_type}, {.type = document_type}}};
      const auto before = driver->records();
      const auto writes = driver->mutation_calls();
      for (const auto& values : invalid) {
         auto tx = co_await store.begin_transaction();
         BOOST_CHECK_THROW(co_await store.restore_allocations(tx, values),
                           forge::db::object::exceptions::invalid_descriptor);
         co_await tx.rollback();
      }
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), writes);
      auto tx = co_await store.begin_transaction();
      auto other = co_await make_store(std::make_shared<memory_driver>(),
                                       {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      auto values = std::vector<allocation>{{.type = account_type, .next = 1U}};
      BOOST_CHECK_THROW(co_await other.restore_allocations(tx, values),
                        forge::db::object::exceptions::invalid_descriptor);
      co_await tx.rollback();
      auto monotonic = co_await make_store(std::make_shared<memory_driver>());
      auto unsafe = co_await monotonic.begin_transaction();
      BOOST_CHECK_THROW(co_await monotonic.restore_allocations(unsafe, values),
                        forge::db::object::exceptions::unsupported_operation);
      co_await unsafe.rollback();
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_cannot_lower_occupied_namespace_and_failure_is_sticky) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      const auto existing = co_await store.create<account>([](auto& value) { value.name = "existing"; });
      co_await store.erase(existing.id); // A sequence alone still makes this non-pristine.
      const auto before = driver->records();
      const auto writes = driver->mutation_calls();
      auto tx = co_await store.begin_transaction();
      const auto point = co_await tx.db_transaction().create_savepoint();
      auto values = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = 0U}};
      BOOST_CHECK_THROW(co_await store.restore_allocations(tx, values),
                        forge::db::object::exceptions::invalid_descriptor);
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 1U);
      BOOST_CHECK_THROW(co_await store.restore_allocations(tx, values),
                        forge::db::core::exceptions::participant_conflict);
      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::core::exceptions::participant_conflict);
      co_await tx.rollback();
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), writes);
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_partial_put_failure_cannot_commit_or_reset_via_savepoint) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      store.register_object<document_object>();
      const auto before = driver->records();
      const auto values = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = 5U},
          {.type = forge::db::object::object_id_of<document_object>::value, .next = 8U}};
      auto tx = co_await store.begin_transaction();
      const auto point = co_await tx.db_transaction().create_savepoint();
      driver->fail_put_after(1U);
      BOOST_CHECK_THROW(co_await store.restore_allocations(tx, values), std::runtime_error);
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 5U);
      BOOST_REQUIRE_EQUAL(tx.projected_changes().allocations.size(), 1U);
      co_await tx.db_transaction().rollback_to_savepoint(point);
      BOOST_CHECK(tx.projected_changes().empty());
      BOOST_CHECK_EQUAL(co_await tx.allocation_next<account_object>(), 0U);
      BOOST_CHECK_THROW(co_await tx.commit(), forge::db::core::exceptions::participant_conflict);
      co_await tx.rollback();
      BOOST_CHECK(driver->records() == before);
      auto fresh = co_await store.begin_transaction();
      co_await store.restore_allocations(fresh, values);
      co_await fresh.commit();
      auto read = co_await store.begin_read();
      BOOST_CHECK_EQUAL(co_await read.allocation_next<account_object>(), 5U);
      BOOST_CHECK_EQUAL(co_await read.allocation_next<document_object>(), 8U);
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_lazy_admission_and_revision_rejection) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store =
          co_await make_store(driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      auto tx = co_await store.begin_transaction();
      auto values = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = 4U}};
      auto operation = store.restore_allocations(tx, values);
      const auto writes = driver->mutation_calls();
      BOOST_CHECK_THROW(static_cast<void>(store.restore_allocations(tx, values)), forge::db::core::exceptions::participant_conflict);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), writes);
      co_await std::move(operation);
      co_await tx.rollback();
      auto captured = co_await store.begin_transaction();
      auto lazy = store.restore_allocations(captured, values);
      captured.db_transaction().attach_participant(
          std::make_shared<pausing_allocation_participant>(co_await boost::asio::this_coro::executor));
      BOOST_CHECK_THROW(co_await std::move(lazy), forge::db::object::exceptions::unsupported_operation);
      BOOST_CHECK_THROW(co_await captured.commit(), forge::db::core::exceptions::participant_conflict);
      co_await captured.rollback();
      auto abandoned = co_await store.begin_transaction();
      {
         auto pending = store.restore_allocations(abandoned, values);
         BOOST_CHECK_THROW(co_await abandoned.commit(), forge::db::core::exceptions::participant_conflict);
      }
      co_await abandoned.rollback();
      BOOST_CHECK(!driver->first_key_with_kind(2U));
   }());
}

BOOST_AUTO_TEST_CASE(db_object_restore_allocations_rejects_prepared_core_but_preserves_native_commit_retry) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, []() -> boost::asio::awaitable<void> {
      auto driver = std::make_shared<memory_driver>();
      auto store = co_await make_store(
          driver, {.id_allocation = forge::db::object::id_allocation_policy::transactional});
      const auto before = driver->records();
      auto tx = co_await store.begin_transaction();
      BOOST_CHECK(tx.projected_changes().empty()); // No observer and no allocation.
      driver->fail_next_commits(1U);
      BOOST_CHECK_THROW(co_await tx.commit(), std::runtime_error);
      const auto scans = driver->scan_calls();
      const auto writes = driver->mutation_calls();
      auto values = std::vector<forge::db::object::store::allocation>{
          {.type = forge::db::object::object_id_of<account_object>::value, .next = 9U}};
      // This is a synchronous pre-admission rejection, not a loading failure.
      BOOST_CHECK_THROW(static_cast<void>(store.restore_allocations(tx, values)), forge::db::core::exceptions::participant_conflict);
      BOOST_CHECK_EQUAL(driver->scan_calls(), scans);
      BOOST_CHECK_EQUAL(driver->mutation_calls(), writes);
      BOOST_CHECK(tx.projected_changes().empty());
      co_await tx.commit(); // Existing Core prepared-commit retry remains valid.
      BOOST_CHECK(driver->records() == before);
      BOOST_CHECK(!driver->first_key_with_kind(2U));
      auto read = co_await store.begin_read();
      BOOST_CHECK_EQUAL(co_await read.allocation_next<account_object>(), 0U);
   }());
}

BOOST_AUTO_TEST_CASE(db_object_cursor_is_opaque_key_boundary) {
   const auto key = forge::db::core::record_key{
       std::vector<std::byte>{std::byte{0x10}, std::byte{0x01}, std::byte{0x00}, std::byte{0x07}}};
   const auto cursor = forge::db::core::cursor{.boundary = key};

   BOOST_CHECK(cursor.boundary == key);
}

BOOST_AUTO_TEST_CASE(db_object_page_request_rejects_invalid_limits_with_typed_exception) {
   BOOST_CHECK_THROW(forge::db::object::validate_page_request(forge::db::core::page_request{.limit = 0}),
                     forge::db::object::exceptions::invalid_cursor);
   BOOST_CHECK_THROW(forge::db::object::validate_page_request(
                         forge::db::core::page_request{.limit = forge::db::core::max_page_limit + 1}),
                     forge::db::object::exceptions::invalid_cursor);
   BOOST_CHECK_NO_THROW(forge::db::object::validate_page_request(forge::db::core::page_request{.limit = 100}));
}

BOOST_AUTO_TEST_SUITE_END()
