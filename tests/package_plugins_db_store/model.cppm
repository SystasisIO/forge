module;

#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <cstdint>
#include <string>

export module forge.package.db.registration.model;

import forge.db.object.index;
import forge.db.object.object;

export namespace package_registration {

struct item : forge::db::object::object<item, 1, 77> {
   std::string name;
   std::uint64_t bytes = 0;
};

struct conflicting_item : forge::db::object::object<conflicting_item, 1, 77> {
   std::string name;
};

struct legacy_item : forge::db::object::object<legacy_item, 1, 78> {
   std::string name;
};

struct by_id;
struct by_name;

using item_index = forge::db::object::object_index<
   item,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<by_name, forge::db::object::member<&item::name>>
   >
>;

using conflicting_index = forge::db::object::object_index<
   conflicting_item,
   forge::db::object::indexed_by<forge::db::object::primary_unique<by_id>>
>;

using legacy_index = forge::db::object::object_index<
   legacy_item,
   forge::db::object::indexed_by<forge::db::object::primary_unique<by_id>>
>;

BOOST_DESCRIBE_STRUCT(item, (forge::db::object::object<item, 1, 77>), (name, bytes))
BOOST_DESCRIBE_STRUCT(conflicting_item, (forge::db::object::object<conflicting_item, 1, 77>), (name))
BOOST_DESCRIBE_STRUCT(legacy_item, (forge::db::object::object<legacy_item, 1, 78>), (name))

} // namespace package_registration

FORGE_DB_OBJECT(package_registration::item_index)
FORGE_DB_OBJECT(package_registration::legacy_index)
