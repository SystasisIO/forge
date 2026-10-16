#include <boost/asio/awaitable.hpp>

#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

import forge.db.core.driver;
import forge.db.core.record;

class package_session final : public forge::db::core::session {
 public:
   [[nodiscard]] forge::db::core::capabilities capabilities() const noexcept override {
      return forge::db::core::capabilities{.snapshot_reads = true, .writes = true};
   }

   boost::asio::awaitable<std::optional<std::vector<std::byte>>> get(forge::db::core::family, forge::db::core::record_key) override {
      co_return std::nullopt;
   }

   boost::asio::awaitable<void> put(forge::db::core::family, forge::db::core::record_key, std::vector<std::byte>) override {
      co_return;
   }

   boost::asio::awaitable<void> erase(forge::db::core::family, forge::db::core::record_key) override {
      co_return;
   }

   boost::asio::awaitable<forge::db::core::record_page> scan_page(forge::db::core::family,
                                                            forge::db::core::record_range,
                                                            forge::db::core::page_request) override {
      co_return forge::db::core::record_page{};
   }

   boost::asio::awaitable<void> commit() override {
      co_return;
   }

   boost::asio::awaitable<void> rollback() override {
      co_return;
   }
};

int main() {
   static_assert(std::derived_from<package_session, forge::db::core::session>);
   static_assert(requires(const forge::db::core::snapshot& view,
                          const forge::db::core::driver& driver) {
      { view.belongs_to(driver) } -> std::same_as<bool>;
   });
   static_assert(requires(const forge::db::core::transaction& active,
                          const forge::db::core::driver& driver) {
      { active.belongs_to(driver) } noexcept -> std::same_as<bool>;
   });
   static_assert(requires(forge::db::core::driver& driver) {
      driver.async_close();
   });
   auto family = forge::db::core::family{"package"};
   return family.name == "package" ? 0 : 1;
}
