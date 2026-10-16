#pragma once

#include <memory>
#include <optional>

namespace forge::db::authenticated::detail {
class transaction_participant_impl;
}

namespace forge::db::authenticated {

struct transaction::impl {
   impl(forge::db::core::transaction& active_value, forge::db::core::family family_value, std::string domain_value,
        digest namespace_hash_value, limits bounds_value, version_id_t candidate_value, std::optional<root> base_value);

   forge::db::core::transaction* active = nullptr;
   forge::db::core::family family;
   std::string domain;
   digest namespace_hash;
   limits bounds;
   version_id_t candidate = 0;
   std::optional<root> base_root;
   std::optional<root> restore_anchor;
   checkpoint_limits restore_bounds;
   std::shared_ptr<detail::transaction_participant_impl> participant;
};

} // namespace forge::db::authenticated
