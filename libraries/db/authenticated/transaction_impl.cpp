module;

#include <boost/asio/awaitable.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.db.authenticated.transaction;

#include "details/transaction_impl.hxx"
#include "details/transaction_participant_impl.hxx"

namespace forge::db::authenticated {

transaction::impl::impl(forge::db::core::transaction& active_value, forge::db::core::family family_value,
                        std::string domain_value, digest namespace_hash_value, limits bounds_value,
                        version_id_t candidate_value, std::optional<root> base_value)
    : active{std::addressof(active_value)}, family{std::move(family_value)}, domain{std::move(domain_value)},
      namespace_hash{namespace_hash_value}, bounds{bounds_value}, candidate{candidate_value},
      base_root{std::move(base_value)},
      participant{std::make_shared<detail::transaction_participant_impl>(family, domain, namespace_hash)} {}

} // namespace forge::db::authenticated
