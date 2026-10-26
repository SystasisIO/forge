module;

#include <forge/exceptions/macros.hpp>
#include <concepts>
#include <typeindex>
#include <typeinfo>
#include <type_traits>

module forge.package.db.registration.catalog;

import forge.package.db.registration.model;
import forge.db.ids.object_id;
import forge.db.object.header;
import forge.db.object.index;
import forge.db.object.registration;
import forge.exceptions;

namespace package_registration {

template <typename T> concept registrable = requires {
   forge::db::object::registration::of<T>();
};

static_assert(registrable<item_index>);
static_assert(registrable<conflicting_index>);
static_assert(!registrable<int>);
static_assert(!registrable<item>);
static_assert(!registrable<forge::db::object::header_index>);
static_assert(!std::is_default_constructible_v<forge::db::object::registration>);
static_assert(!std::is_aggregate_v<forge::db::object::registration>);
static_assert(!std::is_constructible_v<forge::db::object::registration,
                                      forge::db::ids::object_id, std::type_index>);
static_assert(std::is_copy_constructible_v<forge::db::object::registration>);
static_assert(!std::is_copy_assignable_v<forge::db::object::registration>);

forge::db::object::registration metadata() {
   auto value = forge::db::object::registration::of<item_index>();
   FORGE_ASSERT(value.type() == forge::db::object::object_id_of<item_index>::value &&
                    value.model() == std::type_index{typeid(item_index)} &&
                    value.model() != std::type_index{typeid(item)},
                "Native registration lost its exact index type identity");
   return value;
}

forge::db::object::registration conflicting_metadata() {
   auto value = forge::db::object::registration::of<conflicting_index>();
   FORGE_ASSERT(value.type() == metadata().type() && value.model() != metadata().model(),
                "Conflicting fixture must retain the same ID and another native model");
   return value;
}

forge::db::object::registration legacy_metadata() {
   return forge::db::object::registration::of<legacy_index>();
}

} // namespace package_registration
