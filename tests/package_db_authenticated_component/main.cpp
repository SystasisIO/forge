#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/describe.hpp>
#include <boost/mp11.hpp>

#include <concepts>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

import forge.db.authenticated.codec;
import forge.db.authenticated.hash;
import forge.db.authenticated.proof;
import forge.db.authenticated.store;
import forge.db.authenticated.transaction;
import forge.db.authenticated.types;
import forge.db.core.driver;
import forge.db.core.record;
import forge.raw.codec;

namespace {
namespace auth = forge::db::authenticated;
namespace core = forge::db::core;
using records = std::map<core::record_key, std::vector<std::byte>>;

// Private package-consumer backend: no extra Forge component, scheduler or API.
class package_session final : public core::session {
 public:
   package_session(std::shared_ptr<records> stored, bool writable)
       : stored_{std::move(stored)}, active_{*stored_}, writable_{writable} {}
   core::capabilities capabilities() const noexcept override {
      return {.snapshot_reads = true, .writes = writable_, .record_locks = writable_};
   }
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(core::family, core::record_key key) override {
      const auto found = active_.find(key);
      co_return found == active_.end() ? std::nullopt : std::optional{found->second};
   }
   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get_for_update(core::family family,
                                                                                core::record_key key) override {
      co_return co_await get(std::move(family), std::move(key));
   }
   boost::asio::awaitable<void> put(core::family, core::record_key key, std::vector<std::byte> value) override {
      active_.insert_or_assign(std::move(key), std::move(value));
      co_return;
   }
   boost::asio::awaitable<void> erase(core::family, core::record_key key) override {
      active_.erase(key);
      co_return;
   }
   boost::asio::awaitable<core::record_page> scan_page(core::family, core::record_range,
                                                       core::page_request request) override {
      auto page = core::record_page{};
      for (const auto& [key, value] : active_) {
         if (page.entries.size() == request.limit)
            break;
         page.entries.push_back({.key = key, .value = value});
      }
      co_return page;
   }
   boost::asio::awaitable<void> commit() override {
      if (writable_)
         *stored_ = std::move(active_);
      co_return;
   }
   boost::asio::awaitable<void> rollback() override {
      co_return;
   }

 private:
   std::shared_ptr<records> stored_;
   records active_;
   bool writable_;
};

class package_driver final : public core::driver {
 public:
   boost::asio::awaitable<void> async_flush(bool) override {
      co_return;
   }

 private:
   boost::asio::awaitable<std::unique_ptr<core::session>> open_transaction() override {
      co_return std::make_unique<package_session>(stored_, true);
   }
   boost::asio::awaitable<std::unique_ptr<core::session>> open_snapshot() override {
      co_return std::make_unique<package_session>(stored_, false);
   }
   std::shared_ptr<records> stored_ = std::make_shared<records>();
};

boost::asio::awaitable<bool> exercise_checkpoint(std::shared_ptr<package_driver> driver) {
   const auto settings = auth::store::config{.domain = "forge.package.checkpoint.v1",
                                             .bounds = {.max_key_bytes = 64U, .max_value_bytes = 128U}};
   auto store = auth::store{driver, settings};
   const auto policy = auth::checkpoint_limits{.max_records = 32U,
                                               .max_encoded_bytes = 4096U,
                                               .max_value_bytes = 4096U,
                                               .max_frame_bytes = 1024U,
                                               .max_working_bytes = 1U << 20U,
                                               .max_depth = 16U};
   const auto record = auth::checkpoint_record{.node = auth::proof_leaf{.value = auth::bytes{std::byte{7}}}};
   const auto frame = auth::encode(record);
   if (auth::decode_checkpoint_record(frame, settings.bounds, policy) != record ||
       frame.size() != auth::wire_size(record))
      co_return false;
   const auto empty = auth::root{
       .version = 17U,
       .state_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::state)),
       .change_root = auth::hash_empty(auth::canonical_tree_domain(settings.domain, auth::proof_tree::changes))};
   auto verified = std::make_shared<bool>(false);
   auto active = co_await driver->begin_transaction();
   auto joined = store.join_restore(active, empty, policy);
   auth::checkpoint_source source = []() -> boost::asio::awaitable<std::optional<auth::checkpoint_record>> {
      co_return std::nullopt;
   };
   auth::checkpoint_verify verify = [verified](auth::checkpoint_summary value) -> boost::asio::awaitable<void> {
      *verified = value.records == 0U;
      co_return;
   };
   const auto restored = co_await joined.restore_checkpoint(std::move(source), std::move(verify));
   if (!*verified || restored.anchor != empty || joined.restored() != std::optional{empty})
      co_return false;
   co_await active.commit();
   auth::checkpoint_sink sink = [](auth::checkpoint_record) -> boost::asio::awaitable<void> { co_return; };
   const auto exported =
       co_await store.export_checkpoint(co_await driver->begin_read(), empty, std::move(sink), policy);
   co_await driver->async_close();
   co_return exported == restored;
}
} // namespace

int main() {
   static_assert(boost::describe::has_describe_members<forge::raw::unpack_limits>::value);
   static_assert(boost::mp11::mp_size<boost::describe::describe_members<
                     forge::raw::unpack_limits, boost::describe::mod_public>>::value == 4U);
   static_assert(boost::describe::has_describe_members<forge::db::authenticated::checkpoint_limits>::value);
   static_assert(boost::mp11::mp_size<boost::describe::describe_members<
                     forge::db::authenticated::checkpoint_limits, boost::describe::mod_public>>::value == 6U);
   static_assert(std::same_as<forge::db::authenticated::version_id_t, std::uint64_t>);
   static_assert(requires(const forge::db::authenticated::store& value) {
      { value.earliest() } -> std::same_as<boost::asio::awaitable<std::optional<forge::db::authenticated::root>>>;
   });

   const auto options = forge::db::authenticated::prune_options{
       .max_versions = 32,
       .max_garbage_records = 1'024,
   };
   const auto request = forge::db::authenticated::range_request{
       .limit = 128,
       .reverse = true,
   };
   auto io = boost::asio::io_context{};
   auto success = false;
   boost::asio::co_spawn(io, exercise_checkpoint(std::make_shared<package_driver>()),
                         [&success](std::exception_ptr error, bool result) { success = !error && result; });
   io.run();
   return options.max_versions == 32 && request.limit == 128 && request.reverse && success ? 0 : 1;
}
