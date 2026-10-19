#pragma once

#include <algorithm>
#include <map>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

import forge.db.core.driver;
import forge.db.core.record;

namespace swarm_test {

namespace dbc = forge::db::core;

struct memory_state {
   using records = std::map<std::string, std::map<dbc::record_key, std::vector<std::byte>>>;
   std::mutex mutex;
   records live;
   records durable;
   bool fail_commit = false;
   bool fail_flush = false;
   bool fail_rollback = false;
   std::function<void()> before_commit;
   std::function<void()> before_flush;
   std::optional<std::size_t> fail_put_after;
   std::size_t gets = 0, scans = 0, scanned = 0, flushes = 0;
};

// Test-only transactional driver: no custom Swarm storage/index implementation.
class memory_session final : public dbc::session {
 public:
   memory_session(std::shared_ptr<memory_state> state, bool writable)
      : state_{std::move(state)}, writable_{writable} {
      const auto lock = std::scoped_lock{state_->mutex};
      records_ = state_->live;
   }
   dbc::capabilities capabilities() const noexcept override {
      return {.snapshot_reads = true, .writes = writable_};
   }
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(dbc::family family, dbc::record_key key) override {
      { const auto lock = std::scoped_lock{state_->mutex}; ++state_->gets; }
      const auto& values = records_[family.name];
      const auto found = values.find(key);
      if (found == values.end()) { co_return std::nullopt; }
      co_return found->second;
   }
   boost::asio::awaitable<void> put(dbc::family family, dbc::record_key key, std::vector<std::byte> value) override {
      if (!writable_) { throw std::runtime_error{"read-only memory snapshot"}; }
      {
         const auto lock = std::scoped_lock{state_->mutex};
         if (state_->fail_put_after) {
            if (*state_->fail_put_after == 0) {
               state_->fail_put_after.reset();
               throw std::runtime_error{"injected write failure"};
            }
            --*state_->fail_put_after;
         }
      }
      records_[family.name][key] = value;
      erased_.erase({family.name, key});
      writes_[family.name][std::move(key)] = std::move(value);
      co_return;
   }
   boost::asio::awaitable<void> erase(dbc::family family, dbc::record_key key) override {
      if (!writable_) { throw std::runtime_error{"read-only memory snapshot"}; }
      records_[family.name].erase(key);
      writes_[family.name].erase(key);
      erased_.insert({family.name, std::move(key)});
      co_return;
   }
   boost::asio::awaitable<dbc::record_page> scan_page(dbc::family family, dbc::record_range range, dbc::page_request request) override {
      dbc::validate_page_request(request);
      auto result = dbc::record_page{};
      const auto& values = records_[family.name];
      auto next = values.lower_bound(range.begin);
      if (request.after && request.after->boundary >= range.begin) { next = values.upper_bound(request.after->boundary); }
      for (; next != values.end(); ++next) {
         if (range.has_end && next->first >= range.end) { break; }
         const auto& bytes = next->first.bytes();
         if (!range.prefix.empty() && (bytes.size() < range.prefix.bytes().size() ||
             !std::equal(range.prefix.bytes().begin(), range.prefix.bytes().end(), bytes.begin()))) { break; }
         if (result.entries.size() == request.limit) {
            result.next = dbc::cursor{result.entries.back().key};
            break;
         }
         result.entries.push_back({next->first, next->second});
      }
      {
         const auto lock = std::scoped_lock{state_->mutex};
         ++state_->scans;
         state_->scanned += result.entries.size();
      }
      co_return result;
   }
   boost::asio::awaitable<void> commit() override {
      if (!writable_) { throw std::runtime_error{"read-only memory snapshot"}; }
      auto hook = std::function<void()>{};
      { const auto lock = std::scoped_lock{state_->mutex}; hook = state_->before_commit; }
      if (hook) { hook(); }
      const auto lock = std::scoped_lock{state_->mutex};
      if (state_->fail_commit) { throw std::runtime_error{"injected commit failure"}; }
      // Merge this transaction's write set, preserving allocator commits made
      // through a separate DB Core transaction while Object Store owns its gate.
      for (const auto& [family, key] : erased_) { state_->live[family].erase(key); }
      for (const auto& [family, values] : writes_) {
         for (const auto& [key, value] : values) { state_->live[family][key] = value; }
      }
      co_return;
   }
   boost::asio::awaitable<void> rollback() override {
      const auto lock = std::scoped_lock{state_->mutex};
      if (state_->fail_rollback) { throw std::runtime_error{"injected rollback failure"}; }
      co_return;
   }

 private:
   std::shared_ptr<memory_state> state_;
   bool writable_;
   memory_state::records records_;
   memory_state::records writes_;
   std::set<std::pair<std::string, dbc::record_key>> erased_;
};

class memory_driver final : public dbc::driver {
 public:
   std::shared_ptr<memory_state> state = std::make_shared<memory_state>();
   boost::asio::awaitable<void> async_flush(bool sync) override {
      auto hook = std::function<void()>{};
      { const auto lock = std::scoped_lock{state->mutex}; hook = state->before_flush; }
      if (hook) { hook(); }
      const auto lock = std::scoped_lock{state->mutex};
      if (!sync) { throw std::runtime_error{"durable flush required"}; }
      ++state->flushes;
      if (state->fail_flush) { throw std::runtime_error{"injected flush failure"}; }
      state->durable = state->live;
      co_return;
   }
 private:
   boost::asio::awaitable<std::unique_ptr<dbc::session>> open_transaction() override {
      co_return std::make_unique<memory_session>(state, true);
   }
   boost::asio::awaitable<std::unique_ptr<dbc::session>> open_snapshot() override {
      co_return std::make_unique<memory_session>(state, false);
   }
};

} // namespace swarm_test
