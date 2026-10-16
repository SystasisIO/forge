module;

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>

export module forge.db.authenticated.transaction;

import forge.db.authenticated.types;
import forge.db.core.driver;
import forge.db.core.record;

export namespace forge::db::authenticated {

namespace detail {
class transaction_access;
}

class transaction {
 public:
   transaction() = default;
   ~transaction();

   transaction(const transaction&) = delete;
   transaction& operator=(const transaction&) = delete;
   transaction(transaction&&) noexcept;
   transaction& operator=(transaction&&) noexcept;

   [[nodiscard]] version_id_t version() const;
   [[nodiscard]] std::optional<root> base() const;
   [[nodiscard]] std::optional<staged_version> staged() const;
   [[nodiscard]] std::optional<root> restored() const;

   boost::asio::awaitable<staged_version> preview(std::span<const mutation> mutations);
   boost::asio::awaitable<staged_version> stage(std::span<const mutation> mutations,
                                                std::optional<digest> expected_state_root = std::nullopt);
   boost::asio::awaitable<checkpoint_summary> restore_checkpoint(checkpoint_source source, checkpoint_verify verify);

 private:
   struct impl;
   explicit transaction(std::shared_ptr<impl> implementation);

   std::shared_ptr<impl> impl_;

   friend class detail::transaction_access;
};

namespace detail {

class transaction_access {
 public:
   [[nodiscard]] static transaction make(forge::db::core::transaction& active, forge::db::core::family family,
                                         std::string domain, digest namespace_hash, limits bounds,
                                         version_id_t candidate, std::optional<root> base);
   [[nodiscard]] static transaction make_restore(forge::db::core::transaction& active,
                                                 std::shared_ptr<forge::db::core::driver> owner,
                                                 forge::db::core::family family, std::string domain,
                                                 digest namespace_hash, limits bounds, root expected,
                                                 checkpoint_limits checkpoint_bounds);
};

} // namespace detail

} // namespace forge::db::authenticated
