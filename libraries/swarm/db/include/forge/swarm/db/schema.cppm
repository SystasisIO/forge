module;

#include <boost/describe.hpp>
#include <forge/db/object/macros.hpp>
#include <cstdint>

export module forge.swarm.db.schema;

export import forge.db.object.object;
export import forge.db.object.index;

export namespace forge::swarm::db {

inline constexpr std::uint32_t schema_version = 1;

struct schema_state : forge::db::object::object<schema_state, 81, 1> {
   std::uint32_t version = schema_version;

   bool operator==(const schema_state&) const = default;
};

struct by_schema_id;

using schema_index = forge::db::object::object_index<
   schema_state, forge::db::object::indexed_by<forge::db::object::primary_unique<by_schema_id>>>;

inline constexpr auto schema_id = schema_state::id_t{0};

BOOST_DESCRIBE_STRUCT(schema_state, (forge::db::object::object<schema_state, 81, 1>), (version))

} // namespace forge::swarm::db

export FORGE_DB_OBJECT(forge::swarm::db::schema_index)
