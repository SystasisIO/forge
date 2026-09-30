module;

#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.db.core.record;
import forge.db.object.exceptions;
import forge.db.object.index;
import forge.db.object.object;
import forge.db.object.transaction;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.plugins.db.store.api;

#include "details/p2p_state_schema.hxx"

namespace forge::plugins::net::p2p::node::detail {
namespace {

template <typename Object, typename Tag>
boost::asio::awaitable<bool> has_rows(forge::db::object::transaction& objects) {
   auto page = co_await objects.template index<Object, Tag>()
                   .lower_bound(typename Object::id_t{})
                   .page(forge::db::core::page_request{.limit = 1});
   co_return !page.items.empty();
}

template <typename Object, typename Tag>
boost::asio::awaitable<void> erase_all_rows(forge::db::object::transaction& objects) {
   auto next = std::optional<forge::db::core::cursor>{};
   while (true) {
      auto page = co_await objects.template index<Object, Tag>()
                      .lower_bound(typename Object::id_t{})
                      .page(forge::db::core::page_request{.after = next, .limit = forge::db::core::max_page_limit});
      for (const auto& row : page.items) {
         co_await objects.erase(row.id);
      }
      if (!page.next) {
         co_return;
      }
      next = std::move(page.next);
   }
}

boost::asio::awaitable<bool> has_private_cache_rows(forge::db::object::transaction& objects) {
   co_return co_await has_rows<p2p_state_schema::schema_state_object, p2p_state_schema::by_schema_state_id>(objects) ||
       co_await has_rows<p2p_state_schema::peer_object, p2p_state_schema::by_peer_row_id>(objects) ||
       co_await has_rows<p2p_state_schema::legacy_provider_v1_object, p2p_state_schema::by_legacy_provider_v1_row_id>(
           objects) ||
       co_await has_rows<p2p_state_schema::rendezvous_object, p2p_state_schema::by_rendezvous_row_id>(objects) ||
       co_await has_rows<p2p_state_schema::dht_value_object, p2p_state_schema::by_dht_value_row_id>(objects) ||
       co_await has_rows<p2p_state_schema::dht_provider_object, p2p_state_schema::by_dht_provider_row_id>(objects);
}

boost::asio::awaitable<void> erase_private_cache(forge::db::object::transaction& objects) {
   co_await erase_all_rows<p2p_state_schema::peer_object, p2p_state_schema::by_peer_row_id>(objects);
   co_await erase_all_rows<p2p_state_schema::legacy_provider_v1_object, p2p_state_schema::by_legacy_provider_v1_row_id>(
       objects);
   co_await erase_all_rows<p2p_state_schema::rendezvous_object, p2p_state_schema::by_rendezvous_row_id>(objects);
   co_await erase_all_rows<p2p_state_schema::dht_value_object, p2p_state_schema::by_dht_value_row_id>(objects);
   co_await erase_all_rows<p2p_state_schema::dht_provider_object, p2p_state_schema::by_dht_provider_row_id>(objects);
   co_await erase_all_rows<p2p_state_schema::schema_state_object, p2p_state_schema::by_schema_state_id>(objects);
}

[[noreturn]] void throw_incompatible(std::uint32_t actual) {
   FORGE_THROW_EXCEPTION(
       forge::db::object::exceptions::incompatible_version, "P2P state schema format version is incompatible",
       forge::exceptions::ctx("expected", p2p_state_schema::format_version), forge::exceptions::ctx("actual", actual));
}

} // namespace

void p2p_state_schema::register_objects(const forge::plugins::db::store::store_handle& store) {
   auto objects = store.objects();
   objects.register_object<schema_state_object>();
   objects.register_object<peer_object>();
   objects.register_object<legacy_provider_v1_object>();
   objects.register_object<rendezvous_object>();
   objects.register_object<dht_value_object>();
   objects.register_object<dht_provider_object>();
}

boost::asio::awaitable<void> p2p_state_schema::async_prepare(forge::plugins::db::store::api* db,
                                                             forge::plugins::db::store::store_handle store,
                                                             bool reset_incompatible_cache) {
   if (!db || !store) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::invalid_options,
                            "ObjectDB P2P state requires a named DB Store handle");
   }

   auto transaction = co_await store.begin_transaction();
   auto objects = co_await store.objects().join(transaction);
   auto state = co_await objects.find(schema_state_id);
   auto initialize = !state.has_value();
   if (state && state->format_version != format_version) {
      if (!reset_incompatible_cache) {
         throw_incompatible(state->format_version);
      }
      co_await erase_private_cache(objects);
      initialize = true;
   } else if (!state && co_await has_private_cache_rows(objects)) {
      if (!reset_incompatible_cache) {
         FORGE_THROW_EXCEPTION(forge::db::object::exceptions::incompatible_version,
                               "P2P state schema marker is missing from nonempty storage",
                               forge::exceptions::ctx("store", store.name()));
      }
      co_await erase_private_cache(objects);
      initialize = true;
   }
   if (initialize) {
      auto initial = schema_state{};
      initial.id = schema_state_id;
      co_await objects.insert(initial);
   }
   co_await transaction.commit();
   // A retry after an uncertain flush must confirm the already-visible marker too.
   co_await db->flush(store.name(), true);
}

} // namespace forge::plugins::net::p2p::node::detail
