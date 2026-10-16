module;

#include <boost/asio/awaitable.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>

export module forge.db.authenticated.store;

import forge.db.authenticated.types;
import forge.db.authenticated.transaction;
import forge.db.core.driver;
import forge.db.core.record;

export namespace forge::db::authenticated {

class store {
 public:
   struct config {
      forge::db::core::family family{"authenticated"};
      std::string domain;
      limits bounds;
      std::function<void(const forge::db::core::record_key&)> read_observer;
   };

   store() = default;
   explicit store(std::shared_ptr<forge::db::core::driver> driver, config settings);

   boost::asio::awaitable<std::optional<root>> earliest() const;
   boost::asio::awaitable<std::optional<root>> latest() const;
   boost::asio::awaitable<std::optional<root>> find_root(version_id_t version) const;
   boost::asio::awaitable<std::optional<bytes>> get(version_id_t version, std::span<const std::byte> key) const;
   boost::asio::awaitable<verified_range> scan_range(version_id_t version, range_request request,
                                                     proof_tree tree = proof_tree::state) const;
   boost::asio::awaitable<point_proof> prove(version_id_t version, std::span<const std::byte> key,
                                             bool include_value = true) const;
   boost::asio::awaitable<range_proof> prove_range(version_id_t version, range_request request,
                                                   proof_tree tree = proof_tree::state) const;
   boost::asio::awaitable<prune_result> prune_through(forge::db::core::transaction& active,
                                                      version_id_t inclusive_boundary,
                                                      prune_options options = {}) const;

   boost::asio::awaitable<transaction> join(forge::db::core::transaction& active, version_id_t version) const;

   boost::asio::awaitable<checkpoint_summary> export_checkpoint(forge::db::core::snapshot active, root expected,
                                                                checkpoint_sink sink, checkpoint_limits bounds) const;
   [[nodiscard]] transaction join_restore(forge::db::core::transaction& active, root expected,
                                          checkpoint_limits bounds) const;

 private:
   struct impl;
   explicit store(std::shared_ptr<impl> implementation);

   std::shared_ptr<impl> impl_;
};

} // namespace forge::db::authenticated
